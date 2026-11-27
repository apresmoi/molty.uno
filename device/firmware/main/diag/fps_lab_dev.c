/*
 * SPDX-License-Identifier: MIT
 *
 * fps_lab_dev.c: the frame rate lab on the device (fps_lab_dev.h): its HTTP routes and its task. Its task handle is in
 * PSRAM (EXT_RAM_BSS_ATTR), its stack and control block too: no internal RAM.
 */
#include "fps_lab_dev.h"
#include "fps_lab.h"
#include "mole_diag.h"
#include "http.h"

#include <stdlib.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_private/esp_clk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#define LAB_LOG_US   10000000      /* a knob off its default: the `lab` lines this often                         */
#define LAB_CSV_MAX  (FL_FRAMES - 2)

static EXT_RAM_BSS_ATTR TaskHandle_t s_task;

static float cyc_ms(uint64_t cyc, uint32_t n) { return n ? (float)((double)cyc / n / (esp_clk_cpu_freq() / 1000)) : 0.f; }

cJSON *fps_lab_knobs_json(void) {
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    for (int k = 0; k < FL_N; k++) cJSON_AddNumberToObject(o, fps_lab_name(k), fps_lab(k));
    cJSON_AddBoolToObject(o, "nondefault", fps_lab_nondefault());
    return o;
}

static const char *const HOW[3] = { "raster", "same", "cached" };

