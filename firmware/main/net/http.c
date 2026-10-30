/*
 * SPDX-License-Identifier: MIT
 * First-party; evolved from Espressif's tusb_ncm example (Unlicense OR CC0-1.0).
 *
 * http.c — the device HTTP API on port 80 (USB-C at 10.13.37.1, Wi-Fi at
 * molty.local). Route groups:
 *   console + /dev/html .......... net/console_store.c
 *   Wi-Fi setup .................. net/network.c
 *   agent link (/api/link) ....... link/link_client.c
 *   diagnostics (/api/diag, log) . diag/mole_diag.c
 *   everything else .............. here: audio, IMU, creature, reader text,
 *                                  framebuffer, info, OTA, coredump, bootloader.
 * Hardware goes through the board seam (board/molty_board.h): on a board without a
 * speaker / mic / IMU those endpoints answer {"ok":false,"error":"unsupported"}.
 * No auth: the API is for the local network. Remote agents go through the relay.
 */
#include "http.h"
#include "network.h"
#include "console_store.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"   /* esp_app_get_description()->version = VERSION */
#include "esp_core_dump.h"
#include "esp_partition.h"
#include "soc/rtc_cntl_reg.h"   /* RTC_CNTL_OPTION1_REG / FORCE_DOWNLOAD_BOOT */
#include "sdkconfig.h"
#include "cJSON.h"

#include "esp_psram.h"
#include "molty_board.h"
#include "mole_creature.h"
#include "mole_fluid.h"
#include "mole_diag.h"
#include "link_client.h"
#include "link_cmd.h"

static const char *TAG = "http";

// ── IMU WebSocket: handle + list of open WS fds ───────────────────────────────
// The WebSocket task uses httpd_ws_send_data_async to push frames without
// blocking the httpd worker threads.  We track the server handle so the task
// can look up active WS clients with httpd_get_client_list.
static httpd_handle_t    s_http_server       = NULL;
static volatile uint32_t s_imu_ws_rate_hz    = 10;   // default 10 Hz, clamp 1..50

// ─────────────────────────────────────────────────────────────────────────────
// JSON response helpers
// ─────────────────────────────────────────────────────────────────────────────
esp_err_t http_send_json(httpd_req_t *r, const char *json)
{
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_sendstr(r, json);
}

// Read the FULL request body. esp_http_server's httpd_req_recv returns one chunk
// per call (typically ~MSS-sized). For bodies larger than that — anything over
// a few hundred bytes — we must loop until we've consumed content_len bytes.
// Returns the number of bytes read, or <=0 on error (consistent with the
// underlying API). HTTPD_SOCK_ERR_TIMEOUT is retried transparently.
//
// The `access` attribute tells GCC the function writes `total_len` bytes into
// `buf`, which lets callers use the buffer after a successful return without
// `-Wmaybe-uninitialized` warnings (httpd_req_recv has the same hint internally).
__attribute__((access(write_only, 2, 3)))
int http_recv_full(httpd_req_t *r, char *buf, int total_len)
{
    int total = 0;
    while (total < total_len) {
        // ⚠ MUST be httpd_req_recv (the IDF API), not recv_full — an earlier
        // global find-replace of httpd_req_recv → recv_full made this infinitely
        // recursive, blowing the stack at ~92 chunks deep into a POST.
        int n = httpd_req_recv(r, buf + total, total_len - total);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            vTaskDelay(1);
            continue;
        }
        if (n <= 0) return n;
        total += n;
        // Yield each chunk so 100 KB+ POSTs don't starve the idle task on core 0.
        vTaskDelay(1);
    }
    return total;
}

// ─────────────────────────────────────────────────────────────────────────────
// Simple JSON parser helpers — pull ssid and password from a flat JSON object.
// We avoid cJSON to keep the stack small.
// ─────────────────────────────────────────────────────────────────────────────
bool http_json_get_str(const char *json, const char *key, char *out, size_t out_len)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ' ' || *p == ':' || *p == ' ') p++;
    if (*p != '"') return false;
    p++;  // skip opening quote
    size_t i = 0;
    while (*p && *p != '"' && i < out_len - 1) {
        if (*p == '\\' && *(p + 1)) { p++; }  // skip escape
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

// The board has no such hardware (molty_board_has): a clear, stable error.
static esp_err_t send_unsupported(httpd_req_t *r, const char *what)
{
    char buf[112];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"unsupported\",\"detail\":\"this board has no %s\"}", what);
    return http_send_json(r, buf);
}

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

void http_reboot_soon(const char *task_name)
{
    xTaskCreate(reboot_task, task_name, 2048, NULL, 5, NULL);
}

// ═════════════════════════════════════════════════════════════════════════════
// ── Audio endpoints ───────────────────────────────────────────────────────────
// All audio handlers are SYNCHRONOUS — the handler task blocks for the
// duration of playback/recording.  esp_http_server workers are per-request,
// so other requests remain served during this time.
// ═════════════════════════════════════════════════════════════════════════════

// Helper: parse a uint32 query param from the URI, returning def if absent.
// `httpd_query_key_value` wants a clean query string ("k=v&k=v"), not the
// full URI — passing r->uri silently returns "not found" for every key,
// which meant every ?ms=/?amp=/?rate= override we ever sent was ignored.
static uint32_t uri_param_u32(httpd_req_t *r, const char *key, uint32_t def)
{
    size_t qlen = httpd_req_get_url_query_len(r);
    if (qlen == 0) return def;
    char *qbuf = malloc(qlen + 1);
    if (!qbuf) return def;
    uint32_t out = def;
    if (httpd_req_get_url_query_str(r, qbuf, qlen + 1) == ESP_OK) {
        char val[32];
        if (httpd_query_key_value(qbuf, key, val, sizeof(val)) == ESP_OK) {
            long v = atol(val);
            if (v > 0) out = (uint32_t)v;
        }
    }
    free(qbuf);
    return out;
}

// ── GET /api/audio/tone?hz=440&ms=400&amp=8000 ────────────────────────────────
// Synthesise a sine wave at the given frequency and play it synchronously.
static esp_err_t handler_audio_tone(httpd_req_t *r)
{
    if (!molty_board_has(MOLTY_CAP_SPEAKER)) return send_unsupported(r, "speaker");
    uint32_t hz  = uri_param_u32(r, "hz",  440);
    uint32_t ms  = uri_param_u32(r, "ms",  400);
    // Default 28000 ≈ -1.4 dBFS — close to int16 max without clipping. Was
    // 8000 (≈ -12 dBFS) which sounded faint even at "max volume".
    int      amp = (int)uri_param_u32(r, "amp", 28000);
    if (amp > 32767) amp = 32767;

    const uint32_t RATE    = 16000;
    const uint32_t n       = (uint32_t)((uint64_t)RATE * ms / 1000);
    const float    dphase  = 2.0f * (float)M_PI * (float)hz / (float)RATE;

    // Clip buffers live in PSRAM: malloc never returns PSRAM on this build
    // (SPIRAM_USE_CAPS_ALLOC) and internal DRAM has ~95 KB free.
    int16_t *buf = heap_caps_malloc(n * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) return http_send_json(r, "{\"ok\":false,\"error\":\"oom\"}");

    float phase = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        buf[i] = (int16_t)((float)amp * sinf(phase));
        phase += dphase;
        if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
    }

    /* Play raw (amp_scale=32767 means "pass through as-is" since samples already
     * have the caller-specified amplitude baked in). */
    esp_err_t ret = molty_speaker_play(buf, n, RATE, 32767);
    free(buf);

    if (ret != ESP_OK) return http_send_json(r, "{\"ok\":false,\"error\":\"play_failed\"}");

    char resp[80];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"hz\":%lu,\"ms\":%lu}",
             (unsigned long)hz, (unsigned long)ms);
    return http_send_json(r, resp);
}

