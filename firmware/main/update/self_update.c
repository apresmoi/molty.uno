/*
 * SPDX-License-Identifier: MIT
 *
 * self_update.c — the device glue of the firmware self-update (self_update.h): the
 * link command + its tap question, the console endpoints, the download task (HTTPS +
 * esp_ota_*), the screen, and the boot-time mark-valid guard. The rules live in
 * update_core.c (host-tested).
 *
 * Threading: s_lock guards the session (link task, httpd task, download task); events
 * are built under it and sent after it is released (a send may block ~2 s). One OTA
 * writer at a time across this and POST /api/ota (s_ota). The guard (s_rule, s_welcomed)
 * is evaluated under s_lock by app_main, the link task (welcome) and its esp_timer.
 */
#include "self_update.h"
#include "update_core.h"
#include "link_ask.h"
#include "link_client.h"
#include "link_cmd.h"
#include "http.h"
#include "molty_board.h"
#include "mole_creature.h"
#include "mole_fluid.h"
#include "mole_diag.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_tls_errors.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(UPD_IMAGE_HEAD == MOLTY_IMAGE_TAG_OFFSET + MOLTY_IMAGE_TAG_BYTES, "the board tag is inside the checked head");

static const char *TAG = "self_update";
#define DL_BUF     8192          /* PSRAM, as /api/ota */
#define DL_STACK   8192          /* the TLS handshake runs on this task */
#define NVS_NS     "selfupd"     /* stable across versions: the NEXT firmware reads it */
#define NVS_ARMED  "armed"

static upd_session_t s_s;
static _Atomic(SemaphoreHandle_t) s_lock_h;
static atomic_bool s_ota;
static upd_boot_t s_rule = UPD_BOOT_NOTHING;
static bool s_welcomed;
static esp_timer_handle_t s_guard_timer;

static void lock(void)
{
    SemaphoreHandle_t m = atomic_load(&s_lock_h);
    if (!m) {
        SemaphoreHandle_t fresh = xSemaphoreCreateMutex(), none = NULL;
        configASSERT(fresh);
        if (atomic_compare_exchange_strong(&s_lock_h, &none, fresh)) m = fresh;
        else { vSemaphoreDelete(fresh); m = none; }
    }
    xSemaphoreTake(m, portMAX_DELAY);
}
static void unlock(void) { xSemaphoreGive(atomic_load(&s_lock_h)); }

static void send_evt(cJSON *j)
{
    if (!j) return;
    if (link_client_connected()) link_send_json(j);   /* events go out only while the link is up */
    cJSON_Delete(j);
}

bool self_update_ota_claim(void) { bool idle = false; return atomic_compare_exchange_strong(&s_ota, &idle, true); }
void self_update_ota_release(void) { atomic_store(&s_ota, false); }

static void policy(upd_policy_t *p)
{
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    p->repo = CONFIG_MOLTY_RELEASES_REPO;
    p->board = molty_board_id();
    p->slot_size = slot ? (uint32_t)slot->size : 0;
}

/* ── screen: the reader holds one word ("update", "40%", "restart") ─────────── */

/* A screen claim (link_cmd.h), not a plain state set: the update owns the screen, so a
 * listen/say that ends during the download does not restore idle over its progress. */
static void screen_word(const char *w)
{
    if (mole_creature_select("fluid") != ESP_OK) return;
    mole_fluid_set_text(w, (int)strlen(w));
    link_cmd_screen_claim("{\"state\":\"text\",\"hold\":true}");
}

static void screen_home(void) { mole_creature_state_set("{\"state\":\"creature\"}"); }

/* ── the arm (update_core.h "the mark-valid rule") ─────────────────────────── */

static esp_err_t arm_set(const char *label)
{
    nvs_handle_t n;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &n);
    if (e != ESP_OK) return e;
    e = label ? nvs_set_str(n, NVS_ARMED, label) : nvs_erase_key(n, NVS_ARMED);
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    if (e == ESP_OK) e = nvs_commit(n);
    nvs_close(n);
    return e;
}

