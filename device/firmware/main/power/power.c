/*
 * SPDX-License-Identifier: MIT
 *
 * power.c — power.h: do not disturb, the battery history, /api/power.
 *
 * One small task at 1 Hz (internal stack: it writes SPIFFS and NVS) counts the minute's
 * screen-on and audio seconds, then reads the battery and keeps a sample. A dnd change
 * (from any task, sense.c's PSRAM stack too) wakes it to say so on screen ("shh" / "hi"
 * in the fluid) and to store the switch. Its flash writes (the switch, the history every
 * 10 min) wait for a calm screen (mole_flash_may, flash_gate.h); a restart or power off
 * writes them at once. The ring and dnd are shared with httpd under s_mux.
 * Every POWER_EVERY s it tells the fluid what Molty runs on, for the screen's nap (screen_nap.h), and keeps the nap's
 * settings the human changed (mole_fluid_nap_setting) in the same quiet window.
 */
#include "power.h"
#include "molty_board.h"
#include "mole_fluid.h"
#include "mole_diag.h"
#include "mole_creature.h"
#include "link_client.h"
#include "link_audio.h"
#include "http.h"
#include "screen_nap.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "cJSON.h"

#define HIST_N        720                   /* a sample a minute: 12 h                */
#define SAVE_EVERY    10                    /* samples between two writes (10 min)    */
#define HIST_PATH     "/spiffs/power.bin"
#define HIST_TMP      "/spiffs/power.tmp"
#define HIST_MAGIC    0x31525750u           /* "PWR1"                                  */
#define POWER_EVERY   5                     /* s between two looks at USB for the nap  */

enum { F_CHARGING = 1, F_USB = 2, F_DND = 4, F_LINK = 8, F_BATTERY = 16 };

typedef struct __attribute__((packed)) {
    uint16_t boot;                          /* mole_diag_boot_count (low bits)        */
    uint32_t up_s;                          /* uptime at the sample                   */
    uint16_t mv;                            /* 0: no battery reading                  */
    int8_t   pct;                           /* -1: no gauge                           */
    uint8_t  flags;                         /* F_*                                    */
    uint8_t  screen_s, audio_s;             /* of the minute's 60 s                   */
    int8_t   rssi;                          /* 0: Wi-Fi down                          */
} sample_t;

static SemaphoreHandle_t s_mux;
static sample_t *s_hist;                    /* PSRAM ring, HIST_N                     */
static int       s_n, s_head;               /* samples kept; the next slot            */
static bool      s_dnd;
static unsigned  s_dnd_gen = 1;
static TaskHandle_t s_task;
static volatile bool s_dnd_dirty;           /* a change the task has not shown yet   */
static bool      s_dnd_unsaved;             /* (the task's) the switch not on the flash yet           */
static bool      s_hist_due, s_hist_new;    /* ... the history's write due; samples since the last */
static fg_hold_t s_dnd_hold, s_hist_hold, s_nap_hold;
static unsigned  s_nap_saved;               /* (the task's) the nap settings' generation on the flash */
static volatile bool s_off;                 /* power_off: the goodbye, then off      */

/* ── do not disturb ───────────────────────────────────────────────────────── */

bool power_dnd(void) { return s_dnd; }
unsigned power_dnd_gen(void) { return s_dnd_gen; }

void power_dnd_set(bool on)
{
    if (!s_task || on == s_dnd) return;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_dnd = on;
    if (++s_dnd_gen == 0) s_dnd_gen = 1;
    s_dnd_dirty = true;
    xSemaphoreGive(s_mux);
    mole_diag_event(false, "power.dnd %s", on ? "on" : "off");
    xTaskNotifyGive(s_task);
}

void power_off(void)
{
    if (!s_task || s_off) return;
    s_off = true;
    mole_diag_event(false, "power.off goodbye");
    xTaskNotifyGive(s_task);
}

/* On the task: show the switch now; it is kept when the screen is calm (dnd_store). */
static void dnd_show(void)
{
    s_dnd_dirty = false;
    s_dnd_unsaved = true;
    mole_fluid_screen(true);
    mole_creature_state_set(s_dnd ? "{\"state\":\"text\",\"text\":\"shh\"}" : "{\"state\":\"text\",\"text\":\"hi\"}");
}