// ── POST /api/audio/play?rate=16000 ──────────────────────────────────────────
// Body = raw int16 PCM mono.  Play synchronously.
static esp_err_t handler_audio_play(httpd_req_t *r)
{
    if (!molty_board_has(MOLTY_CAP_SPEAKER)) return send_unsupported(r, "speaker");
    uint32_t rate = uri_param_u32(r, "rate", 16000);

    int content_len = r->content_len;
    // Cap at 4 MB — that's ~2 minutes of 16 kHz mono PCM. Playback is currently
    // synchronous (blocks the HTTP worker), so a multi-minute clip looks like
    // the device hung. Future: streaming + cancel endpoint.
    if (content_len <= 0 || content_len > 4 * 1024 * 1024) {
        return http_send_json(r, "{\"ok\":false,\"error\":\"too_large_max_4mb\"}");
    }

    // PSRAM, see handler_audio_tone. calloc so GCC can prove it's initialized — recv_full fills it
    int16_t *buf = heap_caps_calloc(1, (size_t)content_len, MALLOC_CAP_SPIRAM);
    if (!buf) return http_send_json(r, "{\"ok\":false,\"error\":\"oom\"}");

    int received = http_recv_full(r, (char *)buf, content_len);
    if (received <= 0) {
        free(buf);
        return http_send_json(r, "{\"ok\":false,\"error\":\"recv_failed\"}");
    }

    size_t n_samples = (size_t)received / sizeof(int16_t);
    esp_err_t ret = molty_speaker_play(buf, n_samples, rate, 32767);
    free(buf);

    if (ret != ESP_OK) return http_send_json(r, "{\"ok\":false,\"error\":\"play_failed\"}");

    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"samples\":%zu}", n_samples);
    return http_send_json(r, resp);
}

// ── POST /api/audio/play_wav ──────────────────────────────────────────────────
// Body = standard RIFF/WAVE/PCM file.  Parse header, play data chunk.
// Rejects anything that is not int16 mono PCM.
static esp_err_t handler_audio_play_wav(httpd_req_t *r)
{
    if (!molty_board_has(MOLTY_CAP_SPEAKER)) return send_unsupported(r, "speaker");
    int content_len = r->content_len;
    // Cap at 4 MB — same reasoning as /api/audio/play. WAVs over a few MB
    // imply >1 min of playback during which the HTTP server is unresponsive.
    if (content_len < 44 || content_len > 4 * 1024 * 1024) {
        return http_send_json(r, "{\"ok\":false,\"error\":\"too_large_max_4mb\"}");
    }

    // PSRAM, see handler_audio_tone. calloc so GCC can prove it's initialized
    uint8_t *wav = heap_caps_calloc(1, (size_t)content_len, MALLOC_CAP_SPIRAM);
    if (!wav) return http_send_json(r, "{\"ok\":false,\"error\":\"oom\"}");

    int received = http_recv_full(r, (char *)wav, content_len);
    if (received < 44) {
        free(wav);
        return http_send_json(r, "{\"ok\":false,\"error\":\"recv_failed\"}");
    }

    /* Parse RIFF/WAVE header (little-endian, standard offsets) */
    if (memcmp(wav,      "RIFF", 4) != 0 ||
        memcmp(wav + 8,  "WAVE", 4) != 0 ||
        memcmp(wav + 12, "fmt ", 4) != 0) {
        free(wav);
        return http_send_json(r, "{\"ok\":false,\"error\":\"not_wav\"}");
    }
    uint16_t audio_fmt  = (uint16_t)(wav[20] | (wav[21] << 8));
    uint16_t channels   = (uint16_t)(wav[22] | (wav[23] << 8));
    uint32_t sample_rate= (uint32_t)(wav[24] | (wav[25]<<8) | (wav[26]<<16) | (wav[27]<<24));
    uint16_t bits_ps    = (uint16_t)(wav[34] | (wav[35] << 8));

    if (audio_fmt != 1 || channels != 1 || bits_ps != 16) {
        free(wav);
        return http_send_json(r, "{\"ok\":false,\"error\":\"must_be_int16_mono_pcm\"}");
    }

    /* Find "data" chunk (may not be at byte 36 if there are extra fmt bytes) */
    int data_offset = -1;
    for (int i = 12; i + 8 <= received; ) {
        if (memcmp(wav + i, "data", 4) == 0) {
            data_offset = i + 8;
            break;
        }
        uint32_t chunk_size = (uint32_t)(wav[i+4] | (wav[i+5]<<8) |
                                          (wav[i+6]<<16) | (wav[i+7]<<24));
        i += 8 + (int)chunk_size;
    }
    if (data_offset < 0 || data_offset >= received) {
        free(wav);
        return http_send_json(r, "{\"ok\":false,\"error\":\"no_data_chunk\"}");
    }

    size_t n_samples = (size_t)(received - data_offset) / sizeof(int16_t);
    uint32_t dur_ms  = (uint32_t)((uint64_t)n_samples * 1000 / sample_rate);

    esp_err_t ret = molty_speaker_play(
        (const int16_t *)(wav + data_offset), n_samples, sample_rate, 32767);
    free(wav);

    if (ret != ESP_OK) return http_send_json(r, "{\"ok\":false,\"error\":\"play_failed\"}");

    char resp[80];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"rate\":%lu,\"ms\":%lu}",
             (unsigned long)sample_rate, (unsigned long)dur_ms);
    return http_send_json(r, resp);
}

