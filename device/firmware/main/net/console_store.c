/*
 * SPDX-License-Identifier: MIT
 *
 * console_store.c — SPIFFS mount + the console page served at "/".
 *
 * The console (device/console/dist/index.html) is baked into the "spiffs" partition at
 * build time (main/stage_console_html.cmake) and can be replaced at runtime with
 * POST /dev/html. GET / serves, in order: the uploaded/baked console (held in
 * PSRAM), else the small embedded fallback page (net/index.html).
 *
 * Durability rules (each one fixed a real field failure):
 *  - Writes go to a .tmp file in 32 KiB chunks with a yield between chunks (one
 *    multi-MB fwrite starved the watchdogs), then fsync + rename.
 *  - A size marker (/spiffs/index.size, decimal ASCII) is written LAST; at boot
 *    a page whose size does not match its marker is deleted, never served
 *    (a truncated upload once survived a panic and was served on the next boot).
 *  - If a persist fails, an NVS flag schedules a partition format for the next
 *    boot and the device reboots: formatting the 8 MB partition while Wi-Fi/USB/
 *    httpd are live trips the interrupt watchdog, so it only runs early in boot.
 *    The flag is cleared before formatting, so a crash cannot loop.
 *  /api/info reports "spiffs_state" (ok | formatted_this_boot | degraded) and
 *  "spiffs_html_recovered".
 */
#include "console_store.h"
#include "http.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "nvs.h"

static const char *TAG = "console";

#define SPIFFS_HTML_PATH      "/spiffs/index.html"
#define SPIFFS_HTML_TMP_PATH  "/spiffs/index.html.tmp"
#define SPIFFS_HTML_SIZE_PATH "/spiffs/index.size"
#define SPIFFS_PARTITION_LABEL "spiffs"

// Chunk size for the persist write loop (fix v3.a above). 32 KiB is small
// enough that one chunk's erase+program burst cannot plausibly run long
// enough on its own to starve the watchdogs, while still being large enough
// that a 2.68 MB upload is only ~86 chunks (not thousands of tiny writes).
#define SPIFFS_WRITE_CHUNK_BYTES (32 * 1024)

// SPIFFS health, reported by /api/info as "spiffs_state":
//   "ok"                  — mounted normally at boot
//   "formatted_this_boot" — recovery flag was set: partition was formatted at
//                           boot and mounted fresh (client should re-deploy)
//   "degraded"            — mount failed; /dev/html persistence unavailable
static const char *s_spiffs_state = "degraded";

// True once this boot has taken the format flag and attempted the format —
// regardless of the format's outcome. The /dev/html failure path uses this
// (not s_spiffs_state) as its anti-loop guard, so a boot whose format
// ATTEMPT failed also refuses to schedule another format+reboot.
static bool s_spiffs_format_attempted_this_boot = false;

// NVS flag scheduling a SPIFFS format for the next boot ("sys"/"spiffs_fmt").
#define SPIFFS_FMT_FLAG_NS  "sys"
#define SPIFFS_FMT_FLAG_KEY "spiffs_fmt"