static void dnd_store(void)
{
    s_dnd_unsaved = false;
    nvs_handle_t h;
    if (nvs_open("power", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, "dnd", s_dnd); nvs_commit(h); nvs_close(h); }
}

/* The nap settings as the fluid has them now (NVS "fluid", where it loads them from). */
static void nap_store(void)
{
    uint32_t bat, usb;
    s_nap_saved = mole_fluid_nap_setting(&bat, &usb);
    nvs_handle_t h;
    if (nvs_open("fluid", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "nap_bat", (int32_t)bat); nvs_set_i32(h, "nap_usb", (int32_t)usb); nvs_commit(h); nvs_close(h);
    }
}

/* What the nap's timer goes by: on battery only when the board says so (a reading without USB and with a gauge or the
 * charger's state); no battery, or a board that cannot tell (the 1.28's voltage alone), counts as USB. */
static int power_source(void)
{
    molty_battery_t b;
    const esp_err_t e = molty_battery_read(&b);
    if (e == ESP_ERR_NOT_SUPPORTED || e == ESP_ERR_NOT_FOUND) return NAP_POWER_USB;
    if (e != ESP_OK) return -1;                        /* (a failed read: as it was) */
    return b.usb || (b.pct < 0 && !b.charging) ? NAP_POWER_USB : NAP_POWER_BATTERY;
}

/* ── history ──────────────────────────────────────────────────────────────── */

static const sample_t *at(int i) { return &s_hist[(s_head - s_n + i + HIST_N) % HIST_N]; }   /* 0 = oldest */

static void load(void)
{
    FILE *f = fopen(HIST_PATH, "rb");
    if (!f) return;
    uint32_t magic = 0; uint16_t n = 0;
    if (fread(&magic, 4, 1, f) == 1 && magic == HIST_MAGIC && fread(&n, 2, 1, f) == 1 && n <= HIST_N &&
        fread(s_hist, sizeof(sample_t), n, f) == n) { s_n = n; s_head = n % HIST_N; }
    fclose(f);
}

/* The file: the magic, the count (u16), the samples oldest first (load) */
static void save(void)
{
    s_hist_due = s_hist_new = false;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    const uint16_t n = (uint16_t)s_n;
    const size_t len = 6 + sizeof(sample_t) * n;
    uint8_t *buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (buf) {
        memcpy(buf, &(uint32_t){ HIST_MAGIC }, 4); memcpy(buf + 4, &n, 2);
        for (int i = 0; i < n; i++) memcpy(buf + 6 + sizeof(sample_t) * i, at(i), sizeof(sample_t));
    }
    xSemaphoreGive(s_mux);
    const bool ok = buf && mole_flash_file(HIST_PATH, HIST_TMP, buf, len);
    heap_caps_free(buf);
    if (!ok) mole_diag_event(false, "power.save failed");
}

/* On the task: what the flash still owes, when it may go (or at once: a restart or power off is next) */
static void flush_due(void)
{
    if (s_dnd_unsaved && mole_flash_may(&s_dnd_hold)) dnd_store();
    if (mole_fluid_nap_setting(NULL, NULL) != s_nap_saved && mole_flash_may(&s_nap_hold)) nap_store();
    if ((s_hist_due || (s_hist_new && mole_flash_forced())) && mole_flash_may(&s_hist_hold)) save();
}