static cJSON *lab_json(void) {
    cJSON *j = cJSON_CreateObject();
    if (!j) return NULL;
    cJSON_AddTrueToObject(j, "ok");
    cJSON_AddItemToObject(j, "knobs", fps_lab_knobs_json());
    cJSON *def = cJSON_AddObjectToObject(j, "defaults");
    for (int k = 0; k < FL_N; k++) cJSON_AddNumberToObject(def, fps_lab_name(k), fps_lab_default(k));
    const cd_reuse_t *c = fps_lab_creature();
    cJSON_AddNumberToObject(j, "psram", fps_lab_bytes() + (c ? c->bytes : 0));
    fl_sum_t s;
    if (fps_lab_summary(&s)) {   /* (us, but wins px changed owed: counts) */
        static const char *const KIND[3] = { "other", "creature", "scene" };
        cJSON *f = cJSON_AddObjectToObject(j, "frames");
        cJSON_AddNumberToObject(f, "n", s.n); cJSON_AddNumberToObject(f, "span_s", s.span_s); cJSON_AddStringToObject(f, "kind", KIND[s.kind]);
        for (int m = 0; m < FM_N; m++) {
            cJSON *o = cJSON_AddObjectToObject(f, FL_METRIC_NAMES[m]);
            cJSON_AddNumberToObject(o, "mean", s.m[m].mean); cJSON_AddNumberToObject(o, "p50", s.m[m].p50); cJSON_AddNumberToObject(o, "p95", s.m[m].p95);
            cJSON_AddNumberToObject(o, "p99", s.m[m].p99); cJSON_AddNumberToObject(o, "max", s.m[m].max);
        }
        const int wake[4] = { (int)s.wake[0], (int)s.wake[1], (int)s.wake[2], (int)s.wake[3] }, pres[4] = { (int)s.pres[0], (int)s.pres[1], (int)s.pres[2], (int)s.pres[3] };
        cJSON_AddItemToObject(f, "wake_pulses", cJSON_CreateIntArray(wake, 4));   /* 1 or less, 2, 3, 4 or more */
        cJSON_AddItemToObject(f, "pres_pulses", cJSON_CreateIntArray(pres, 4));
        cJSON_AddNumberToObject(f, "late", s.late); cJSON_AddNumberToObject(f, "late_pres", s.late_pres); cJSON_AddNumberToObject(f, "repeats", s.repeats);
        cJSON *t = cJSON_AddObjectToObject(f, "tracked");
        cJSON_AddNumberToObject(t, "track", fps_lab(FL_TRACK));
        cJSON_AddStringToObject(t, "shapes", fps_lab_shapes());   /* (a letter a shape: the index for track) */
        cJSON_AddNumberToObject(t, "moves", s.moves); cJSON_AddNumberToObject(t, "moves_s", s.moves_s);
        cJSON_AddNumberToObject(t, "done", s.done); cJSON_AddNumberToObject(t, "done_s", s.done_s); cJSON_AddNumberToObject(t, "hold_pulses", s.hold);
    }
    fl_pose_sum_t p;
    if (fps_lab_pose_summary(&p)) {
        cJSON *o = cJSON_AddObjectToObject(j, "pose");
        cJSON_AddNumberToObject(o, "span_s", p.span_s); cJSON_AddNumberToObject(o, "n", p.n); cJSON_AddNumberToObject(o, "n_gesture", p.ng);
        cJSON_AddNumberToObject(o, "same", p.same); cJSON_AddNumberToObject(o, "same_gesture", p.same_g);
        cJSON_AddNumberToObject(o, "distinct", p.distinct); cJSON_AddNumberToObject(o, "distinct_idle", p.distinct_i); cJSON_AddNumberToObject(o, "distinct_gesture", p.distinct_g);
        cJSON_AddNumberToObject(o, "distinct_per_min", p.span_s > 0.f ? p.distinct * 60.f / p.span_s : 0.f);
        cJSON *top = cJSON_AddArrayToObject(o, "top");
        for (int i = 0; i < p.ntop; i++) { cJSON *e = cJSON_CreateObject(); cJSON_AddNumberToObject(e, "n", p.top[i].n); cJSON_AddBoolToObject(e, "gesture", p.top[i].g); cJSON_AddItemToArray(top, e); }
        cJSON *lru = cJSON_AddObjectToObject(o, "lru_hit");
        for (int i = 0; i < FL_LRU_N; i++) { char k[8]; snprintf(k, sizeof(k), "%d", FL_LRU[i]); cJSON_AddNumberToObject(lru, k, p.lru[i]); }
        cJSON_AddNumberToObject(o, "total", p.total); cJSON_AddNumberToObject(o, "total_same", p.total_same);
    }
    if (c) {
        cJSON *o = cJSON_AddObjectToObject(j, "creature");
        cJSON_AddStringToObject(o, "last", HOW[c->last < 3 ? c->last : 0]);
        cJSON_AddNumberToObject(o, "bytes", c->bytes);
        for (int h = 0; h < 3; h++) {
            cJSON *e = cJSON_AddObjectToObject(o, HOW[h]);
            cJSON_AddNumberToObject(e, "frames", c->frames[h]); cJSON_AddNumberToObject(e, "gesture_frames", c->gframes[h]);
            cJSON_AddNumberToObject(e, "core1_ms", cyc_ms(c->cyc[h], c->frames[h])); cJSON_AddNumberToObject(e, "band_ms", cyc_ms(c->band[h], c->frames[h]));
        }
        const uint32_t n = c->frames[0] + c->frames[1] + c->frames[2];
        const float raster = cyc_ms(c->cyc[0] + c->band[0], c->frames[0]);
        cJSON_AddNumberToObject(o, "saved_ms_same", c->frames[1] ? raster - cyc_ms(c->cyc[1] + c->band[1], c->frames[1]) : 0.f);
        cJSON_AddNumberToObject(o, "saved_ms_cached", c->frames[2] ? raster - cyc_ms(c->cyc[2] + c->band[2], c->frames[2]) : 0.f);
        cJSON_AddNumberToObject(o, "upkeep_us", cyc_ms(c->upkeep, n) * 1000.f);
    }
    return j;
}

