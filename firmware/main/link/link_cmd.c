#include "link_cmd.h"
#include "link_protocol.h"
#include "link_audio.h"
#include "link_pair.h"
#include "mole_creature.h"
#include "molty_board.h"
#include "sdkconfig.h"
#include "mole_fluid.h"
#include "mole_diag.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static struct { const char *name; link_cmd_handler_t handler; } s_registry[16];
static size_t s_count;
static char s_mood[16] = "idle";
/* Bumped by every explicit mood (the link's `mood` command, a local POST with a mood)
 * that the creature accepted. Audio jobs snapshot it so their completion only falls
 * back to idle when nobody chose a mood meanwhile. Not bumped by the moods the device
 * sets for itself (listening / speaking / idle). */
static atomic_uint s_mood_gen;

/* Applying an explicit mood + counting it, and a job's end checking the count +
 * writing idle, run on different tasks (local HTTP server, link task). One lock over
 * both pairs, so idle can never land between an explicit mood and its count. Created
 * on first use: the HTTP API works even when the link never started. */
static _Atomic(SemaphoreHandle_t) s_mood_lock;

static void mood_lock(void)
{
    SemaphoreHandle_t m = atomic_load(&s_mood_lock);
    if (!m) {
        SemaphoreHandle_t fresh = xSemaphoreCreateMutex(), none = NULL;
        configASSERT(fresh);
        if (atomic_compare_exchange_strong(&s_mood_lock, &none, fresh)) m = fresh;
        else { vSemaphoreDelete(fresh); m = none; }
    }
    xSemaphoreTake(m, portMAX_DELAY);
}
static void mood_unlock(void) { xSemaphoreGive(atomic_load(&s_mood_lock)); }

unsigned link_cmd_mood_gen(void) { return atomic_load(&s_mood_gen); }

static void note_state_body(const char *body)
{
    if (!body || !strstr(body, "\"mood\"")) return;     /* cheap: most bodies carry no mood */
    cJSON *j = cJSON_Parse(body);
    if (cJSON_IsString(cJSON_GetObjectItemCaseSensitive(j, "mood"))) atomic_fetch_add(&s_mood_gen, 1);
    cJSON_Delete(j);
}

esp_err_t link_cmd_state_set_explicit(const char *body)
{
    mood_lock();
    esp_err_t e = mole_creature_state_set(body);
    if (e == ESP_OK) note_state_body(body);
    mood_unlock();
    return e;
}

void link_cmd_mood_idle_unless(unsigned gen0)
{
    mood_lock();
    if (atomic_load(&s_mood_gen) == gen0) link_cmd_mood("idle", -1);
    mood_unlock();
}

esp_err_t link_cmd_register(const char *name, link_cmd_handler_t handler)
{
    if (!name || !handler) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < s_count; i++) {
        if (!strcmp(name, s_registry[i].name)) { s_registry[i].handler = handler; return ESP_OK; }
    }
    if (s_count == 16) return ESP_ERR_NO_MEM;
    s_registry[s_count].name = name; s_registry[s_count++].handler = handler;
    return ESP_OK;
}

const char *link_cmd_current_mood(void) { return s_mood; }

esp_err_t link_cmd_mood(const char *mood, double hold_s)
{
    const char *state;
    if (!link_mood_map(mood, &state)) return ESP_ERR_INVALID_ARG;
    esp_err_t e = mole_creature_select("fluid");
    if (e != ESP_OK) return e;
    if (state) {
        cJSON *j = cJSON_CreateObject();
        if (!j) return ESP_ERR_NO_MEM;
        cJSON_AddStringToObject(j, "state", state);
        cJSON_AddStringToObject(j, "mood", mood);
        if (hold_s >= 0) cJSON_AddNumberToObject(j, "hold_s", hold_s);
        char *body = cJSON_PrintUnformatted(j); cJSON_Delete(j);
        if (!body) return ESP_ERR_NO_MEM;
        e = mole_creature_state_set(body); free(body);
    }
    if (e == ESP_OK) strcpy(s_mood, mood);
    return e;
}