// ── GET /api/audio/record?ms=2000&rate=16000 ─────────────────────────────────
// Record from PDM mic for N ms.  Returns audio/wav with a standard WAV header.
static esp_err_t handler_audio_record(httpd_req_t *r)
{
    if (!molty_board_has(MOLTY_CAP_MIC)) return send_unsupported(r, "microphone");
    // Refuse if the mic streaming WS is active — one consumer at a time.
    if (molty_mic_stream_active()) {
        return http_send_json(r, "{\"ok\":false,\"error\":\"mic_busy\"}");
    }

    uint32_t ms   = uri_param_u32(r, "ms",   2000);
    uint32_t rate = uri_param_u32(r, "rate", 16000);
    if (ms > 10000) ms = 10000;

    int16_t *pcm = NULL;
    size_t n_samples = 0;
    esp_err_t ret = molty_mic_record(rate, ms, &pcm, &n_samples);
    if (ret != ESP_OK || !pcm) {
        // Surface the actual esp_err_t code + symbolic name so we can diagnose
        // without serial logs (CONFIG_ESP_CONSOLE_NONE is set on this firmware).
        char buf[160];
        snprintf(buf, sizeof(buf),
            "{\"ok\":false,\"error\":\"record_failed\","
            "\"code\":%d,\"code_hex\":\"0x%x\",\"name\":\"%s\","
            "\"samples\":%zu}",
            (int)ret, (unsigned)ret, esp_err_to_name(ret), n_samples);
        return http_send_json(r, buf);
    }

    /* Build a WAV header (44 bytes) inline */
    uint32_t data_bytes = (uint32_t)(n_samples * sizeof(int16_t));
    uint8_t hdr[44];
    uint32_t riff_size = 36 + data_bytes;
    uint32_t byte_rate = rate * 2;          // 1 channel, 2 bytes/sample
    uint16_t block_align = 2;
    memcpy(hdr,      "RIFF", 4);
    hdr[4]  = (uint8_t)(riff_size);
    hdr[5]  = (uint8_t)(riff_size >> 8);
    hdr[6]  = (uint8_t)(riff_size >> 16);
    hdr[7]  = (uint8_t)(riff_size >> 24);
    memcpy(hdr + 8,  "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    hdr[16] = 16; hdr[17] = 0; hdr[18] = 0; hdr[19] = 0;  // fmt chunk size = 16
    hdr[20] = 1;  hdr[21] = 0;                              // PCM
    hdr[22] = 1;  hdr[23] = 0;                              // mono
    hdr[24] = (uint8_t)(rate);       hdr[25] = (uint8_t)(rate >> 8);
    hdr[26] = (uint8_t)(rate >> 16); hdr[27] = (uint8_t)(rate >> 24);
    hdr[28] = (uint8_t)(byte_rate);       hdr[29] = (uint8_t)(byte_rate >> 8);
    hdr[30] = (uint8_t)(byte_rate >> 16); hdr[31] = (uint8_t)(byte_rate >> 24);
    hdr[32] = (uint8_t)(block_align); hdr[33] = 0;          // block align
    hdr[34] = 16; hdr[35] = 0;                              // bits per sample
    memcpy(hdr + 36, "data", 4);
    hdr[40] = (uint8_t)(data_bytes);       hdr[41] = (uint8_t)(data_bytes >> 8);
    hdr[42] = (uint8_t)(data_bytes >> 16); hdr[43] = (uint8_t)(data_bytes >> 24);

    httpd_resp_set_type(r, "audio/wav");
    httpd_resp_set_hdr(r, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");

    /* Send in two chunks: header then PCM data */
    httpd_resp_send_chunk(r, (const char *)hdr, 44);
    httpd_resp_send_chunk(r, (const char *)pcm, (ssize_t)data_bytes);
    httpd_resp_send_chunk(r, NULL, 0);   // EOF
    free(pcm);
    return ESP_OK;
}

// ── POST /api/audio/volume ────────────────────────────────────────────────────
// Body: {"volume": N} where N is 0..21.  Accepts raw integer string too.
static esp_err_t handler_audio_volume(httpd_req_t *r)
{
    if (!molty_board_has(MOLTY_CAP_SPEAKER)) return send_unsupported(r, "speaker");
    int content_len = r->content_len;
    if (content_len <= 0 || content_len > 64) {
        return http_send_json(r, "{\"ok\":false,\"error\":\"bad_request\"}");
    }
    char body[65];
    int received = http_recv_full(r, body, content_len);
    if (received <= 0) {
        return http_send_json(r, "{\"ok\":false,\"error\":\"recv_failed\"}");
    }
    body[received] = '\0';

    /* Try JSON first: {"volume":N} */
    char vol_str[8] = {};
    int vol = -1;
    if (http_json_get_str(body, "volume", vol_str, sizeof(vol_str))) {
        vol = atoi(vol_str);
    } else {
        /* Plain integer */
        vol = atoi(body);
    }

    if (vol < 0 || vol > 21) {
        return http_send_json(r, "{\"ok\":false,\"error\":\"volume_out_of_range\"}");
    }

    molty_speaker_set_volume((uint8_t)vol);
    return http_send_json(r, "{\"ok\":true}");
}

// ═════════════════════════════════════════════════════════════════════════════
// ── IMU endpoints ─────────────────────────────────────────────────────────────
// ═════════════════════════════════════════════════════════════════════════════

// ── GET /api/imu/snapshot ─────────────────────────────────────────────────────
static esp_err_t handler_imu_snapshot(httpd_req_t *r)
{
    molty_imu_sample_t s;
    if (!molty_imu_get(&s)) return send_unsupported(r, "IMU");

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"ok\":true,"
        "\"ax\":%.4f,\"ay\":%.4f,\"az\":%.4f,"
        "\"gx\":%.3f,\"gy\":%.3f,\"gz\":%.3f,"
        "\"ms\":%lu}",
        s.ax, s.ay, s.az,
        s.gx, s.gy, s.gz,
        (unsigned long)s.ms);
    return http_send_json(r, buf);
}

// ── GET /ws/imu (WebSocket upgrade) ──────────────────────────────────────────
// After upgrade, streams IMU JSON at s_imu_ws_rate_hz.
// Accepts {"type":"set_rate","hz":N} from client (1..50 Hz).
//
// Implementation note: esp_http_server's WebSocket support requires
// CONFIG_HTTPD_WS_SUPPORT=y (added to sdkconfig.defaults).  The handler is
// called first for the upgrade handshake (is_websocket=false in httpd_req_t),
// then for each incoming frame.  We detect the type and either do the handshake
// or process the client message.  Outbound streaming is done by the separate
// imu_ws_task which wakes periodically and broadcasts to the fds that
// upgraded on THIS route (marker-tracked, see below).
/* Per-session subscription marker for /ws/imu: each WS route tracks its own
 * clients, so IMU JSON never leaks into /ws/audio/mic. /ws/imu stores this
 * marker's address in the session context at upgrade time and the broadcaster
 * sends ONLY to marked sessions. sess_ctx is per-socket and cleared by the
 * server when the session closes, so this is immune to fd-number reuse. The
 * no-op free_ctx is REQUIRED: with free_ctx == NULL, httpd would call free()
 * on the static marker at session close and corrupt the heap. */
static const int s_imu_ws_marker = 0;
static void imu_ws_marker_free_noop(void *ctx) { (void)ctx; }

static esp_err_t handler_ws_imu(httpd_req_t *r)
{
    if (r->method == HTTP_GET) {
        /* No IMU on this board: refuse the upgrade (nothing would ever stream). */
        if (!molty_board_has(MOLTY_CAP_IMU)) return ESP_ERR_NOT_SUPPORTED;
        /* HTTP GET — this is the WebSocket upgrade request. Mark this session
         * as an IMU subscriber (see s_imu_ws_marker above), then return
         * ESP_OK to let the server complete the handshake. */
        ESP_LOGI(TAG, "WS /ws/imu upgrade");
        r->sess_ctx = (void *)&s_imu_ws_marker;
        r->free_ctx = imu_ws_marker_free_noop;
        return ESP_OK;
    }

    /* Incoming WebSocket frame from an already-connected client */
    httpd_ws_frame_t pkt = {};
    pkt.type = HTTPD_WS_TYPE_TEXT;

    /* Get frame length first */
    esp_err_t ret = httpd_ws_recv_frame(r, &pkt, 0);
    if (ret != ESP_OK) return ret;

    if (pkt.len == 0) return ESP_OK;

    uint8_t *payload = malloc(pkt.len + 1);
    if (!payload) return ESP_ERR_NO_MEM;
    pkt.payload = payload;

    ret = httpd_ws_recv_frame(r, &pkt, pkt.len);
    if (ret == ESP_OK) {
        payload[pkt.len] = '\0';
        /* Accept {"type":"set_rate","hz":N} */
        char hz_str[8] = {};
        if (http_json_get_str((char *)payload, "hz", hz_str, sizeof(hz_str))) {
            int hz = atoi(hz_str);
            if (hz < 1)  hz = 1;
            if (hz > 50) hz = 50;
            s_imu_ws_rate_hz = (uint32_t)hz;
            ESP_LOGI(TAG, "WS IMU rate set to %d Hz", hz);
        }
    }
    free(payload);
    return ret;
}

// ── Background task: broadcast IMU frames to /ws/imu subscribers only ────────
static void imu_ws_task(void *arg)
{
    /* TWDT subscribe, so a silent hang panics and leaves a coredump.
     * This task is created exactly once at boot (app_main step 9) and never
     * deleted, so a single subscribe is enough — no matching delete needed. */
    esp_err_t wdt_err = esp_task_wdt_add(NULL);
    if (wdt_err != ESP_OK) {
        ESP_LOGW(TAG, "imu_ws: esp_task_wdt_add failed (%s) — no watchdog coverage",
                 esp_err_to_name(wdt_err));
    }

    while (1) {
        uint32_t hz = s_imu_ws_rate_hz;
        if (hz < 1) hz = 1;
        vTaskDelay(pdMS_TO_TICKS(1000 / hz));
        esp_task_wdt_reset();   /* feed once per iteration, before the `continue` below —
                                 * worst-case gap is the 1 s period at hz=1, well inside 10 s */

        if (!s_http_server) continue;

        molty_imu_sample_t s;
        molty_imu_get(&s);

        char json[256];
        int jlen = snprintf(json, sizeof(json),
            "{\"type\":\"imu\",\"t\":%lu,"
            "\"ax\":%.4f,\"ay\":%.4f,\"az\":%.4f,"
            "\"gx\":%.3f,\"gy\":%.3f,\"gz\":%.3f}",
            (unsigned long)s.ms,
            s.ax, s.ay, s.az,
            s.gx, s.gy, s.gz);

        /* Send ONLY to sessions that upgraded on /ws/imu (marked via
         * s_imu_ws_marker in sess_ctx), never to /ws/audio/mic clients. */
        size_t max_fds = 8;
        int fds[8];
        esp_err_t ret = httpd_get_client_list(s_http_server, &max_fds, fds);
        if (ret != ESP_OK) continue;

        httpd_ws_frame_t frame = {
            .final   = true,
            .fragmented = false,
            .type    = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)json,
            .len     = (size_t)jlen,
        };

        for (size_t i = 0; i < max_fds; i++) {
            if (httpd_ws_get_fd_info(s_http_server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET &&
                httpd_sess_get_ctx(s_http_server, fds[i]) == (void *)&s_imu_ws_marker) {
                httpd_ws_send_data(s_http_server, fds[i], &frame);
            }
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// ── /ws/audio/mic WebSocket ───────────────────────────────────────────────────
//
// Protocol:
//   Client → server (TEXT JSON):
//     {"cmd":"start","rate":16000,"chunk_ms":200,"gain":8}  — start streaming
//     {"cmd":"stop"}                                         — stop streaming
//   Server → client:
//     After start: TEXT JSON {"type":"started","rate":N,"chunk_ms":N,
//                              "chunk_samples":N,"gain":N}
//     Then: BINARY frames, each chunk_samples * 2 bytes of int16 mono PCM LE
//     On error: TEXT JSON {"type":"error","error":"<reason>"}
//
// One streaming task is spawned per WS connection (pinned to core 0, 4 KB
// stack).  Only one mic consumer at a time — concurrent connections or a live
// /api/audio/record get a mic_busy error.
// ═════════════════════════════════════════════════════════════════════════════

typedef struct {
    httpd_handle_t server;
    int            client_fd;
    uint32_t       rate;
    uint32_t       chunk_ms;
    uint8_t        gain;
} mic_stream_task_arg_t;

static void mic_stream_task(void *arg)
{
    mic_stream_task_arg_t *a = (mic_stream_task_arg_t *)arg;
    httpd_handle_t server    = a->server;
    int            fd        = a->client_fd;
    uint32_t       rate      = a->rate;
    uint32_t       chunk_ms  = a->chunk_ms;
    uint8_t        gain      = a->gain;
    free(a);

    // ── Start the mic stream ──────────────────────────────────────────────────
    esp_err_t ret = molty_mic_stream_start(rate, chunk_ms, gain);
    if (ret != ESP_OK) {
        // Send error frame then bail
        char err[128];
        if (ret == ESP_ERR_NOT_SUPPORTED)
            snprintf(err, sizeof(err), "{\"type\":\"error\",\"error\":\"unsupported\"}");
        else snprintf(err, sizeof(err),
            "{\"type\":\"error\",\"error\":\"start_stream_failed\",\"code\":%d}",
            (int)ret);
        httpd_ws_frame_t ef = {
            .final = true, .fragmented = false,
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)err, .len = strlen(err),
        };
        httpd_ws_send_data(server, fd, &ef);
        vTaskDelete(NULL);
        return;
    }

    // Retrieve the actual chunk_samples from the stream (may differ slightly
    // from requested due to integer rounding).
    // chunk_samples = rate * chunk_ms / 1000 — compute the same way as rx_start_stream.
    size_t chunk_samples = (size_t)((uint64_t)rate * chunk_ms / 1000);

    // ── Send "started" confirmation ───────────────────────────────────────────
    char started[128];
    int slen = snprintf(started, sizeof(started),
        "{\"type\":\"started\",\"rate\":%lu,\"chunk_ms\":%lu,"
        "\"chunk_samples\":%zu,\"gain\":%u}",
        (unsigned long)rate, (unsigned long)chunk_ms, chunk_samples, (unsigned)gain);
    httpd_ws_frame_t sf = {
        .final = true, .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)started, .len = (size_t)slen,
    };
    ret = httpd_ws_send_data(server, fd, &sf);
    if (ret != ESP_OK) goto cleanup;

    // ── Allocate mono output buffer ───────────────────────────────────────────
    int16_t *mono = malloc(chunk_samples * sizeof(int16_t));
    if (!mono) {
        char err[80];
        snprintf(err, sizeof(err), "{\"type\":\"error\",\"error\":\"oom\"}");
        httpd_ws_frame_t ef2 = {
            .final = true, .fragmented = false,
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)err, .len = strlen(err),
        };
        httpd_ws_send_data(server, fd, &ef2);
        goto cleanup;
    }

    // ── Streaming loop ────────────────────────────────────────────────────────
    // i2s_channel_read inside the board's mic stream_read blocks for ~chunk_ms,
    // which naturally paces the loop.  We exit if:
    //   - the stream is flagged inactive (stop command arrived via the handler)
    //   - the send fails (client disconnected)
    while (molty_mic_stream_active()) {
        size_t got = 0;
        ret = molty_mic_stream_read(mono, chunk_samples, &got);
        if (ret != ESP_OK) break;

        httpd_ws_frame_t bf = {
            .final = true, .fragmented = false,
            .type = HTTPD_WS_TYPE_BINARY,
            .payload = (uint8_t *)mono,
            .len = got * sizeof(int16_t),
        };
        ret = httpd_ws_send_data(server, fd, &bf);
        if (ret != ESP_OK) break;

        // Yield briefly to feed the WDT and let other tasks run. The blocking
        // i2s_channel_read already yields, so this is just a safety valve.
        vTaskDelay(1);
    }

    free(mono);

cleanup:
    molty_mic_stream_stop();
    vTaskDelete(NULL);
}

// ── WebSocket handler for /ws/audio/mic ──────────────────────────────────────
// Called on upgrade (HTTP_GET) and on each incoming client frame.
static esp_err_t handler_ws_audio_mic(httpd_req_t *r)
{
    if (r->method == HTTP_GET) {
        // WebSocket upgrade request.
        // Reject immediately if mic is already in use.
        if (molty_mic_stream_active()) {
            // We must complete the WS handshake before we can send a WS frame,
            // but esp_http_server doesn't provide a "reject upgrade" path that
            // still sends a WS close frame cleanly. Instead, return ESP_OK to
            // allow the upgrade, then the next incoming frame handler will see
            // the busy state and close. We set a flag so the cmd handler below
            // can reject the first start command.
            // (This path only fires if a second browser tab connects while one
            // is streaming — an edge case in lab use.)
        }
        ESP_LOGI(TAG, "WS /ws/audio/mic upgrade");
        return ESP_OK;
    }

    // ── Incoming frame from client ────────────────────────────────────────────
    httpd_ws_frame_t pkt = {};
    pkt.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t ret = httpd_ws_recv_frame(r, &pkt, 0);
    if (ret != ESP_OK) return ret;
    if (pkt.len == 0) return ESP_OK;

    uint8_t *payload = malloc(pkt.len + 1);
    if (!payload) return ESP_ERR_NO_MEM;
    pkt.payload = payload;
    ret = httpd_ws_recv_frame(r, &pkt, pkt.len);
    if (ret != ESP_OK) { free(payload); return ret; }
    payload[pkt.len] = '\0';

    // Parse "cmd"
    char cmd[16] = {};
    http_json_get_str((char *)payload, "cmd", cmd, sizeof(cmd));

    if (strcmp(cmd, "start") == 0) {
        // Reject if mic is already busy
        if (molty_mic_stream_active()) {
            const char *err = "{\"type\":\"error\",\"error\":\"mic_busy\"}";
            httpd_ws_frame_t ef = {
                .final = true, .fragmented = false,
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t *)err, .len = strlen(err),
            };
            httpd_ws_send_frame(r, &ef);
            free(payload);
            return ESP_OK;
        }

        // Parse parameters with defaults + clamp
        char val[16] = {};
        uint32_t rate      = 16000;
        uint32_t chunk_ms  = 200;
        uint8_t  gain      = 8;

        if (http_json_get_str((char *)payload, "rate", val, sizeof(val)))
            rate = (uint32_t)atol(val);
        if (http_json_get_str((char *)payload, "chunk_ms", val, sizeof(val)))
            chunk_ms = (uint32_t)atol(val);
        if (http_json_get_str((char *)payload, "gain", val, sizeof(val)))
            gain = (uint8_t)atoi(val);

        // Clamp here too (the board's stream_start also clamps, but we want
        // the confirmed values in the "started" frame).
        if (rate < 8000)  rate = 8000;
        if (rate > 32000) rate = 32000;
        if (chunk_ms < 50)  chunk_ms = 50;
        if (chunk_ms > 500) chunk_ms = 500;
        if (gain < 1)  gain = 1;
        if (gain > 32) gain = 32;

        // Spawn the streaming task pinned to core 0.
        mic_stream_task_arg_t *arg = malloc(sizeof(mic_stream_task_arg_t));
        if (!arg) {
            const char *err = "{\"type\":\"error\",\"error\":\"oom\"}";
            httpd_ws_frame_t ef = {
                .final = true, .fragmented = false,
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t *)err, .len = strlen(err),
            };
            httpd_ws_send_frame(r, &ef);
            free(payload);
            return ESP_OK;
        }

        arg->server    = s_http_server;
        arg->client_fd = httpd_req_to_sockfd(r);
        arg->rate      = rate;
        arg->chunk_ms  = chunk_ms;
        arg->gain      = gain;

        BaseType_t ok = xTaskCreatePinnedToCore(
            mic_stream_task, "mic_ws", 4096, arg, 4, NULL, 0);
        if (ok != pdPASS) {
            free(arg);
            const char *err = "{\"type\":\"error\",\"error\":\"task_create_failed\"}";
            httpd_ws_frame_t ef = {
                .final = true, .fragmented = false,
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t *)err, .len = strlen(err),
            };
            httpd_ws_send_frame(r, &ef);
        }

    } else if (strcmp(cmd, "stop") == 0) {
        // Signal the streaming task to stop by flagging the stream inactive.
        // The task checks molty_mic_stream_active() in its loop condition.
        molty_mic_stream_stop();
    }

    free(payload);
    return ESP_OK;
}