/*
 * Read once and erased: true when it names the running slot. A read error counts as
 * not armed (fails open, deliberately): it cannot be told from "never armed", and the
 * unarmed rule still keeps only an image that boots far enough to serve HTTP, so a
 * broken image still rolls back and the local console stays a way back. Failing
 * closed would roll back every console install on a device whose NVS is unreadable,
 * onto an old image with the same NVS.
 */
static bool arm_take(const esp_partition_t *running)
{
    nvs_handle_t n;
    if (nvs_open(NVS_NS, NVS_READWRITE, &n) != ESP_OK) return false;
    char label[20] = "";
    size_t len = sizeof(label);
    bool armed = nvs_get_str(n, NVS_ARMED, label, &len) == ESP_OK && running && !strcmp(label, running->label);
    if (nvs_erase_key(n, NVS_ARMED) == ESP_OK) nvs_commit(n);
    nvs_close(n);
    return armed;
}

/* ── the download's I/O (upd_io_t) ─────────────────────────────────────────── */

typedef struct {
    esp_http_client_handle_t http;
    const esp_partition_t *part;
    esp_ota_handle_t ota;
    bool began;
    mbedtls_sha256_context sha;
    int shown;                     /* the 10 % step on screen */
    char err[24];
} dl_t;
static dl_t s_dl;                  /* one download at a time (s_ota) */

static bool redirect(int st) { return st == 301 || st == 302 || st == 303 || st == 307 || st == 308; }

static const char *conn_error(dl_t *d)
{
    int code = 0, flags = 0;
    esp_http_client_get_and_clear_last_tls_error(d->http, &code, &flags);
    if (code == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME) return "dns";
    if (flags || code >= ESP_ERR_MBEDTLS_CERT_PARTLY_OK) return "tls";   /* certificate / handshake */
    return "connect";
}

static const char *io_open(void *ctx, const char *url, int64_t *len)
{
    dl_t *d = ctx;
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* verified TLS, also after the redirect */
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 3072,    /* the CDN's signed redirect URL (~1 KB) is the request line */
        .user_agent = "molty-firmware",
        .keep_alive_enable = false,
    };
    d->http = esp_http_client_init(&cfg);
    if (!d->http) return "oom";
    for (int hop = 0;; hop++) {
        if (esp_http_client_open(d->http, 0) != ESP_OK) return conn_error(d);
        if (esp_http_client_fetch_headers(d->http) < 0) return "read";
        int st = esp_http_client_get_status_code(d->http);
        if (redirect(st)) {
            /* github.com → its asset CDN: follow, https only */
            if (hop >= 5) return "redirects";
            esp_http_client_flush_response(d->http, NULL);
            if (esp_http_client_set_redirection(d->http) != ESP_OK) return "redirect";
            char u[16];
            if (esp_http_client_get_url(d->http, u, sizeof(u)) != ESP_OK || strncmp(u, "https://", 8)) return "redirect";
            continue;
        }
        if (st != 200) { snprintf(d->err, sizeof(d->err), "http_%d", st); return d->err; }
        int64_t cl = esp_http_client_is_chunked_response(d->http) ? -1 : esp_http_client_get_content_length(d->http);
        *len = cl > 0 ? cl : -1;
        return NULL;
    }
}

static int io_read(void *ctx, uint8_t *buf, int cap) { return esp_http_client_read(((dl_t *)ctx)->http, (char *)buf, cap); }

static void io_close(void *ctx)
{
    dl_t *d = ctx;
    if (!d->http) return;
    esp_http_client_close(d->http);
    esp_http_client_cleanup(d->http);
    d->http = NULL;
}

static const char *io_ota_begin(void *ctx)
{
    dl_t *d = ctx;
    d->part = esp_ota_get_next_update_partition(NULL);
    if (!d->part) return "begin";
    if (esp_ota_begin(d->part, OTA_WITH_SEQUENTIAL_WRITES, &d->ota) != ESP_OK) return "begin";
    d->began = true;
    return NULL;
}