static int8_t rssi_now(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

/* On the task: the goodbye plays (at most 8 s), the history is kept, the power goes. With
 * USB keeping the board up it restarts instead: the boot screen says it heard. */
static void off_now(void)
{
    link_audio_stop_any();
    mole_fluid_goodbye();
    for (int i = 0; i < 80 && !mole_fluid_goodbye_done(); i++) vTaskDelay(pdMS_TO_TICKS(100));
    mole_flash_force();
    if (s_dnd_unsaved) dnd_store();
    if (mole_fluid_nap_setting(NULL, NULL) != s_nap_saved) nap_store();
    save();
    mole_diag_event(false, "power.off now");
    mole_diag_flush(3000);                             /* the log ring does not survive a power off */
    const esp_err_t e = molty_power_off();
    mole_diag_event(false, "power.off failed (%s): restarting", esp_err_to_name(e));
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

static void power_task(void *arg)
{
    (void)arg;
    int screen_s = 0, audio_s = 0, ticks = 0, since_save = 0;
    TickType_t next = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
    for (;;) {
        const TickType_t now = xTaskGetTickCount();
        if ((int32_t)(next - now) > 0) ulTaskNotifyTake(pdTRUE, next - now);
        if (s_off) off_now();
        if (s_dnd_dirty) dnd_show();
        const bool forced = mole_flash_forced();
        flush_due();
        if (forced) mole_flash_flushed();               /* (a restart waits for this: flash_shutdown) */
        if ((int32_t)(xTaskGetTickCount() - next) < 0) continue;
        next += pdMS_TO_TICKS(1000);
        screen_s += mole_fluid_screen_on();
        audio_s += link_audio_busy();
        if (ticks % POWER_EVERY == 0) { const int p = power_source(); if (p >= 0) mole_fluid_power(p); }
        if (++ticks < 60) continue;

        molty_battery_t b;
        const bool bat = molty_battery_read(&b) == ESP_OK;
        sample_t s = {
            .boot = (uint16_t)mole_diag_boot_count(), .up_s = (uint32_t)(esp_timer_get_time() / 1000000),
            .mv = bat ? (uint16_t)b.mv : 0, .pct = bat ? (int8_t)b.pct : -1,
            .flags = (uint8_t)((bat ? F_BATTERY : 0) | (bat && b.charging ? F_CHARGING : 0) | (bat && b.usb ? F_USB : 0) |
                               (s_dnd ? F_DND : 0) | (link_client_connected() ? F_LINK : 0)),
            .screen_s = (uint8_t)screen_s, .audio_s = (uint8_t)audio_s, .rssi = rssi_now(),
        };
        ticks = screen_s = audio_s = 0;
        xSemaphoreTake(s_mux, portMAX_DELAY);
        s_hist[s_head] = s;
        s_head = (s_head + 1) % HIST_N;
        if (s_n < HIST_N) s_n++;
        s_hist_new = true;
        xSemaphoreGive(s_mux);
        mole_diag_event(false, "pwr mv=%u pct=%d chg=%d usb=%d scr=%us aud=%us link=%d rssi=%d dnd=%d",
                        s.mv, s.pct, !!(s.flags & F_CHARGING), !!(s.flags & F_USB), s.screen_s, s.audio_s,
                        !!(s.flags & F_LINK), s.rssi, !!(s.flags & F_DND));
        if (++since_save >= SAVE_EVERY) { since_save = 0; s_hist_due = true; flush_due(); }
    }
}

esp_err_t power_start(void)
{
    s_mux = xSemaphoreCreateMutex();
    s_hist = heap_caps_calloc(HIST_N, sizeof(sample_t), MALLOC_CAP_SPIRAM);
    if (!s_mux || !s_hist) return ESP_ERR_NO_MEM;
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("power", NVS_READONLY, &h) == ESP_OK) { if (nvs_get_u8(h, "dnd", &v) == ESP_OK) s_dnd = v; nvs_close(h); }
    load();
    if (xTaskCreate(power_task, "power", 4096, NULL, 1, &s_task) != pdPASS) return ESP_ERR_NO_MEM;
    mole_flash_flusher(s_task);                       /* a restart keeps the history (and the switch) first */
    return ESP_OK;
}

/* ── HTTP ─────────────────────────────────────────────────────────────────── */

/* The gauge's fall per hour over the last hour of this boot on battery (≥ 15 min of it), or NAN. */
static double drain_pct_h(void)
{
    if (!s_n) return NAN;
    const sample_t *last = at(s_n - 1), *first = NULL;
    for (int i = s_n - 1; i >= 0; i--) {
        const sample_t *s = at(i);
        if (s->boot != last->boot || s->pct < 0 || (s->flags & (F_CHARGING | F_USB)) || last->up_s - s->up_s > 3600) break;
        first = s;
    }
    if (!first || last->pct < 0 || last->up_s - first->up_s < 900) return NAN;
    return (first->pct - last->pct) * 3600.0 / (last->up_s - first->up_s);
}

/* The reply as text (PSRAM): the history is up to HIST_N rows, too many for cJSON nodes. */
static char *power_json(int screen)
{
    const size_t cap = 640 + (size_t)HIST_N * 56;
    char *o = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!o) return NULL;
    size_t n = 0;
#define PUT(...) do { int k = snprintf(o + n, cap - n, __VA_ARGS__); n += k > 0 && (size_t)k < cap - n ? (size_t)k : 0; } while (0)
    molty_battery_t b;
    PUT("{\"ok\":true,\"battery\":");
    if (molty_battery_read(&b) != ESP_OK) PUT("null");
    else if (b.pct < 0) PUT("{\"mv\":%d,\"pct\":null,\"charging\":%s,\"usb\":%s}", b.mv, b.charging ? "true" : "false", b.usb ? "true" : "false");
    else PUT("{\"mv\":%d,\"pct\":%d,\"charging\":%s,\"usb\":%s}", b.mv, b.pct, b.charging ? "true" : "false", b.usb ? "true" : "false");
    xSemaphoreTake(s_mux, portMAX_DELAY);
    const double d = drain_pct_h();
    PUT(",\"screen\":%s,\"dnd\":%s,\"drain_pct_h\":", (screen >= 0 ? screen : mole_fluid_screen_on()) ? "true" : "false", s_dnd ? "true" : "false");
    if (d == d) PUT("%.1f", d); else PUT("null");
    PUT(",\"cols\":[\"boot\",\"up_s\",\"mv\",\"pct\",\"charging\",\"usb\",\"link\",\"dnd\",\"screen_s\",\"audio_s\",\"rssi\"],\"history\":[");
    for (int i = 0; i < s_n; i++) {
        const sample_t *s = at(i);
        PUT("%s[%u,%lu,%u,%d,%d,%d,%d,%d,%u,%u,%d]", i ? "," : "", s->boot, (unsigned long)s->up_s, s->mv, s->pct,
            !!(s->flags & F_CHARGING), !!(s->flags & F_USB), !!(s->flags & F_LINK), !!(s->flags & F_DND), s->screen_s, s->audio_s, s->rssi);
    }
    xSemaphoreGive(s_mux);
    PUT("]}");
#undef PUT
    return o;
}