// ═════════════════════════════════════════════════════════════════════════════
// ── /api/creature — creature registry + switching ─────────────────────────────
// GET  → {"ok":true,"current":"fluid","available":["fluid"]}
// POST {"name":"fluid"} → switch + return same shape
// ═════════════════════════════════════════════════════════════════════════════

static char *creature_status_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddBoolToObject(root, "ok", true);
    const char *cur = mole_creature_current();
    cJSON_AddStringToObject(root, "current", cur ? cur : "");
    cJSON *avail = cJSON_CreateArray();
    for (size_t i = 0; i < mole_creature_count(); i++) {
        cJSON_AddItemToArray(avail, cJSON_CreateString(mole_creature_name_at(i)));
    }
    cJSON_AddItemToObject(root, "available", avail);
    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return str;
}

static esp_err_t handler_creature_get(httpd_req_t *r)
{
    char *js = creature_status_json();
    if (!js) return http_send_json(r, "{\"ok\":false,\"error\":\"oom\"}");
    esp_err_t ret = http_send_json(r, js);
    free(js);
    return ret;
}

static esp_err_t handler_creature_post(httpd_req_t *r)
{
    int content_len = r->content_len;
    if (content_len <= 0 || content_len > 256) {
        return http_send_json(r, "{\"ok\":false,\"error\":\"bad_request\"}");
    }
    char *body = malloc((size_t)content_len + 1);
    if (!body) return http_send_json(r, "{\"ok\":false,\"error\":\"oom\"}");
    int received = http_recv_full(r, body, content_len);
    if (received <= 0) { free(body); return http_send_json(r, "{\"ok\":false,\"error\":\"recv_failed\"}"); }
    body[received] = '\0';

    cJSON *root = cJSON_ParseWithLength(body, (size_t)received);
    free(body);
    if (!root) return http_send_json(r, "{\"ok\":false,\"error\":\"json_parse\"}");

    cJSON *name_item = cJSON_GetObjectItem(root, "name");
    if (!name_item || !cJSON_IsString(name_item)) {
        cJSON_Delete(root);
        return http_send_json(r, "{\"ok\":false,\"error\":\"missing_name\"}");
    }
    char name_buf[32];
    snprintf(name_buf, sizeof(name_buf), "%s", name_item->valuestring);
    cJSON_Delete(root);

    esp_err_t ret = mole_creature_select(name_buf);
    if (ret == ESP_ERR_NOT_FOUND) {
        return http_send_json(r, "{\"ok\":false,\"error\":\"not_found\"}");
    }
    if (ret != ESP_OK) {
        char err[80];
        snprintf(err, sizeof(err), "{\"ok\":false,\"error\":\"select_failed\",\"code\":%d}", (int)ret);
        return http_send_json(r, err);
    }

    char *js = creature_status_json();
    if (!js) return http_send_json(r, "{\"ok\":false,\"error\":\"oom\"}");
    ret = http_send_json(r, js);
    free(js);
    return ret;
}