esp_err_t link_ui_set_text(const char *text, int len)
{
    return mole_fluid_set_text(text, len);   /* the fluid's speed reader owns text */
}

static esp_err_t state_get(cJSON **out)
{
    char *body = NULL;
    esp_err_t e = mole_creature_state_get(&body);
    if (e != ESP_OK) return e;
    *out = cJSON_Parse(body); free(body);
    return *out ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t cmd_status(const cJSON *args, cJSON **out)
{
    (void)args;
    esp_err_t e = state_get(out);
    if (e != ESP_OK) return e;
    cJSON *j = *out;
    cJSON_AddStringToObject(j, "fw", esp_app_get_description()->version);
    cJSON_AddNumberToObject(j, "uptime", esp_timer_get_time() / 1000000);
    /* the mood on screen (a quick mood may have reverted, the creature may doze off);
     * the last commanded one when the fluid reports none */
    cJSON *shown = cJSON_GetObjectItemCaseSensitive(j, "mood");
    if (!cJSON_IsString(shown) || !shown->valuestring[0]) {
        cJSON_DeleteItemFromObjectCaseSensitive(j, "mood");
        cJSON_AddStringToObject(j, "mood", s_mood);
    }
    if (!cJSON_HasObjectItem(j, "creature")) cJSON_AddStringToObject(j, "creature", mole_creature_current());
    if (!cJSON_HasObjectItem(j, "state")) cJSON_AddStringToObject(j, "state", "unknown");
    if (!cJSON_HasObjectItem(j, "fps")) cJSON_AddNumberToObject(j, "fps", 0);
    wifi_ap_record_t ap;
    cJSON_AddNumberToObject(j, "rssi", esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0);
    return ESP_OK;
}

static esp_err_t cmd_state(const cJSON *args, cJSON **out)
{
    if (!args->child) return state_get(out);
    char *body = cJSON_PrintUnformatted(args);
    if (!body) return ESP_ERR_NO_MEM;
    esp_err_t e = mole_creature_state_set(body); free(body);
    return e == ESP_OK ? state_get(out) : e;
}

static esp_err_t cmd_mood(const cJSON *args, cJSON **out)
{
    (void)out;
    const char *mood; double hold_s;
    if (!link_mood_args(args, &mood, &hold_s)) return ESP_ERR_INVALID_ARG;
    mood_lock();
    esp_err_t e = link_cmd_mood(mood, hold_s);
    if (e == ESP_OK) atomic_fetch_add(&s_mood_gen, 1);
    mood_unlock();
    return e;
}

static esp_err_t cmd_show(const cJSON *args, cJSON **out)
{
    (void)out;
    cJSON *text = cJSON_GetObjectItemCaseSensitive(args, "text");
    cJSON *wpm = cJSON_GetObjectItemCaseSensitive(args, "wpm");
    if (!cJSON_IsString(text) || strlen(text->valuestring) > 16384 ||
        (wpm && (!cJSON_IsNumber(wpm) || wpm->valuedouble < 100 || wpm->valuedouble > 800))) return ESP_ERR_INVALID_ARG;
    esp_err_t e = mole_creature_select("fluid");
    if (e != ESP_OK) return e;
    e = link_ui_set_text(text->valuestring, (int)strlen(text->valuestring));
    if (e != ESP_OK) return e;
    cJSON *j = cJSON_CreateObject();
    if (!j) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(j, "state", "text");
    if (wpm) cJSON_AddNumberToObject(j, "wpm", wpm->valuedouble);
    char *body = cJSON_PrintUnformatted(j); cJSON_Delete(j);
    if (!body) return ESP_ERR_NO_MEM;
    e = mole_creature_state_set(body); free(body);
    return e;
}

static esp_err_t cmd_volume(const cJSON *args, cJSON **out)
{
    (void)out;
    cJSON *v = cJSON_GetObjectItemCaseSensitive(args, "volume");
    if (!cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > 21 || v->valuedouble != v->valueint) return ESP_ERR_INVALID_ARG;
    return molty_speaker_set_volume((uint8_t)v->valueint);   /* unsupported without a speaker */
}

static esp_err_t cmd_imu(const cJSON *args, cJSON **out)
{
    (void)args;
    molty_imu_sample_t s;
    if (!molty_imu_get(&s)) return ESP_ERR_NOT_SUPPORTED;
    cJSON *j = cJSON_CreateObject();
    if (!j) return ESP_ERR_NO_MEM;
    cJSON_AddNumberToObject(j, "ax", s.ax); cJSON_AddNumberToObject(j, "ay", s.ay);
    cJSON_AddNumberToObject(j, "az", s.az); cJSON_AddNumberToObject(j, "gx", s.gx);
    cJSON_AddNumberToObject(j, "gy", s.gy); cJSON_AddNumberToObject(j, "gz", s.gz);
    cJSON_AddNumberToObject(j, "ms", s.ms); *out = j;
    return ESP_OK;
}

static esp_err_t cmd_diag(const cJSON *args, cJSON **out)
{
    cJSON *tl = cJSON_GetObjectItemCaseSensitive(args, "timeline");
    if (tl && (!cJSON_IsNumber(tl) || tl->valuedouble < 0 || tl->valuedouble > 180)) return ESP_ERR_INVALID_ARG;
    *out = mole_diag_json(tl ? tl->valueint : 0);
    return *out ? ESP_OK : ESP_ERR_NO_MEM;
}

/* Text frames are capped at 64 KiB on the wire; keep the log reply well under. */
static esp_err_t cmd_log(const cJSON *args, cJSON **out)
{
    cJSON *since = cJSON_GetObjectItemCaseSensitive(args, "since");
    cJSON *max = cJSON_GetObjectItemCaseSensitive(args, "max");
    if ((since && (!cJSON_IsNumber(since) || since->valuedouble < 0)) ||
        (max && (!cJSON_IsNumber(max) || max->valuedouble < 1024 || max->valuedouble > 32768))) return ESP_ERR_INVALID_ARG;
    uint32_t next = 0;
    char *text = mole_diag_log_text(since ? (uint32_t)since->valuedouble : 0, max ? (size_t)max->valueint : 12288, &next, false);
    if (!text) return ESP_ERR_NO_MEM;
    cJSON *j = cJSON_CreateObject();
    if (!j) { heap_caps_free(text); return ESP_ERR_NO_MEM; }
    cJSON_AddNumberToObject(j, "next", next);
    cJSON_AddStringToObject(j, "text", text);
    heap_caps_free(text);
    *out = j;
    return ESP_OK;
}

void link_cmd_init(void)
{
    link_cmd_register("diag", cmd_diag); link_cmd_register("log", cmd_log);
    link_cmd_register("status", cmd_status); link_cmd_register("state", cmd_state);
    link_cmd_register("mood", cmd_mood); link_cmd_register("show", cmd_show);
    link_cmd_register("say", link_audio_say); link_cmd_register("listen", link_audio_listen);
    link_cmd_register("volume", cmd_volume); link_cmd_register("imu", cmd_imu);
}

esp_err_t link_cmd_run(const char *name, const cJSON *args, cJSON **result)
{
    *result = NULL;
    if (!name || !cJSON_IsObject(args)) return ESP_ERR_INVALID_ARG;
    if (link_pair_active()) return ESP_ERR_INVALID_STATE;
    for (size_t i = 0; i < s_count; i++)
        if (!strcmp(name, s_registry[i].name)) return s_registry[i].handler(args, result);
    return ESP_ERR_NOT_FOUND;   /* "unknown_command"; ESP_ERR_NOT_SUPPORTED is "unsupported" (no such hardware) */
}