static const char *io_ota_write(void *ctx, const uint8_t *buf, size_t n)
{
    return esp_ota_write(((dl_t *)ctx)->ota, buf, n) == ESP_OK ? NULL : "write";
}

static const char *io_ota_end(void *ctx)
{
    dl_t *d = ctx;
    d->began = false;   /* esp_ota_end frees the handle either way */
    return esp_ota_end(d->ota) == ESP_OK ? NULL : "verify";
}

static const char *io_ota_set_boot(void *ctx)
{
    dl_t *d = ctx;
    /* the new image must reach the relay too when this one did (update_core.h) */
    bool arm = upd_should_arm(link_client_connected());
    /*
     * An arm that can't be written installs unarmed (fails open, deliberately): the
     * image is then kept once its HTTP server is up, so one that does not boot still
     * rolls back; what is lost is only the "must reach the relay" check, and the local
     * console can reinstall. Refusing would block every self-update on a device whose
     * NVS is full or worn (the usual cause, and a lasting one) for a rarer risk.
     */
    if (arm_set(arm ? d->part->label : NULL) != ESP_OK && arm) {
        ESP_LOGW(TAG, "could not arm the welcome check: the new image is kept once its HTTP server is up");
        arm = false;
    }
    if (esp_ota_set_boot_partition(d->part) != ESP_OK) { arm_set(NULL); return "set_boot"; }
    mole_diag_event(false, "update.boot_set slot=%s armed=%d", d->part->label, arm);
    return NULL;
}

static void io_ota_abort(void *ctx)
{
    dl_t *d = ctx;
    if (d->began) esp_ota_abort(d->ota);
    d->began = false;
}

static void io_sha_start(void *ctx)
{
    dl_t *d = ctx;
    mbedtls_sha256_init(&d->sha);
    mbedtls_sha256_starts(&d->sha, 0);
}
static void io_sha_update(void *ctx, const uint8_t *buf, size_t n) { mbedtls_sha256_update(&((dl_t *)ctx)->sha, buf, n); }
static void io_sha_finish(void *ctx, uint8_t out[32])
{
    dl_t *d = ctx;
    mbedtls_sha256_finish(&d->sha, out);
    mbedtls_sha256_free(&d->sha);
}

static int io_board_check(void *ctx, const uint8_t *head, size_t len)
{
    (void)ctx;
    char other[64];
    int r = molty_board_image_check(head, len, other, sizeof(other));
    if (r == 0) mole_diag_event(true, "update.wrong_board image=%s device=%s", other, molty_board_dir());
    return r;
}

static void report(upd_state_t st, uint32_t done, uint32_t total, const char *error)
{
    cJSON *ev;
    lock();
    upd_step(&s_s, st, done, total, esp_timer_get_time(), error, &ev);
    unlock();
    send_evt(ev);
}

static void io_step(void *ctx, upd_state_t st, uint32_t done, uint32_t total)
{
    dl_t *d = ctx;
    report(st, done, total, NULL);
    if (st == UPD_FLASHING) { screen_word("install"); return; }
    vTaskDelay(1);   /* per chunk, as http_recv_full: a fast download never starves idle */
    int step = total ? (int)((uint64_t)done * 10 / total) * 10 : 0;
    if (step > d->shown) {
        char w[16];
        snprintf(w, sizeof(w), "%d%%", step);
        screen_word(w);
        d->shown = step;
    }
}

static const upd_io_t s_io = {
    .ctx = &s_dl,
    .open = io_open, .read = io_read, .close = io_close,
    .ota_begin = io_ota_begin, .ota_write = io_ota_write, .ota_end = io_ota_end,
    .ota_set_boot = io_ota_set_boot, .ota_abort = io_ota_abort,
    .sha_start = io_sha_start, .sha_update = io_sha_update, .sha_finish = io_sha_finish,
    .board_check = io_board_check, .step = io_step,
};