// ── GET /api/creature/state — current creature's state JSON ──────────────────
static esp_err_t handler_creature_state_get(httpd_req_t *r)
{
    char *js = NULL;
    esp_err_t ret = mole_creature_state_get(&js);
    if (ret == ESP_ERR_INVALID_STATE)
        return http_send_json(r, "{\"ok\":false,\"error\":\"no_creature\"}");
    if (ret != ESP_OK || !js)
        return http_send_json(r, "{\"ok\":false,\"error\":\"state_get_failed\"}");
    ret = http_send_json(r, js);
    free(js);
    return ret;
}

// ── POST /api/creature/state — forward to current creature's state_set_json ──
static esp_err_t handler_creature_state_post(httpd_req_t *r)
{
    int content_len = r->content_len;
    /* State bodies can be large (e.g. inline reader text): up to 200 KB,
     * staged in PSRAM to spare the internal heap. */
    if (content_len <= 0 || content_len > 200000) {
        return http_send_json(r, "{\"ok\":false,\"error\":\"bad_request\"}");
    }
    char *body = heap_caps_malloc((size_t)content_len + 1, MALLOC_CAP_SPIRAM);
    if (!body) return http_send_json(r, "{\"ok\":false,\"error\":\"oom\"}");
    int received = http_recv_full(r, body, content_len);
    if (received <= 0) { free(body); return http_send_json(r, "{\"ok\":false,\"error\":\"recv_failed\"}"); }
    body[received] = '\0';

    /* an explicit mood: a listen/say finishing must not drop it back to idle */
    esp_err_t ret = link_cmd_state_set_explicit(body);
    free(body);
    if (ret == ESP_ERR_INVALID_STATE)
        return http_send_json(r, "{\"ok\":false,\"error\":\"no_creature\"}");
    if (ret != ESP_OK) {
        char err[80];
        snprintf(err, sizeof(err), "{\"ok\":false,\"error\":\"set_failed\",\"code\":%d}", (int)ret);
        return http_send_json(r, err);
    }

    /* Return updated state */
    char *js = NULL;
    ret = mole_creature_state_get(&js);
    if (ret != ESP_OK || !js) return http_send_json(r, "{\"ok\":false,\"error\":\"state_get_failed\"}");
    ret = http_send_json(r, js);
    free(js);
    return ret;
}



// ═════════════════════════════════════════════════════════════════════════════
// ── /api/creature/file — the fluid's creature as a .molty file ────────────────
// POST (binary, ≤ 16 KB): validated (magic, version, length, CRC, bounds — see
// creatures/MOLTY-FORMAT.md), stored atomically as /spiffs/creature.molty and
// hot-reloaded into the fluid's "creature" state. No reflash to swap creatures.
// GET: the stored file, else the firmware default (header X-Molty-Default: 1).
// ═════════════════════════════════════════════════════════════════════════════
static esp_err_t handler_creature_file_post(httpd_req_t *r)
{
    int len = r->content_len;
    if (len <= 0 || len > 16384) return http_send_json(r, "{\"ok\":false,\"error\":\"bad_size_max_16kb\"}");
    uint8_t *body = heap_caps_malloc((size_t)len, MALLOC_CAP_SPIRAM);
    if (!body) return http_send_json(r, "{\"ok\":false,\"error\":\"oom\"}");
    int got = http_recv_full(r, (char *)body, len);
    if (got != len) { heap_caps_free(body); return http_send_json(r, "{\"ok\":false,\"error\":\"recv_failed\"}"); }
    const char *why = "?";
    esp_err_t e = mole_fluid_creature_file_put(body, (size_t)len, &why);
    heap_caps_free(body);
    char buf[96];
    if (e != ESP_OK) snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", why);
    else snprintf(buf, sizeof(buf), "{\"ok\":true,\"bytes\":%d}", len);
    return http_send_json(r, buf);
}