static esp_err_t send_lab(httpd_req_t *r) {
    cJSON *j = lab_json();
    char *out = j ? cJSON_PrintUnformatted(j) : NULL;
    cJSON_Delete(j);
    if (!out) return http_send_json(r, "{\"ok\":false,\"error\":\"no_mem\"}");
    const esp_err_t e = http_send_json(r, out);
    cJSON_free(out);
    return e;
}

static void lab_log(void) {
    char line[136];
    int o = snprintf(line, sizeof(line), "lab knobs");
    for (int k = 0; k < FL_N && o < (int)sizeof(line); k++) o += snprintf(line + o, sizeof(line) - (size_t)o, " %s=%d", fps_lab_name(k), fps_lab(k));
    const cd_reuse_t *c = fps_lab_creature();
    mole_diag_event(false, "%s psram=%luKB", line, (unsigned long)((fps_lab_bytes() + (c ? c->bytes : 0)) / 1024));
    fl_sum_t s;
    if (fps_lab_summary(&s)) {
        #define MS(x) s.m[x].p50 / 1000.f, s.m[x].p95 / 1000.f
        mole_diag_event(false, "lab f n=%d sim=%.1f/%.1f dust=%.1f/%.1f cells=%.1f/%.1f send=%.1f/%.1f wait=%.1f/%.1f frame=%.1f/%.1f ms p50/p95",
                        s.n, MS(FM_SIM), MS(FM_DUST), MS(FM_CELLS), MS(FM_SEND), MS(FM_WAIT), MS(FM_FRAME));
        #undef MS
        mole_diag_event(false, "lab t wake=%lu/%lu/%lu/%lu pres=%lu/%lu/%lu/%lu late=%lu/%lu rep=%lu pos=%.1f/s moves=%.1f/s hold=%lu lab=%.3f/%.3fms",
                        (unsigned long)s.wake[0], (unsigned long)s.wake[1], (unsigned long)s.wake[2], (unsigned long)s.wake[3],
                        (unsigned long)s.pres[0], (unsigned long)s.pres[1], (unsigned long)s.pres[2], (unsigned long)s.pres[3],
                        (unsigned long)s.late, (unsigned long)s.late_pres, (unsigned long)s.repeats, s.done_s, s.moves_s, (unsigned long)s.hold,
                        s.m[FM_LAB].mean / 1000.f, s.m[FM_LAB].max / 1000.f);
    }
    fl_pose_sum_t p;
    if (fps_lab_pose_summary(&p))
        mole_diag_event(false, "lab pose n=%d/%d same=%d/%d distinct=%d (idle %d, gesture %d) in %.0fs top=%lu lru=%.0f/%.0f/%.0f/%.0f%%",
                        p.n, p.ng, p.same, p.same_g, p.distinct, p.distinct_i, p.distinct_g, p.span_s, (unsigned long)(p.ntop ? p.top[0].n : 0),
                        100.f * p.lru[0], 100.f * p.lru[1], 100.f * p.lru[2], 100.f * p.lru[3]);
    if (c && (fps_lab(FL_SKIP) || fps_lab(FL_CACHE))) {
        const float raster = cyc_ms(c->cyc[0] + c->band[0], c->frames[0]);
        mole_diag_event(false, "lab reuse raster=%lu same=%lu cached=%lu (gesture %lu/%lu) raster=%.1fms hit=%.1fms upkeep=%.0fus bytes=%lu",
                        (unsigned long)c->frames[0], (unsigned long)c->frames[1], (unsigned long)c->frames[2], (unsigned long)c->gframes[1], (unsigned long)c->gframes[2],
                        raster, cyc_ms(c->cyc[1] + c->band[1] + c->cyc[2] + c->band[2], c->frames[1] + c->frames[2]),
                        cyc_ms(c->upkeep, c->frames[0] + c->frames[1] + c->frames[2]) * 1000.f, (unsigned long)c->bytes);
    }
}

