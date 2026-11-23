/*
 * SPDX-License-Identifier: MIT
 *
 * haptic.c: haptic.h, the vibration motor: LEDC PWM, a one-shot timer, POST /api/haptic, the link's buzz.
 *
 * No task of its own. The plan and its step are shared by the caller (httpd) and the timer's
 * callback (the esp_timer task) under s_mux, held only for a few register writes. A callback
 * that fired just before a new buzz re-armed the timer finds it active and leaves the new
 * plan alone.
 */
#include "haptic.h"
#include "molty_board.h"
#include "http.h"
#include "link_cmd.h"

#include <stdatomic.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"

static const char *TAG = "haptic";

#define MODE   LEDC_LOW_SPEED_MODE
#define TIMER  LEDC_TIMER_0       /* nothing else in the firmware uses LEDC */
#define CHAN   LEDC_CHANNEL_0
#define BITS   10
#define FREQ   25000              /* Hz: above hearing, no whine from the motor */

static SemaphoreHandle_t   s_mux;  /* NULL: no motor */
static esp_timer_handle_t  s_timer;
static haptic_plan_t       s_plan;
static int                 s_step;
static _Atomic int64_t     s_quiet_until;   /* us, esp_timer clock */

static void motor_off(void) { ledc_stop(MODE, CHAN, 0); }   /* the output idles LOW */

static bool motor_on(uint32_t duty)
{
    if (ledc_set_duty(MODE, CHAN, duty) == ESP_OK && ledc_update_duty(MODE, CHAN) == ESP_OK) return true;
    motor_off();
    return false;
}

/* Start s_step (or the next non-empty one) and arm the timer for its end; past the last, or
 * on any failure, the motor off and the plan over. Under s_mux. */
static void run_step(void)
{
    for (; s_step < s_plan.n; s_step++) {
        const uint16_t ms = s_plan.seg[s_step];
        if (!ms) continue;
        bool ok = true;
        if (s_step & 1) motor_off();
        else ok = motor_on(haptic_plan_duty(&s_plan, BITS));
        if (ok && esp_timer_start_once(s_timer, (uint64_t)ms * 1000) == ESP_OK) return;
        break;
    }
    s_step = s_plan.n;
    motor_off();
}

static void tick(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (!esp_timer_is_active(s_timer)) { s_step++; run_step(); }   /* active: a newer buzz re-armed it */
    xSemaphoreGive(s_mux);
}

void haptic_init(void)
{
    const int pin = molty_haptic_gpio();
    if (pin < 0) return;
    const ledc_timer_config_t t = {
        .speed_mode = MODE, .duty_resolution = BITS, .timer_num = TIMER, .freq_hz = FREQ, .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t c = {
        .gpio_num = pin, .speed_mode = MODE, .channel = CHAN, .timer_sel = TIMER, .duty = 0, .hpoint = 0,
    };
    const esp_timer_create_args_t a = { .callback = tick, .name = "haptic" };
    SemaphoreHandle_t mux = xSemaphoreCreateMutex();
    esp_err_t e = mux ? ledc_timer_config(&t) : ESP_ERR_NO_MEM;
    if (e == ESP_OK) e = ledc_channel_config(&c);
    if (e == ESP_OK) e = esp_timer_create(&a, &s_timer);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "motor on GPIO%d unavailable (%s): held off", pin, esp_err_to_name(e));
        gpio_set_level(pin, 0);
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        if (mux) vSemaphoreDelete(mux);
        return;
    }
    motor_off();
    s_mux = mux;
    ESP_LOGI(TAG, "motor on GPIO%d, %d Hz PWM", pin, FREQ);
}

bool haptic_available(void) { return s_mux != NULL; }

esp_err_t haptic_buzz(const haptic_plan_t *p)
{
    if (!s_mux) return ESP_ERR_NOT_SUPPORTED;
    if (molty_mic_stream_active()) return ESP_ERR_INVALID_STATE;   /* never into the open mic: dropped */
    xSemaphoreTake(s_mux, portMAX_DELAY);
    esp_timer_stop(s_timer);                       /* (not running: harmless) */
    s_plan = *p;
    s_step = 0;
    atomic_store(&s_quiet_until, haptic_plan_quiet_until(p, esp_timer_get_time()));
    run_step();
    xSemaphoreGive(s_mux);
    return ESP_OK;
}

esp_err_t haptic_cue(const char *name)
{
    haptic_plan_t p;
    return haptic_plan_named(&p, name, 100) ? haptic_buzz(&p) : ESP_ERR_INVALID_ARG;
}

esp_err_t haptic_touch(void)
{
    haptic_plan_t p;
    haptic_plan_one(&p, HAPTIC_TOUCH_MS, HAPTIC_TOUCH_STRENGTH);
    return haptic_buzz(&p);
}

bool haptic_quiet(void) { return s_mux && esp_timer_get_time() < atomic_load(&s_quiet_until); }

/* ── POST /api/haptic, the link's buzz ────────────────────────────────────── */