static void download_task(void *arg)
{
    (void)arg;
    upd_target_t t;
    lock(); t = s_s.t; unlock();
    memset(&s_dl, 0, sizeof(s_dl));
    mole_diag_event(false, "update.download v=%s size=%u", t.version, (unsigned)t.size);
    screen_word("update");
    uint8_t *buf = heap_caps_malloc(DL_BUF, MALLOC_CAP_SPIRAM);
    int64_t t0 = esp_timer_get_time();
    const char *e = buf ? upd_download(&t, &s_io, buf, DL_BUF) : "oom";
    heap_caps_free(buf);
    if (e) {
        mole_diag_event(true, "update.failed %s after %ums", e, (unsigned)((esp_timer_get_time() - t0) / 1000));
        report(UPD_FAILED, 0, 0, e);
        screen_home();
        self_update_ota_release();
        vTaskDelete(NULL);
        return;
    }
    mole_diag_event(false, "update.ready v=%s in %ums", t.version, (unsigned)((esp_timer_get_time() - t0) / 1000));
    screen_word("restart");
    report(UPD_REBOOTING, 0, 0, NULL);   /* sent (written to the socket) before the restart */
    vTaskDelay(pdMS_TO_TICKS(600));      /* let TCP flush it and the console poll see it */
    esp_restart();
}

/* accepted (session state): claim the slot and start the download */
static void start_download(void)
{
    if (!self_update_ota_claim()) { report(UPD_FAILED, 0, 0, "busy"); return; }   /* POST /api/ota is writing */
    if (xTaskCreate(download_task, "self_update", DL_STACK, NULL, 1, NULL) != pdPASS) {
        self_update_ota_release();
        report(UPD_FAILED, 0, 0, "oom");
    }
}

/* ── the link: `cmd update` and its question ───────────────────────────────── */

static void on_answer(int index, const char *reason)
{
    cJSON *ev;
    lock();
    bool go = upd_answer(&s_s, index, reason, &ev);
    unlock();
    mole_diag_event(false, "update.answer index=%d reason=%s", index, reason ? reason : "-");
    send_evt(ev);
    if (go) start_download();
}

esp_err_t self_update_cmd(const cJSON *args, cJSON **out)
{
    if (!molty_board_has(MOLTY_CAP_TOUCH)) return ESP_ERR_NOT_SUPPORTED;   /* nobody could confirm */
    upd_policy_t p;
    policy(&p);
    lock();
    const char *e = upd_request(&s_s, args, true, &p);
    upd_target_t t = s_s.t;
    unlock();
    if (e) {
        mole_diag_event(true, "update.refused %s", e);
        return !strcmp(e, "invalid_args") ? ESP_ERR_INVALID_ARG : link_cmd_refuse(e);
    }
    cJSON *result = cJSON_CreateObject();
    const char *labels[2] = { UPD_LABEL_UPDATE, UPD_LABEL_LATER };
    const int32_t colors[2] = { UPD_COLOR_UPDATE, UPD_COLOR_LATER };
    esp_err_t r = result ? link_ask_open(t.job, labels, colors, 2, t.timeout_ms, on_answer) : ESP_ERR_NO_MEM;
    if (r != ESP_OK) {
        lock();
        if (s_s.state == UPD_CONFIRM) s_s.state = UPD_IDLE;
        unlock();
        cJSON_Delete(result);
        return r;
    }
    cJSON_AddStringToObject(result, "job", t.job);
    *out = result;
    return ESP_OK;
}

/* ── the console: GET / POST /api/update ───────────────────────────────────── */

static esp_err_t send_status(httpd_req_t *r)
{
    lock();
    cJSON *j = upd_status_json(&s_s);
    unlock();
    char *body = j ? cJSON_PrintUnformatted(j) : NULL;
    cJSON_Delete(j);
    if (!body) return http_send_json(r, "{\"ok\":false,\"error\":\"oom\"}");
    esp_err_t e = http_send_json(r, body);
    free(body);
    return e;
}

static esp_err_t http_get(httpd_req_t *r) { return send_status(r); }