static void nvs_set_spiffs_format_flag(void)
{
    nvs_handle_t h;
    if (nvs_open(SPIFFS_FMT_FLAG_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_u8(h, SPIFFS_FMT_FLAG_KEY, 1) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

// Reads AND CLEARS the format flag in one step (clear-before-format, so a
// crash or watchdog reset mid-format cannot loop). Returns true if a format
// was scheduled by the previous session.
static bool nvs_take_spiffs_format_flag(void)
{
    nvs_handle_t h;
    if (nvs_open(SPIFFS_FMT_FLAG_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    uint8_t v = 0;
    bool set = (nvs_get_u8(h, SPIFFS_FMT_FLAG_KEY, &v) == ESP_OK) && v != 0;
    if (set) {
        nvs_erase_key(h, SPIFFS_FMT_FLAG_KEY);
        nvs_commit(h);
    }
    nvs_close(h);
    return set;
}

// Detail string for the most recent persist failure (stage + errno/esp_err),
// surfaced verbatim in the /dev/html failure JSON so field failures
// self-describe without a debug probe. Written only by the httpd worker
// serving /dev/html (single writer) and read back in the same request.
static char s_spiffs_persist_err[128] = "";

// True if spiffs_load_html() (at boot) had to reject and delete a
// html+marker pair because they didn't agree — i.e. a previously-persisted
// upload was discarded as truncated/corrupt rather than served. Surfaced on
// /api/info as "spiffs_html_recovered" so this event is observable from
// outside instead of only ever showing up as an unexplained fallback to the
// embedded index.html.
static bool s_spiffs_html_recovered = false;

static esp_err_t spiffs_mount(void)
{
    esp_vfs_spiffs_conf_t cfg = {
        .base_path = "/spiffs",
        .partition_label = SPIFFS_PARTITION_LABEL,
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    return esp_vfs_spiffs_register(&cfg);
}

// Deletes the html file and its size marker together — used both by the
// public spiffs_delete_html() (POST /dev/reset_html) and internally whenever
// the load path finds the pair inconsistent and needs to make sure a
// truncated/corrupt file can't come back on a later boot either.
static void spiffs_delete_html_pair(void)
{
    remove(SPIFFS_HTML_PATH);
    remove(SPIFFS_HTML_SIZE_PATH);
}

// Returns malloc'd PSRAM buffer + length, or NULL if there's nothing valid to
// restore. "Valid" now REQUIRES an exact-size marker (SPIFFS_HTML_SIZE_PATH)
// that agrees with the real file's on-disk size — see the fix v3 note on the
// SPIFFS persistence block above. A file that exists but has no marker, or
// whose marker disagrees with its actual size, is presumed truncated/corrupt
// and is deleted rather than served (this is the fix for the bug where a
// panic-truncated file looked "self-consistent" enough to pass the old
// stat()-only check and got served as the console on the next boot).
static char *spiffs_load_html(size_t *out_len)
{
    FILE *mf = fopen(SPIFFS_HTML_SIZE_PATH, "rb");
    if (!mf) {
        // No marker: either nothing was ever persisted (common — first
        // boot), or a file was left by pre-fix firmware / an aborted write
        // that never got as far as writing the marker. Either way there is
        // no independent confirmation of size, which is exactly the gap
        // this fix closes, so refuse rather than trust the file alone.
        struct stat st;
        if (stat(SPIFFS_HTML_PATH, &st) == 0) {
            ESP_LOGW(TAG, "/dev/html SPIFFS restore: index.html present with no size marker — "
                          "discarding as unverifiable");
            spiffs_delete_html_pair();
            s_spiffs_html_recovered = true;
        }
        return NULL;
    }
    char digits[24] = {0};
    size_t dn = fread(digits, 1, sizeof(digits) - 1, mf);
    int rerr = ferror(mf);
    fclose(mf);
    char *end = NULL;
    unsigned long expected = (dn > 0 && !rerr) ? strtoul(digits, &end, 10) : 0;
    if (dn == 0 || rerr || end == digits) {
        ESP_LOGW(TAG, "/dev/html SPIFFS restore: size marker unreadable — discarding pair");
        spiffs_delete_html_pair();
        s_spiffs_html_recovered = true;
        return NULL;
    }

    struct stat st;
    if (stat(SPIFFS_HTML_PATH, &st) != 0 || st.st_size <= 0 || (unsigned long)st.st_size != expected) {
        ESP_LOGW(TAG, "/dev/html SPIFFS restore: size mismatch (marker=%lu on-disk=%ld) — "
                      "discarding as truncated/corrupt",
                 expected, (stat(SPIFFS_HTML_PATH, &st) == 0) ? (long)st.st_size : -1L);
        spiffs_delete_html_pair();
        s_spiffs_html_recovered = true;
        return NULL;
    }

    FILE *f = fopen(SPIFFS_HTML_PATH, "rb");
    if (!f) {
        ESP_LOGW(TAG, "/dev/html SPIFFS restore: fopen failed (errno=%d)", errno);
        return NULL;
    }
    char *buf = heap_caps_calloc(1, (size_t)st.st_size, MALLOC_CAP_SPIRAM);
    if (!buf) buf = calloc(1, (size_t)st.st_size);
    if (!buf) { fclose(f); ESP_LOGW(TAG, "/dev/html SPIFFS restore: OOM (%ld bytes)", (long)st.st_size); return NULL; }
    size_t n = fread(buf, 1, (size_t)st.st_size, f);
    int frerr = ferror(f);
    fclose(f);
    if (n != (size_t)st.st_size || frerr) {
        // The size marker matched stat() a moment ago but the read itself
        // came up short — treat exactly like any other mismatch: discard
        // the pair rather than hand back a partial page.
        ESP_LOGW(TAG, "/dev/html SPIFFS restore: short read (%u/%ld bytes, ferror=%d) — discarding",
                 (unsigned)n, (long)st.st_size, frerr);
        heap_caps_free(buf);
        spiffs_delete_html_pair();
        s_spiffs_html_recovered = true;
        return NULL;
    }
    *out_len = n;
    return buf;
}

// Writes `value` as decimal ASCII to SPIFFS_HTML_SIZE_PATH using the same
// write-tmp-verify-rename pattern as the html content itself (see
// spiffs_save_html below), so the marker is just as durability-checked as
// the file it describes. The payload is a handful of bytes, so there's no
// need to chunk this write — only the multi-MB html body needs that.
static bool spiffs_write_size_marker(size_t value)
{
    char digits[24];
    int dlen = snprintf(digits, sizeof(digits), "%u", (unsigned)value);
    if (dlen <= 0) return false;

    const char *tmp_path = SPIFFS_HTML_SIZE_PATH ".tmp";
    remove(tmp_path);
    FILE *f = fopen(tmp_path, "wb");
    if (!f) return false;

    size_t n = fwrite(digits, 1, (size_t)dlen, f);
    int werr = ferror(f);
    if (n == (size_t)dlen && !werr) fsync(fileno(f));
    int cerr = fclose(f);
    if (n != (size_t)dlen || werr || cerr != 0) {
        remove(tmp_path);
        return false;
    }

    struct stat st;
    if (stat(tmp_path, &st) != 0 || st.st_size != dlen) {
        remove(tmp_path);
        return false;
    }

    remove(SPIFFS_HTML_SIZE_PATH);   // SPIFFS_rename fails if the dest exists
    if (rename(tmp_path, SPIFFS_HTML_SIZE_PATH) != 0) {
        remove(tmp_path);
        return false;
    }
    return true;
}

// Persists `body` to SPIFFS. Every failure is surfaced (never silent):
//   - fopen()/fwrite()/fclose() errors are checked individually.
//   - The multi-MB body is written in SPIFFS_WRITE_CHUNK_BYTES chunks with a
//     vTaskDelay(1) between each — see the fix v3.a note on the SPIFFS
//     persistence block above. A single unbroken multi-MB fwrite() was the
//     root cause of an on-hardware panic (reset_reason "panic") under a
//     2.68 MB upload: it starved the watchdogs by keeping this httpd worker
//     running through dozens of SPIFFS erase/program cycles with no
//     scheduling point in between while wifi/tinyusb/httpd interrupt load
//     was simultaneously live.
//   - The body is written to a .tmp path first, fsync'd, and verified by
//     size BEFORE being promoted (rename()'d) over the real path — so a
//     write that fails partway through never touches the file the load path
//     would restore from. The exact-size marker (SPIFFS_HTML_SIZE_PATH) is
//     committed LAST, after the content rename, so a crash between the two
//     only ever costs the upload that was in flight, never a
//     previously-good persisted copy (the load path will simply see the OLD
//     marker not matching the NEW file and refuse it, falling back to
//     whatever was valid before).
//   - A short or failed write does NOT leave a truncated file behind at
//     either the real path or the .tmp path — both are remove()'d so the
//     next boot can't restore a corrupt half-upload.
//   - The write is verified after the fact by stat()-ing the file back and
//     confirming its on-disk size actually matches `len`, rather than
//     trusting the libc return codes alone (belt + suspenders against a
//     write that "succeeds" at the VFS layer but doesn't land, e.g. under
//     SPIFFS garbage-collection pressure).
// On failure, s_spiffs_persist_err holds a stage+errno description (fixed
// words + integers only — safe to embed in JSON unescaped).
// Returns ESP_OK only if `len` bytes are confirmed durable on flash AND the
// size marker was committed to match — i.e. only if the load path will
// actually serve this upload back on the next boot.
static esp_err_t spiffs_save_html(const char *body, size_t len)
{
    // Clean slate: a stale .tmp from an earlier crashed/aborted attempt must
    // never be mistaken for this attempt's output.
    remove(SPIFFS_HTML_TMP_PATH);

    FILE *f = fopen(SPIFFS_HTML_TMP_PATH, "wb");
    if (!f) {
        snprintf(s_spiffs_persist_err, sizeof(s_spiffs_persist_err),
                 "fopen tmp failed errno=%d", errno);
        ESP_LOGE(TAG, "/dev/html SPIFFS persist: %s", s_spiffs_persist_err);
        return ESP_FAIL;
    }

    size_t written = 0;
    bool short_write = false;
    while (written < len) {
        size_t remaining = len - written;
        size_t chunk = remaining < SPIFFS_WRITE_CHUNK_BYTES ? remaining : SPIFFS_WRITE_CHUNK_BYTES;
        size_t n = fwrite(body + written, 1, chunk, f);
        written += n;
        if (n != chunk) { short_write = true; break; }
        // Scheduling point between erase/program bursts — see the fix v3.a
        // note above. This is the actual panic fix: without it, a 2.68 MB
        // body was written in one uninterrupted fwrite() and starved the
        // watchdogs under live network interrupt load.
        vTaskDelay(1);
    }

    int werr = ferror(f);
    if (!short_write && !werr) fsync(fileno(f));
    int cerr = fclose(f);

    if (short_write || written != len || werr || cerr != 0) {
        snprintf(s_spiffs_persist_err, sizeof(s_spiffs_persist_err),
                 "write failed wrote=%u/%u ferror=%d fclose=%d errno=%d",
                 (unsigned)written, (unsigned)len, werr, cerr, errno);
        ESP_LOGE(TAG, "/dev/html SPIFFS persist: %s", s_spiffs_persist_err);
        remove(SPIFFS_HTML_TMP_PATH);   // don't leave a truncated tmp file behind
        return ESP_FAIL;
    }

    // Verify the tmp file's on-disk size BEFORE it's promoted — belt +
    // suspenders against a write that "succeeds" at the libc layer but
    // doesn't fully land (e.g. under SPIFFS garbage-collection pressure).
    struct stat st;
    if (stat(SPIFFS_HTML_TMP_PATH, &st) != 0 || (size_t)st.st_size != len) {
        snprintf(s_spiffs_persist_err, sizeof(s_spiffs_persist_err),
                 "tmp verify failed on-disk=%ld expected=%u errno=%d",
                 (stat(SPIFFS_HTML_TMP_PATH, &st) == 0) ? (long)st.st_size : -1L,
                 (unsigned)len, errno);
        ESP_LOGE(TAG, "/dev/html SPIFFS persist: %s", s_spiffs_persist_err);
        remove(SPIFFS_HTML_TMP_PATH);
        return ESP_FAIL;
    }

    // Promote: remove the previous real file first (SPIFFS_rename fails if
    // the destination already exists) then rename the verified tmp over it.
    // This is the commit point for CONTENT; the size marker commits last
    // (below) — see the big comment above for why that ordering matters.
    remove(SPIFFS_HTML_PATH);
    if (rename(SPIFFS_HTML_TMP_PATH, SPIFFS_HTML_PATH) != 0) {
        snprintf(s_spiffs_persist_err, sizeof(s_spiffs_persist_err),
                 "rename failed errno=%d", errno);
        ESP_LOGE(TAG, "/dev/html SPIFFS persist: %s", s_spiffs_persist_err);
        remove(SPIFFS_HTML_TMP_PATH);
        return ESP_FAIL;
    }

    // Commit the size marker LAST. If this fails or a crash lands right
    // here, the content on disk is fully valid but unmarked — the load path
    // requires the marker (see spiffs_load_html), so it would refuse and
    // delete this otherwise-good file on the next boot. Report that
    // honestly as a persist failure rather than claiming success for an
    // upload the load path won't actually honor.
    if (!spiffs_write_size_marker(len)) {
        snprintf(s_spiffs_persist_err, sizeof(s_spiffs_persist_err),
                 "size marker write failed errno=%d", errno);
        ESP_LOGE(TAG, "/dev/html SPIFFS persist: %s", s_spiffs_persist_err);
        spiffs_delete_html_pair();
        return ESP_FAIL;
    }

    s_spiffs_persist_err[0] = '\0';
    return ESP_OK;
}

static void spiffs_delete_html(void)
{
    spiffs_delete_html_pair();
    remove(SPIFFS_HTML_TMP_PATH);
    remove(SPIFFS_HTML_SIZE_PATH ".tmp");
}

// ── Embedded index.html ────────────────────────────────────────────────────────
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

// ── /dev/html PSRAM + SPIFFS override buffer ──────────────────────────────────
// POST /dev/html stores the uploaded HTML in PSRAM (fast serving) AND writes it
// to SPIFFS (/spiffs/index.html) so the override survives any reset — OTA
// reboots, power cycles, or crashes — without a re-push.  On boot, console_store_init()
// restores the SPIFFS copy into this buffer before the HTTP server starts.
// POST /dev/reset_html clears both the in-memory buffer and the SPIFFS file.
//
// A FreeRTOS mutex protects the pointer so a concurrent push doesn't race with
// a concurrent GET /.
#define DEV_HTML_MAX_BYTES (4 * 1024 * 1024)   // 4 MB cap (PSRAM-backed)
static char            *s_html_override      = NULL;
static size_t           s_html_override_len  = 0;
static SemaphoreHandle_t s_html_mutex        = NULL;

// ─────────────────────────────────────────────────────────────────────────────
// GET /
// If a /dev/html override has been posted, serve that; otherwise serve the
// embedded fallback. The override is restored from SPIFFS at boot (see
// console_store_init()), so it survives reboots as long as the POST /dev/html
// response reported bytes_persisted == bytes_received — see the SPIFFS
// persistence block above for how a failed persist is now reported loudly
// instead of silently falling back next boot.
// ─────────────────────────────────────────────────────────────────────────────
static esp_err_t handler_root(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_hdr(r, "Access-Control-Allow-Origin", "*");

    xSemaphoreTake(s_html_mutex, portMAX_DELAY);
    if (s_html_override && s_html_override_len > 0) {
        esp_err_t ret = httpd_resp_send(r, s_html_override, (ssize_t)s_html_override_len);
        xSemaphoreGive(s_html_mutex);
        return ret;
    }
    xSemaphoreGive(s_html_mutex);

    size_t len = (size_t)(index_html_end - index_html_start);
    return httpd_resp_send(r, (const char *)index_html_start, (ssize_t)len);
}

// ═════════════════════════════════════════════════════════════════════════════
// ── Dev / hot-swap endpoints ──────────────────────────────────────────────────
// ═════════════════════════════════════════════════════════════════════════════

// ── POST /dev/html ────────────────────────────────────────────────────────────
// Body = raw HTML bytes.  Written to PSRAM (fast serving) + SPIFFS
// (/spiffs/index.html, partition "spiffs" — see the sizing note on the SPIFFS
// persistence block above; ~8.4 MB, comfortably fits multi-MB uploads).
// GET / serves this override until POST /dev/reset_html.  Survives any reset —
// OTA, power cycle, crash — because SPIFFS is a separate flash partition that
// OTA never touches.  On boot, console_store_init() restores the SPIFFS copy before the
// HTTP server is registered.
//
// The response reports bytes_received (what the PSRAM override got, so GET /
// works right now regardless) vs bytes_persisted (what's actually durable on
// SPIFFS). "ok" reflects whether the upload SURVIVES A REBOOT, since that is
// this endpoint's whole point — a persist failure sets ok:false even though
// the in-memory override is live, so a deploy that silently wouldn't survive
// the next reset fails loudly here instead of only being discovered later.
//
// The OLD override is freed BEFORE the new body is received, so a re-deploy
// never holds two multi-MB copies in PSRAM at once. Until the new page lands,
// GET / serves the embedded fallback (or, on a failed upload, the SPIFFS copy
// restored by dev_html_restore_from_spiffs).
static void dev_html_restore_from_spiffs(void)
{
    size_t restored_len = 0;
    char *restored = spiffs_load_html(&restored_len);
    if (!restored) return;   // best-effort — embedded fallback still serves
    xSemaphoreTake(s_html_mutex, portMAX_DELAY);
    s_html_override     = restored;
    s_html_override_len = restored_len;
    xSemaphoreGive(s_html_mutex);
}

static esp_err_t handler_dev_html(httpd_req_t *r)
{
    int content_len = r->content_len;
    if (content_len <= 0 || content_len > DEV_HTML_MAX_BYTES) {
        return http_send_json(r, "{\"ok\":false,\"error\":\"content_too_large\"}");
    }

    // Free the OLD override BEFORE allocating the NEW one (see this
    // function's file header) — never hold two full-size copies at once.
    xSemaphoreTake(s_html_mutex, portMAX_DELAY);
    if (s_html_override) {
        heap_caps_free(s_html_override);
        s_html_override     = NULL;
        s_html_override_len = 0;
    }
    xSemaphoreGive(s_html_mutex);

    /* Allocate in PSRAM for large HTML payloads */
    char *new_buf = heap_caps_calloc(1, (size_t)content_len, MALLOC_CAP_SPIRAM);
    if (!new_buf) {
        /* Fall back to regular heap if PSRAM allocation fails */
        new_buf = calloc(1, (size_t)content_len);
        if (!new_buf) {
            // The old override is already gone — best-effort restore from
            // SPIFFS so GET / doesn't fall all the way back to the embedded
            // page until the next reboot (see dev_html_restore_from_spiffs).
            dev_html_restore_from_spiffs();
            return http_send_json(r, "{\"ok\":false,\"error\":\"oom\"}");
        }
    }

    int received = http_recv_full(r, new_buf, content_len);
    if (received <= 0) {
        free(new_buf);
        dev_html_restore_from_spiffs();
        return http_send_json(r, "{\"ok\":false,\"error\":\"recv_failed\"}");
    }

    xSemaphoreTake(s_html_mutex, portMAX_DELAY);
    s_html_override     = new_buf;
    s_html_override_len = (size_t)received;
    xSemaphoreGive(s_html_mutex);

    // Persist to SPIFFS so a reboot keeps the same UI. The PSRAM copy serves
    // requests fast; the SPIFFS copy is the durable backup. spiffs_save_html
    // checks every failure mode (fopen/fwrite/fclose + a post-write stat()
    // verify), so success here is a real durability signal, not an assumption.
    if (spiffs_save_html(new_buf, (size_t)received) == ESP_OK) {
        ESP_LOGI(TAG, "/dev/html: stored %d bytes (PSRAM + SPIFFS)", received);
        char resp[160];
        snprintf(resp, sizeof(resp),
                 "{\"ok\":true,\"bytes\":%d,\"bytes_received\":%d,\"bytes_persisted\":%d,\"persisted\":true}",
                 received, received, received);
        return http_send_json(r, resp);
    }

    // Persist failed. Recovery is a boot-time format (an inline format here
    // trips the interrupt watchdog — see the SPIFFS section note), UNLESS
    // this boot already attempted the format and writes STILL fail — then
    // another format+reboot would just loop, so report and stop. (Guard is
    // on the ATTEMPT, not on s_spiffs_state, so a boot whose format itself
    // errored also refuses to reschedule.)
    // error_detail: stage + errno of the underlying failure; fixed words +
    // integers only, so it embeds in JSON unescaped.
    char resp[448];
    if (s_spiffs_format_attempted_this_boot) {
        ESP_LOGE(TAG, "/dev/html: SPIFFS persist FAILED (%s) even after this boot's format — "
                       "NOT scheduling another format (would loop). PSRAM override is live but "
                       "will not survive a reboot.", s_spiffs_persist_err);
        snprintf(resp, sizeof(resp),
                 "{\"ok\":false,\"bytes\":%d,\"bytes_received\":%d,\"bytes_persisted\":0,"
                 "\"persisted\":false,\"error\":\"spiffs_persist_failed\","
                 "\"error_detail\":\"%s\","
                 "\"action\":\"partition was already formatted this boot and still fails; not rebooting again\"}",
                 received, received, s_spiffs_persist_err);
        return http_send_json(r, resp);
    }

    // Schedule the boot-time format (NVS flag) and reboot once the response
    // has flushed. The PSRAM override this request just installed is lost by
    // the reboot — deliberate: the client is told to re-deploy, and the
    // freshly-formatted filesystem will then persist it for real.
    nvs_set_spiffs_format_flag();
    ESP_LOGE(TAG, "/dev/html: SPIFFS persist FAILED (%s) — scheduled partition format for next "
                   "boot, rebooting", s_spiffs_persist_err);
    snprintf(resp, sizeof(resp),
             "{\"ok\":false,\"bytes\":%d,\"bytes_received\":%d,\"bytes_persisted\":0,"
             "\"persisted\":false,\"error\":\"spiffs_recovery_scheduled\","
             "\"error_detail\":\"%s\","
             "\"action\":\"device will format spiffs on next boot and is rebooting now; re-deploy after it comes back\"}",
             received, received, s_spiffs_persist_err);
    esp_err_t e = http_send_json(r, resp);
    http_reboot_soon("spiffs_rbt");
    return e;
}

// ── POST /dev/reset_html ──────────────────────────────────────────────────────
static esp_err_t handler_dev_reset_html(httpd_req_t *r)
{
    xSemaphoreTake(s_html_mutex, portMAX_DELAY);
    if (s_html_override) {
        heap_caps_free(s_html_override);
        s_html_override     = NULL;
        s_html_override_len = 0;
    }
    xSemaphoreGive(s_html_mutex);

    // Also remove the persistent copy.
    spiffs_delete_html();

    ESP_LOGI(TAG, "/dev/reset_html: reverted to embedded index.html");
    return http_send_json(r, "{\"ok\":true}");
}



/* ── boot + registration ─────────────────────────────────────────────────── */

void console_store_init(void)
{
    s_html_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_html_mutex ? ESP_OK : ESP_ERR_NO_MEM);

    /* Scheduled-format recovery runs here, before USB/Wi-Fi/httpd exist (see the
     * header). The flag is taken (cleared) before the format starts. */
    bool formatted_this_boot = false;
    if (nvs_take_spiffs_format_flag()) {
        s_spiffs_format_attempted_this_boot = true;   // anti-loop guard for /dev/html
        ESP_LOGW(TAG, "SPIFFS format scheduled by previous session — formatting partition '%s'",
                 SPIFFS_PARTITION_LABEL);
        esp_err_t ferr = esp_spiffs_format(SPIFFS_PARTITION_LABEL);
        formatted_this_boot = (ferr == ESP_OK);
        ESP_LOGI(TAG, "SPIFFS boot-time format: %s", esp_err_to_name(ferr));
    }

    esp_err_t spiffs_err = spiffs_mount();
    if (spiffs_err != ESP_OK) {
        // Not fatal (the embedded page still serves), but observable via /api/info.
        s_spiffs_state = "degraded";
        ESP_LOGE(TAG, "SPIFFS mount failed at boot: %s — /dev/html persistence degraded; "
                      "a failing POST /dev/html will schedule a format+reboot",
                 esp_err_to_name(spiffs_err));
        return;
    }
    s_spiffs_state = formatted_this_boot ? "formatted_this_boot" : "ok";
    size_t html_len = 0;
    char *html_buf = spiffs_load_html(&html_len);
    if (html_buf) {
        xSemaphoreTake(s_html_mutex, portMAX_DELAY);
        s_html_override     = html_buf;
        s_html_override_len = html_len;
        xSemaphoreGive(s_html_mutex);
        ESP_LOGI(TAG, "console restored from SPIFFS: %u bytes", (unsigned)html_len);
    }
}

const char *console_spiffs_state(void) { return s_spiffs_state; }
bool console_html_recovered(void) { return s_spiffs_html_recovered; }

esp_err_t console_http_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = handler_root},
        {.uri = "/dev/html", .method = HTTP_POST, .handler = handler_dev_html},
        {.uri = "/dev/reset_html", .method = HTTP_POST, .handler = handler_dev_reset_html},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t e = httpd_register_uri_handler(server, &routes[i]);
        if (e != ESP_OK) return e;
    }
    return ESP_OK;
}