/* a JSON number as whole ms, -1 when not one (or negative); large values saturate (the plan caps them) */
static int ms_of(const cJSON *n)
{
    if (!cJSON_IsNumber(n) || !(n->valuedouble >= 0)) return -1;
    return n->valuedouble > 60000 ? 60000 : (int)n->valuedouble;
}

/* The args (haptic.h) → *p: NULL, or the error ("invalid_args" | "unknown_pattern"). */
static const char *plan_args(const cJSON *a, haptic_plan_t *p)
{
    const cJSON *ms = cJSON_GetObjectItemCaseSensitive(a, "ms"), *pat = cJSON_GetObjectItemCaseSensitive(a, "pattern"),
                *str = cJSON_GetObjectItemCaseSensitive(a, "strength");
    const int strength = !str ? 100 : !cJSON_IsNumber(str) ? -1                 /* (-1: not a number) */
                       : str->valuedouble < 0 ? 0 : str->valuedouble > 100 ? 100 : (int)str->valuedouble;
    if (!cJSON_IsObject(a) || !ms == !pat || strength < 0) return "invalid_args";
    if (ms) return haptic_plan_one(p, ms_of(ms), strength) ? NULL : "invalid_args";
    if (cJSON_IsString(pat)) return haptic_plan_named(p, pat->valuestring, strength) ? NULL : "unknown_pattern";
    int segs[HAPTIC_SEGS], n = cJSON_IsArray(pat) ? cJSON_GetArraySize(pat) : 0;
    bool ok = true;
    for (int i = 0; ok && i < n && i < HAPTIC_SEGS; i++) ok = (segs[i] = ms_of(cJSON_GetArrayItem(pat, i))) >= 0;
    return ok && haptic_plan_pattern(p, segs, n, strength) ? NULL : "invalid_args";
}

/* haptic_buzz's refusal as an error */
static const char *buzz_error(esp_err_t e) { return e == ESP_ERR_INVALID_STATE ? "listening" : "no_motor"; }

/* what started (after the cap) into j: ms, strength, and the segments when a pattern was asked */
static void add_started(cJSON *j, const haptic_plan_t *p, bool segs)
{
    cJSON_AddNumberToObject(j, "ms", haptic_plan_ms(p));
    cJSON_AddNumberToObject(j, "strength", p->strength);
    if (!segs) return;
    cJSON *a = cJSON_AddArrayToObject(j, "pattern");
    for (int i = 0; a && i < p->n; i++) cJSON_AddItemToArray(a, cJSON_CreateNumber(p->seg[i]));
}

static esp_err_t http_fail(httpd_req_t *r, const char *error)
{
    char out[64];
    snprintf(out, sizeof out, "{\"ok\":false,\"error\":\"%s\"}", error);
    return http_send_json(r, out);
}

static esp_err_t http_post(httpd_req_t *r)
{
    if (!s_mux) return http_fail(r, "no_motor");
    char body[160];
    if (r->content_len <= 0 || r->content_len >= (int)sizeof(body)) return http_fail(r, "invalid_args");
    const int got = http_recv_full(r, body, r->content_len);
    body[got > 0 ? got : 0] = 0;
    cJSON *a = got == r->content_len ? cJSON_Parse(body) : NULL;
    haptic_plan_t p;
    const char *bad = plan_args(a, &p);
    const bool segs = cJSON_GetObjectItemCaseSensitive(a, "pattern") != NULL;
    cJSON_Delete(a);
    if (bad) return http_fail(r, bad);
    const esp_err_t e = haptic_buzz(&p);
    if (e != ESP_OK) return http_fail(r, buzz_error(e));
    cJSON *j = cJSON_CreateObject();
    if (!j) return http_fail(r, "no_mem");
    cJSON_AddTrueToObject(j, "ok");
    add_started(j, &p, segs);
    char *out = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!out) return http_fail(r, "no_mem");
    const esp_err_t sent = http_send_json(r, out);
    cJSON_free(out);
    return sent;
}

esp_err_t haptic_http_register(httpd_handle_t server)
{
    const httpd_uri_t route = { .uri = "/api/haptic", .method = HTTP_POST, .handler = http_post };
    return httpd_register_uri_handler(server, &route);
}

/* buzz (cap "haptic"): the args above, at once; never takes the screen. */
esp_err_t haptic_cmd(const cJSON *args, cJSON **out)
{
    if (!s_mux) return link_cmd_refuse("no_motor");
    haptic_plan_t p;
    const char *bad = plan_args(args, &p);
    if (bad) return bad[0] == 'i' ? ESP_ERR_INVALID_ARG : link_cmd_refuse(bad);
    const esp_err_t e = haptic_buzz(&p);
    if (e != ESP_OK) return link_cmd_refuse(buzz_error(e));
    cJSON *j = cJSON_CreateObject();
    if (!j) return ESP_ERR_NO_MEM;
    add_started(j, &p, cJSON_GetObjectItemCaseSensitive(args, "pattern") != NULL);
    *out = j;
    return ESP_OK;
}