static esp_err_t handler_creature_file_get(httpd_req_t *r)
{
    uint8_t *b = NULL; size_t n = 0; bool dflt = false;
    if (mole_fluid_creature_file_get(&b, &n, &dflt) != ESP_OK || !b) return http_send_json(r, "{\"ok\":false,\"error\":\"none\"}");
    httpd_resp_set_type(r, "application/octet-stream");
    httpd_resp_set_hdr(r, "Content-Disposition", "attachment; filename=\"creature.molty\"");
    if (dflt) httpd_resp_set_hdr(r, "X-Molty-Default", "1");
    esp_err_t e = httpd_resp_send(r, (const char *)b, (ssize_t)n);
    heap_caps_free(b);
    return e;
}

// ═════════════════════════════════════════════════════════════════════════════
// ── /api/reader/text — text for the fluid's speed reader ──────────────────────
// POST text/plain (raw UTF-8, ≤ 16 KB) — no JSON wrapping for large pastes.
// Returns the fluid's state JSON (nwords, text_ver, ...) when it is active.
// ═════════════════════════════════════════════════════════════════════════════

#define READER_TEXT_HTTP_MAX (16 * 1024)

static esp_err_t handler_reader_text(httpd_req_t *r)
{
    int len = r->content_len;
    if (len <= 0 || len > READER_TEXT_HTTP_MAX) {
        return http_send_json(r, "{\"ok\":false,\"error\":\"bad_request_max_16kb\"}");
    }
    char *body = heap_caps_malloc((size_t)len + 1, MALLOC_CAP_SPIRAM);
    if (!body) return http_send_json(r, "{\"ok\":false,\"error\":\"oom\"}");

    int got = http_recv_full(r, body, len);
    if (got <= 0) {
        heap_caps_free(body);
        return http_send_json(r, "{\"ok\":false,\"error\":\"recv_failed\"}");
    }
    body[got] = '\0';

    const char *cur = mole_creature_current();
    esp_err_t e = mole_fluid_set_text(body, got);
    heap_caps_free(body);
    if (e != ESP_OK) {
        char buf[80];
        snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"set_text\",\"code\":%d}", (int)e);
        return http_send_json(r, buf);
    }

    /* Return the updated state so the web can sync nwords + text_ver. */
    char *js = NULL;
    if (cur && strcmp(cur, "fluid") == 0) {
        mole_creature_state_get(&js);
    }
    if (js) {
        esp_err_t ret = http_send_json(r, js);
        free(js);
        return ret;
    }
    return http_send_json(r, "{\"ok\":true}");
}

// ═════════════════════════════════════════════════════════════════════════════
// ── /api/reader/append — stream-append words to the speed reader ──────────────
// POST with Content-Type: text/plain; body = one or more whitespace-delimited
// words (e.g., "Hello," or "Hello, world!").  Does NOT reset playback; extends
// the schedule so the new words play immediately after the last buffered word.
// Used to stream an LLM reply word by word.
// A streaming session begins with POST /api/creature/state {stream_start:true}
// and ends with POST /api/creature/state {stream_end:true}.
// Returns: {"ok":true}
#define READER_APPEND_MAX 2048   /* per-request cap; buffer total is 16 KB */

static esp_err_t handler_reader_append(httpd_req_t *r)
{
    int len = r->content_len;
    if (len <= 0 || len > READER_APPEND_MAX) {
        return http_send_json(r, "{\"ok\":false,\"error\":\"bad_size\"}");
    }
    char *body = heap_caps_malloc((size_t)len + 1, MALLOC_CAP_DEFAULT);
    if (!body) return http_send_json(r, "{\"ok\":false,\"error\":\"oom\"}");

    int got = http_recv_full(r, body, len);
    if (got <= 0) {
        heap_caps_free(body);
        return http_send_json(r, "{\"ok\":false,\"error\":\"recv\"}");
    }
    body[got] = '\0';

    esp_err_t e = mole_fluid_append_text(body, got);   /* streams into the fluid's speed reader */
    heap_caps_free(body);
    if (e != ESP_OK) {
        char buf[80];
        snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"append\",\"code\":%d}", (int)e);
        return http_send_json(r, buf);
    }
    return http_send_json(r, "{\"ok\":true}");
}


// ── /api/reboot_bootloader — reboot into ROM download mode (hands-free flash) ──
// ═════════════════════════════════════════════════════════════════════════════
// Sets the ROM's FORCE_DOWNLOAD_BOOT latch and restarts; the chip comes up in
// USB-Serial-JTAG download mode with no BOOT button held, so esptool can flash
// it directly. The bit is cleared early in app_main so a normal boot never loops.
static void reboot_bootloader_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(300));      /* let the HTTP response flush */
    REG_SET_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

static esp_err_t handler_reboot_bootloader(httpd_req_t *r)
{
    esp_err_t e = http_send_json(r, "{\"ok\":true,\"rebooting\":\"download\"}");
    xTaskCreate(reboot_bootloader_task, "reboot_dl", 2048, NULL, 5, NULL);
    return e;
}

// ═════════════════════════════════════════════════════════════════════════════
// ── /api/ota — stream a new app image into the spare OTA slot, then reboot ─────
// ═════════════════════════════════════════════════════════════════════════════
// Hands-free flashing: `curl --data-binary @build/tusb_ncm.bin http://molty.local/api/ota`
// Rollback-protected — if the new image fails to boot+validate, the bootloader
// reverts to this image on the next reset.
static esp_err_t handler_ota(httpd_req_t *r)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return http_send_json(r, "{\"ok\":false,\"error\":\"no_ota_partition\"}");
    if (r->content_len <= 0) return http_send_json(r, "{\"ok\":false,\"error\":\"empty\"}");

    esp_ota_handle_t h = 0;
    esp_err_t e = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (e != ESP_OK) {
        char b[96]; snprintf(b, sizeof(b), "{\"ok\":false,\"error\":\"begin\",\"code\":%d}", (int)e);
        return http_send_json(r, b);
    }

    char *buf = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (!buf) { esp_ota_abort(h); return http_send_json(r, "{\"ok\":false,\"error\":\"oom\"}"); }
    int remaining = r->content_len, total = 0;
    /* the image's board tag (molty_board.h) sits in its first bytes: refuse an image
     * built for another board before it can boot here */
    uint8_t head[MOLTY_IMAGE_TAG_OFFSET + MOLTY_IMAGE_TAG_BYTES];
    size_t have = 0;
    bool checked = false;
    while (remaining > 0) {
        int want = remaining < 8192 ? remaining : 8192;
        int rcv = httpd_req_recv(r, buf, want);
        if (rcv == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (rcv <= 0) { heap_caps_free(buf); esp_ota_abort(h); return http_send_json(r, "{\"ok\":false,\"error\":\"recv\"}"); }
        if (!checked) {
            size_t n = sizeof(head) - have < (size_t)rcv ? sizeof(head) - have : (size_t)rcv;
            memcpy(head + have, buf, n);
            have += n;
            if (have == sizeof(head)) {
                checked = true;
                char other[64];
                if (molty_board_image_check(head, have, other, sizeof(other)) == 0) {
                    heap_caps_free(buf); esp_ota_abort(h);
                    char b[192];
                    snprintf(b, sizeof(b), "{\"ok\":false,\"error\":\"wrong_board\",\"image\":\"%s\",\"device\":\"%s\"}",
                             other, molty_board_dir());
                    return http_send_json(r, b);
                }
            }
        }
        if (esp_ota_write(h, buf, rcv) != ESP_OK) { heap_caps_free(buf); esp_ota_abort(h); return http_send_json(r, "{\"ok\":false,\"error\":\"write\"}"); }
        remaining -= rcv; total += rcv;
    }
    heap_caps_free(buf);

    if ((e = esp_ota_end(h)) != ESP_OK) {
        char b[96]; snprintf(b, sizeof(b), "{\"ok\":false,\"error\":\"verify\",\"code\":%d}", (int)e);
        return http_send_json(r, b);   // bad image (CRC/signature) — current app keeps running
    }
    if ((e = esp_ota_set_boot_partition(part)) != ESP_OK) {
        char b[96]; snprintf(b, sizeof(b), "{\"ok\":false,\"error\":\"setboot\",\"code\":%d}", (int)e);
        return http_send_json(r, b);
    }
    char ok[96]; snprintf(ok, sizeof(ok), "{\"ok\":true,\"written\":%d,\"slot\":\"%s\",\"reboot\":true}", total, part->label);
    esp_err_t resp = http_send_json(r, ok);
    http_reboot_soon("ota_reboot");
    return resp;
}