static esp_err_t send(httpd_req_t *r, char *body)
{
    esp_err_t e = http_send_json(r, body ? body : "{\"ok\":false,\"error\":\"oom\"}");
    heap_caps_free(body);
    return e;
}

static esp_err_t http_get(httpd_req_t *r) { return send(r, power_json(-1)); }

static esp_err_t http_post(httpd_req_t *r)
{
    char body[96];
    if (r->content_len <= 0 || r->content_len >= (int)sizeof(body)) return http_send_json(r, "{\"ok\":false,\"error\":\"invalid_args\"}");
    const int got = http_recv_full(r, body, r->content_len);
    body[got > 0 ? got : 0] = 0;
    cJSON *a = got == r->content_len ? cJSON_Parse(body) : NULL;
    const cJSON *scr = cJSON_GetObjectItemCaseSensitive(a, "screen"), *dnd = cJSON_GetObjectItemCaseSensitive(a, "dnd");
    const bool ok = cJSON_IsObject(a) && (!scr || cJSON_IsBool(scr)) && (!dnd || cJSON_IsBool(dnd)) && (scr || dnd);
    const int screen = ok && scr ? cJSON_IsTrue(scr) : -1;
    if (screen >= 0) mole_fluid_screen(screen);
    if (ok && dnd) power_dnd_set(cJSON_IsTrue(dnd));
    cJSON_Delete(a);
    return ok ? send(r, power_json(screen)) : http_send_json(r, "{\"ok\":false,\"error\":\"invalid_args\"}");
}

esp_err_t power_http_register(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        {.uri = "/api/power", .method = HTTP_GET,  .handler = http_get},
        {.uri = "/api/power", .method = HTTP_POST, .handler = http_post},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t e = httpd_register_uri_handler(server, &routes[i]);
        if (e != ESP_OK) return e;
    }
    return ESP_OK;
}