static esp_err_t http_post(httpd_req_t *r)
{
    if (r->content_len <= 0 || r->content_len > 1024) return http_send_json(r, "{\"ok\":false,\"error\":\"invalid_args\"}");
    char body[1025];
    int got = http_recv_full(r, body, r->content_len);
    if (got != r->content_len) return http_send_json(r, "{\"ok\":false,\"error\":\"recv\"}");
    body[got] = 0;
    cJSON *args = cJSON_ParseWithOpts(body, NULL, true);
    upd_policy_t p;
    policy(&p);
    lock();
    const char *e = args ? upd_request(&s_s, args, false, &p) : "invalid_args";
    if (!e && !self_update_ota_claim()) { s_s.state = UPD_IDLE; e = "busy"; }   /* POST /api/ota is writing */
    unlock();
    cJSON_Delete(args);
    if (e) {
        char b[64];
        snprintf(b, sizeof(b), "{\"ok\":false,\"error\":\"%s\"}", e);
        return http_send_json(r, b);
    }
    mole_diag_event(false, "update.console");
    if (xTaskCreate(download_task, "self_update", DL_STACK, NULL, 1, NULL) != pdPASS) {
        self_update_ota_release();
        report(UPD_FAILED, 0, 0, "oom");
    }
    return send_status(r);
}

esp_err_t self_update_http_register(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        {.uri = "/api/update", .method = HTTP_GET, .handler = http_get},
        {.uri = "/api/update", .method = HTTP_POST, .handler = http_post},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t e = httpd_register_uri_handler(server, &routes[i]);
        if (e != ESP_OK) return e;
    }
    return ESP_OK;
}

/* ── boot: the mark-valid guard ────────────────────────────────────────────── */

/*
 * The decision and its side effect stay under s_lock: upd_guard_step hands out one
 * decision per boot (it consumes s_rule), so of the welcome and the window's timer
 * exactly one marks valid OR rolls back; the other finds DONE and does nothing.
 */
static void guard_eval(void)
{
    lock();
    upd_guard_t g = upd_guard_step(&s_rule, s_welcomed, esp_timer_get_time());
    bool welcomed = s_welcomed;
    if (g != UPD_GUARD_WAIT && s_guard_timer) esp_timer_stop(s_guard_timer);
    if (g == UPD_GUARD_MARK_VALID) {
        esp_ota_mark_app_valid_cancel_rollback();
    } else if (g == UPD_GUARD_ROLLBACK) {
        mole_diag_event(true, "update.rollback: no relay welcome within %us", (unsigned)(UPD_WELCOME_WINDOW_US / 1000000));
        esp_ota_mark_app_invalid_rollback_and_reboot();
        /* only returns when there is no other app to go back to: keep this one */
        esp_ota_mark_app_valid_cancel_rollback();
    }
    unlock();
    if (g == UPD_GUARD_MARK_VALID) mole_diag_event(false, "update.valid (%s)", welcomed ? "relay welcome" : "http up");
}

static void guard_timer(void *arg) { (void)arg; guard_eval(); }

void self_update_welcomed(void)
{
    lock(); s_welcomed = true; unlock();
    guard_eval();
}

void self_update_boot(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    bool pending = esp_ota_get_state_partition(running, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
    bool armed = arm_take(running);
    upd_boot_t rule = upd_boot_rule(pending, armed);
    lock(); s_rule = rule; unlock();
    if (rule == UPD_BOOT_AWAIT_WELCOME) {
        ESP_LOGI(TAG, "new image: kept once the relay welcomes it (else rolled back in %us)", (unsigned)(UPD_WELCOME_WINDOW_US / 1000000));
        const esp_timer_create_args_t a = { .callback = guard_timer, .name = "upd_guard" };
        int64_t left = UPD_WELCOME_WINDOW_US - esp_timer_get_time();
        if (esp_timer_create(&a, &s_guard_timer) == ESP_OK)
            esp_timer_start_once(s_guard_timer, left > 0 ? (uint64_t)left : 1);
    }
    guard_eval();   /* not armed: valid now (HTTP is up); armed + already welcomed: valid now */
}