// ═════════════════════════════════════════════════════════════════════════════
// ── /api/info ─────────────────────────────────────────────────────────────────
// ═════════════════════════════════════════════════════════════════════════════

// Forward-declared — defined with the rest of the coredump retrieval code
// below (near GET /api/coredump), used here so /api/info can report presence
// without pulling the whole image.
static bool coredump_present(size_t *out_size);

static esp_err_t handler_info(httpd_req_t *r)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    uint32_t uptime_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    // Reset reason for THIS boot + the persisted boot counter (mole_diag): a
    // panic / WDT / brownout shows up here instead of looking like a power cycle.
    const char *reset_reason = mole_diag_reset_reason();

    // Coredump presence — see the "Coredump retrieval" block below. Cheap
    // (reads a 4-byte length field), safe to call on every /api/info poll.
    size_t coredump_bytes = 0;
    bool has_coredump = coredump_present(&coredump_bytes);

    // Task watchdog: "on" when CONFIG_ESP_TASK_WDT_PANIC is set, so a wedge on a
    // subscribed task panics -> coredump (GET /api/coredump). The list is a
    // literal (the TWDT has no "list subscribers" API): keep it in sync with the
    // esp_task_wdt_add() call sites (qmi8658.c, imu_ws_task, mole_fluid.c).
#if CONFIG_ESP_TASK_WDT_PANIC
    static const char *k_twdt_state = "on";
#else
    static const char *k_twdt_state = "off";
#endif
    static const char *k_twdt_tasks = "[\"imu_poll\",\"imu_ws\",\"fluid_task\",\"fluid_draw\"]";

    // The board: id, display and the capabilities that came up (molty_board.h).
    char caps[160] = "[";
    for (int i = 0, n = 1; i < MOLTY_CAP_COUNT; i++) {
        if (!molty_board_has(1u << i)) continue;
        n += snprintf(caps + n, sizeof(caps) - (size_t)n, "%s\"%s\"", n > 1 ? "," : "", molty_cap_name(i));
        if (n >= (int)sizeof(caps) - 2) { n = (int)sizeof(caps) - 2; break; }
    }
    strlcat(caps, "]", sizeof(caps));
    char mismatch[192] = "";
    if (molty_board_mismatch()) snprintf(mismatch, sizeof(mismatch), "\"board_mismatch\":\"%s\",", molty_board_mismatch());
    char battery[32] = "";
    int mv = 0;
    if (molty_battery_mv(&mv) == ESP_OK) snprintf(battery, sizeof(battery), "\"battery_mv\":%d,", mv);

    char buf[1280];
    snprintf(buf, sizeof(buf),
        "{\"ok\":true,"
        "\"chip\":\"esp32-s3\","
        "\"board\":\"%s\","
        "\"board_name\":\"%s\","
        "\"display\":{\"width\":%d,\"height\":%d,\"shape\":\"%s\"},"
        "\"caps\":%s,"
        "%s%s"
        "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
        "\"flash_mb\":%lu,"
        "\"psram_mb\":%lu,"
        "\"free_heap\":%lu,"
        "\"free_psram\":%lu,"
        "\"uptime_ms\":%lu,"
        "\"reset_reason\":\"%s\","
        "\"boot_count\":%lu,"
        "\"spiffs_state\":\"%s\","
        "\"spiffs_html_recovered\":%s,"
        "\"coredump_present\":%s,"
        "\"coredump_bytes\":%u,"
        "\"twdt\":\"%s\","
        "\"twdt_tasks\":%s,"
        "\"idf_version\":\"v5.5\","
        "\"fw\":\"%s\","
        "\"fw_build\":\"" __DATE__ "T" __TIME__ "\"}",
        molty_board_id(), molty_board_name(),
        molty_display_width(), molty_display_height(),
        molty_display_shape() == MOLTY_SHAPE_ROUND ? "round" : "rect",
        caps, mismatch, battery,
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        (unsigned long)(flash_size / (1024 * 1024)),
        (unsigned long)(esp_psram_get_size() / (1024 * 1024)),
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned long)uptime_ms,
        reset_reason,
        (unsigned long)mole_diag_boot_count(),
        console_spiffs_state(),
        console_html_recovered() ? "true" : "false",
        has_coredump ? "true" : "false",
        (unsigned)coredump_bytes,
        k_twdt_state,
        k_twdt_tasks,
        esp_app_get_description()->version);

    return http_send_json(r, buf);
}


// ═════════════════════════════════════════════════════════════════════════════
// ── GET /api/framebuffer — screenshot of the LCD ───────────────────────────────
// Streams the live framebuffer (the board's display size, 412×412 on the 1.46) as a
// binary PPM (P6). The framebuffer (molty_display_framebuffer()) holds RGB565
// byte-swapped for the SPI panel's wire
// order; we swap back and expand 5/6/5 bits to 8 bits per channel.
// Consistency: there is no framebuffer lock, so we memcpy the whole 339 KB
// buffer first (sub-millisecond) and convert from the copy; a frame can still,
// rarely, be part-old/part-new.
// ═════════════════════════════════════════════════════════════════════════════
static inline void rgb565_be_to_rgb888(uint16_t stored, uint8_t *out /* r,g,b */)
{
    uint16_t le = (uint16_t)((stored >> 8) | (stored << 8));  // undo the byte-swap
    uint8_t r5 = (uint8_t)((le >> 11) & 0x1F);
    uint8_t g6 = (uint8_t)((le >> 5)  & 0x3F);
    uint8_t b5 = (uint8_t)(le & 0x1F);
    out[0] = (uint8_t)((r5 << 3) | (r5 >> 2));   // 5→8 bits, replicate high bits
    out[1] = (uint8_t)((g6 << 2) | (g6 >> 4));   // 6→8 bits
    out[2] = (uint8_t)((b5 << 3) | (b5 >> 2));   // 5→8 bits
}

#define FB_PARITY_ROWS_PER_CHUNK 32   // 32*412*3 = 39,552 B/chunk on the 1.46 — modest scratch, ~13 chunks/frame