static void lab_task(void *arg) {
    (void)arg;
    int64_t logged = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
        fps_lab_reconcile();
        const int64_t now = esp_timer_get_time();
        if (!fps_lab_nondefault()) { logged = 0; continue; }
        if (!logged) logged = now;                  /* (the first line 10 s after a knob moved: something to summarise) */
        else if (now - logged >= LAB_LOG_US) { logged = now; lab_log(); }
    }
}

static esp_err_t http_get(httpd_req_t *r) {
    char q[24], v[8];
    const int raw = httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK && httpd_query_key_value(q, "raw", v, sizeof(v)) == ESP_OK ? atoi(v) : 0;
    if (raw <= 0) return send_lab(r);
    const int n = raw < LAB_CSV_MAX ? raw : LAB_CSV_MAX, len = 160 * (n + 1);
    char *out = heap_caps_malloc((size_t)len, MALLOC_CAP_SPIRAM);
    if (!out) return http_send_json(r, "{\"ok\":false,\"error\":\"no_mem\"}");
    const int w = fps_lab_csv(out, len, n);
    httpd_resp_set_type(r, "text/csv");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    const esp_err_t e = httpd_resp_send(r, out, w);
    heap_caps_free(out);
    return e;
}

static esp_err_t http_post(httpd_req_t *r) {
    if (r->content_len <= 0 || r->content_len > 512) return http_send_json(r, "{\"ok\":false,\"error\":\"bad_request\"}");
    char body[513];
    const int n = http_recv_full(r, body, r->content_len);
    if (n <= 0) return http_send_json(r, "{\"ok\":false,\"error\":\"recv_failed\"}");
    body[n] = '\0';
    cJSON *j = cJSON_Parse(body);
    if (!cJSON_IsObject(j)) { cJSON_Delete(j); return http_send_json(r, "{\"ok\":false,\"error\":\"bad_json\"}"); }
    const cJSON *it;
    cJSON_ArrayForEach(it, j) {   /* (all checked before any is set: a bad one sets none) */
        if (!strcmp(it->string, "reset")) continue;
        const int e = !cJSON_IsNumber(it) ? -3 : fps_lab_check(it->string, it->valueint);
        if (e < 0) {
            char err[96];
            snprintf(err, sizeof(err), "{\"ok\":false,\"error\":\"%s\",\"knob\":\"%.32s\"}", e == -1 ? "unknown_knob" : e == -2 ? "out_of_range" : "not_a_number", it->string);
            cJSON_Delete(j);
            return http_send_json(r, err);
        }
    }
    const cJSON *rs = cJSON_GetObjectItemCaseSensitive(j, "reset");
    if (cJSON_IsTrue(rs) || (cJSON_IsNumber(rs) && rs->valueint)) fps_lab_reset();
    cJSON_ArrayForEach(it, j) if (strcmp(it->string, "reset")) fps_lab_set(it->string, it->valueint);
    cJSON_Delete(j);
    if (!s_task && xTaskCreatePinnedToCoreWithCaps(lab_task, "fps_lab", 3584, NULL, 1, &s_task, tskNO_AFFINITY, MALLOC_CAP_SPIRAM) != pdPASS) s_task = NULL;
    if (s_task) { xTaskNotifyGive(s_task); vTaskDelay(pdMS_TO_TICKS(150)); }   /* (its rings made before the answer) */
    else fps_lab_reconcile();
    mole_diag_event(false, "lab set: %s", fps_lab_nondefault() ? "knobs off their defaults" : "all at their defaults");
    return send_lab(r);
}

esp_err_t fps_lab_http_register(httpd_handle_t server) {
    const httpd_uri_t routes[] = {
        { .uri = "/api/lab", .method = HTTP_GET, .handler = http_get },
        { .uri = "/api/lab", .method = HTTP_POST, .handler = http_post },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        const esp_err_t e = httpd_register_uri_handler(server, &routes[i]);
        if (e != ESP_OK) return e;
    }
    return ESP_OK;
}