static esp_err_t handler_framebuffer(httpd_req_t *r)
{
    const int W = molty_display_width(), H = molty_display_height();
    uint16_t *live_fb = molty_display_framebuffer();
    if (!live_fb) return http_send_json(r, "{\"ok\":false,\"error\":\"no_framebuffer\"}");

    size_t fb_bytes = (size_t)W * H * sizeof(uint16_t);
    uint16_t *snap = (uint16_t *)heap_caps_malloc(fb_bytes, MALLOC_CAP_SPIRAM);
    if (!snap) return http_send_json(r, "{\"ok\":false,\"error\":\"oom\"}");
    memcpy(snap, live_fb, fb_bytes);   // the "snapshot" — see consistency note above

    size_t rgb_chunk_bytes = (size_t)FB_PARITY_ROWS_PER_CHUNK * W * 3;
    uint8_t *rgb_scratch = (uint8_t *)heap_caps_malloc(rgb_chunk_bytes, MALLOC_CAP_SPIRAM);
    if (!rgb_scratch) {
        heap_caps_free(snap);
        return http_send_json(r, "{\"ok\":false,\"error\":\"oom\"}");
    }

    httpd_resp_set_type(r, "image/x-portable-pixmap");
    httpd_resp_set_hdr(r, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");

    char hdr[32];
    int hdr_len = snprintf(hdr, sizeof(hdr), "P6 %d %d 255\n", W, H);
    httpd_resp_send_chunk(r, hdr, hdr_len);

    for (int y0 = 0; y0 < H; y0 += FB_PARITY_ROWS_PER_CHUNK) {
        int rows = FB_PARITY_ROWS_PER_CHUNK;
        if (y0 + rows > H) rows = H - y0;
        uint8_t *dst = rgb_scratch;
        for (int y = 0; y < rows; y++) {
            const uint16_t *src_row = snap + (size_t)(y0 + y) * W;
            for (int x = 0; x < W; x++) {
                rgb565_be_to_rgb888(src_row[x], dst);
                dst += 3;
            }
        }
        httpd_resp_send_chunk(r, (const char *)rgb_scratch, (ssize_t)((size_t)rows * W * 3));
    }
    httpd_resp_send_chunk(r, NULL, 0);   // EOF

    heap_caps_free(rgb_scratch);
    heap_caps_free(snap);
    return ESP_OK;
}

// ═════════════════════════════════════════════════════════════════════════════
// ── Coredump retrieval ─────────────────────────────────────────────────────────
// ═════════════════════════════════════════════════════════════════════════════
// CONFIG_ESP_CONSOLE_NONE (the 1.46 sdkconfig.board) means that board has no
// UART / USB-Serial-JTAG console at all, so a panic prints to nowhere and
// `idf.py monitor`/serial-based coredump capture is not an option.
// CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH (sdkconfig.defaults) instead writes an
// ELF+SHA256 core dump into the dedicated "coredump" partition (partitions.csv,
// 64 KB, carved out of SPIFFS) on any panic / abort / task-WDT reset. GET
// /api/coredump is the only channel this board has to get that image off the
// device: it streams the partition's exact stored image (header+ELF+checksum
// — the same bytes `idf.py coredump-info --core <file>` expects, auto-detected
// as "raw") back over HTTP in chunks. See /api/info's "coredump_present" /
// "coredump_bytes" fields to check without pulling the whole thing.

// Returns true (and, if out_size is non-NULL, the stored image size in bytes)
// if a valid core dump currently sits in flash; false if there is none, or if
// what's there fails its own length sanity check. esp_core_dump_image_get()
// only reads the partition's leading 4-byte length field and is documented as
// "always available even when core dump is disabled", so this is cheap enough
// to call on every /api/info request.
static bool coredump_present(size_t *out_size)
{
    size_t addr = 0, size = 0;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK) return false;
    if (out_size) *out_size = size;
    return true;
}

#define COREDUMP_STREAM_CHUNK_BYTES 4096

static esp_err_t handler_coredump(httpd_req_t *r)
{
    size_t size = 0;
    if (!coredump_present(&size)) {
        httpd_resp_set_status(r, "404 Not Found");
        return http_send_json(r, "{\"ok\":false,\"error\":\"no_coredump\"}");
    }

    const esp_partition_t *core_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!core_part) {
        httpd_resp_set_status(r, "404 Not Found");
        return http_send_json(r, "{\"ok\":false,\"error\":\"no_coredump_partition\"}");
    }

    uint8_t *chunk = (uint8_t *)heap_caps_malloc(COREDUMP_STREAM_CHUNK_BYTES, MALLOC_CAP_SPIRAM);
    if (!chunk) return http_send_json(r, "{\"ok\":false,\"error\":\"oom\"}");

    httpd_resp_set_type(r, "application/octet-stream");
    httpd_resp_set_hdr(r, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_hdr(r, "Content-Disposition", "attachment; filename=\"coredump.bin\"");

    size_t sent = 0;
    esp_err_t result = ESP_OK;
    while (sent < size) {
        size_t want = size - sent;
        if (want > COREDUMP_STREAM_CHUNK_BYTES) want = COREDUMP_STREAM_CHUNK_BYTES;
        esp_err_t rerr = esp_partition_read(core_part, sent, chunk, want);
        if (rerr != ESP_OK) {
            ESP_LOGE(TAG, "/api/coredump: esp_partition_read failed at off=%u (%s)",
                     (unsigned)sent, esp_err_to_name(rerr));
            result = ESP_FAIL;
            break;
        }
        result = httpd_resp_send_chunk(r, (const char *)chunk, (ssize_t)want);
        if (result != ESP_OK) break;
        sent += want;
        vTaskDelay(1);   // yield between chunks — same pattern as recv_full/spiffs_save_html
    }
    httpd_resp_send_chunk(r, NULL, 0);   // EOF (harmless if the loop above already errored)

    heap_caps_free(chunk);
    ESP_LOGI(TAG, "/api/coredump: streamed %u/%u bytes", (unsigned)sent, (unsigned)size);
    return result;
}


// ─────────────────────────────────────────────────────────────────────────────
// 404 handler
// ─────────────────────────────────────────────────────────────────────────────
static esp_err_t handler_404(httpd_req_t *r, httpd_err_code_t err)
{
    return http_send_json(r, "{\"ok\":false,\"error\":\"not_found\"}");
}

// ─────────────────────────────────────────────────────────────────────────────
// Start HTTP server
// ─────────────────────────────────────────────────────────────────────────────
esp_err_t http_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size       = 8192;   // audio handlers do math on the stack
    cfg.max_open_sockets = 7;      // headroom for WS + concurrent tabs
    cfg.lru_purge_enable = true;
    cfg.server_port      = 80;
    cfg.max_uri_handlers = 48;     // ~36 routes today; headroom so new routes always fit

    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &cfg), TAG, "httpd_start failed");
    s_http_server = server;

    static const httpd_uri_t routes[] = {
        // audio
        {.uri = "/api/audio/tone",     .method = HTTP_GET,  .handler = handler_audio_tone},
        {.uri = "/api/audio/play",     .method = HTTP_POST, .handler = handler_audio_play},
        {.uri = "/api/audio/play_wav", .method = HTTP_POST, .handler = handler_audio_play_wav},
        {.uri = "/api/audio/record",   .method = HTTP_GET,  .handler = handler_audio_record},
        {.uri = "/api/audio/volume",   .method = HTTP_POST, .handler = handler_audio_volume},
        {.uri = "/ws/audio/mic",       .method = HTTP_GET,  .handler = handler_ws_audio_mic, .is_websocket = true},
        // IMU
        {.uri = "/api/imu/snapshot",   .method = HTTP_GET,  .handler = handler_imu_snapshot},
        {.uri = "/ws/imu",             .method = HTTP_GET,  .handler = handler_ws_imu, .is_websocket = true},
        // creature + reader text + screen
        {.uri = "/api/creature",       .method = HTTP_GET,  .handler = handler_creature_get},
        {.uri = "/api/creature",       .method = HTTP_POST, .handler = handler_creature_post},
        {.uri = "/api/creature/state", .method = HTTP_GET,  .handler = handler_creature_state_get},
        {.uri = "/api/creature/state", .method = HTTP_POST, .handler = handler_creature_state_post},
        {.uri = "/api/creature/file",  .method = HTTP_POST, .handler = handler_creature_file_post},
        {.uri = "/api/creature/file",  .method = HTTP_GET,  .handler = handler_creature_file_get},
        {.uri = "/api/reader/text",    .method = HTTP_POST, .handler = handler_reader_text},
        {.uri = "/api/reader/append",  .method = HTTP_POST, .handler = handler_reader_append},
        {.uri = "/api/framebuffer",    .method = HTTP_GET,  .handler = handler_framebuffer},
        // system
        {.uri = "/api/info",           .method = HTTP_GET,  .handler = handler_info},
        {.uri = "/api/coredump",       .method = HTTP_GET,  .handler = handler_coredump},
        {.uri = "/api/ota",            .method = HTTP_POST, .handler = handler_ota},
        {.uri = "/api/reboot_bootloader", .method = HTTP_POST, .handler = handler_reboot_bootloader},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &routes[i]), TAG, "route %s", routes[i].uri);
    }

    ESP_RETURN_ON_ERROR(console_http_register(server), TAG, "console routes");
    ESP_RETURN_ON_ERROR(net_http_register(server), TAG, "wifi routes");
    ESP_RETURN_ON_ERROR(link_http_register(server), TAG, "link routes");
    ESP_RETURN_ON_ERROR(mole_diag_http_register(server), TAG, "diag routes");
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, handler_404);

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}

void http_imu_ws_start(void)
{
    if (!molty_board_has(MOLTY_CAP_IMU)) return;   /* no IMU: no streamer (the upgrade is refused) */
    xTaskCreate(imu_ws_task, "imu_ws", 4096, NULL, 4, NULL);
}
