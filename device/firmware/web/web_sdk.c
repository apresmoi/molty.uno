/*
 * SPDX-License-Identifier: MIT
 *
 * web_sdk.c: the chip's SDK on one thread (sdk/web_sdk.h): a clock and a random source the
 * caller sets, semaphores that are counts, tasks that are functions the caller runs.
 */
#include "web_sdk.h"

#include <string.h>

/* ── the clock ───────────────────────────────────────────────────────────── */
static int64_t s_now, s_once;
static bool    s_once_set;

void web_sdk_clock(int64_t us) { s_now = us; }
void web_sdk_clock_once(int64_t us) { s_once = us; s_once_set = true; }
int64_t esp_timer_get_time(void) {
    if (s_once_set) { s_once_set = false; return s_once; }
    return s_now;
}

struct web_timer { int unused; };
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out) {
    static struct web_timer t;
    (void)args; *out = &t;
    return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us) { (void)t; (void)us; return ESP_OK; }
esp_err_t esp_timer_stop(esp_timer_handle_t t) { (void)t; return ESP_OK; }
esp_err_t esp_timer_delete(esp_timer_handle_t t) { (void)t; return ESP_OK; }

/* ── random: xorshift32 from the caller's seed ───────────────────────────── */
static uint32_t s_rng = 0x6d6f6c74u;
void web_sdk_seed(uint32_t seed) { s_rng = seed ? seed : 0x6d6f6c74u; }
uint32_t esp_random(void) {
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}

const char *esp_err_to_name(esp_err_t e) { return e == ESP_OK ? "ESP_OK" : "ESP_ERR"; }

/* ── waiting: nobody else runs, so a task that would wait hands the thread back: the caller is told
 *    (web_sdk_on_wait) and the wait is over at once ─────────────────────────────────────── */
void (*web_sdk_on_wait)(void);
static void waits(void) { if (web_sdk_on_wait) web_sdk_on_wait(); }
void vTaskDelay(TickType_t t) { (void)t; waits(); }
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait) { (void)clear; (void)wait; waits(); return 0; }

/* ── semaphores: counts. One that is empty stays empty: a wait with a time out is a wait
 *    (above) that fails, a wait for ever is a deadlock (abort) ─────────────────────────── */
struct web_sem { int n; };
static SemaphoreHandle_t sem(int n) {
    struct web_sem *s = malloc(sizeof(*s));
    if (s) s->n = n;
    return s;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return sem(1); }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return sem(0); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait) {
    if (s->n > 0) { s->n--; return pdTRUE; }
    if (wait == portMAX_DELAY) abort();
    waits();
    return pdFALSE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { s->n = 1; return pdTRUE; }
void vSemaphoreDelete(SemaphoreHandle_t s) { free(s); }

/* ── tasks: kept by name, run by the caller, from the top each time ───────── */
#define WEB_TASKS 4
struct web_task { const char *name; void (*fn)(void *); void *arg; };
static struct web_task s_tasks[WEB_TASKS];

BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, uint32_t stack, void *arg, int prio, TaskHandle_t *out, int core) {
    (void)stack; (void)prio; (void)core;
    struct web_task *t = NULL;
    for (int i = 0; i < WEB_TASKS && !t; i++) if (!s_tasks[i].fn || !strcmp(s_tasks[i].name, name)) t = &s_tasks[i];
    if (!t) return pdFALSE;
    *t = (struct web_task){ name, fn, arg };
    if (out) *out = t;
    return pdPASS;
}

void web_sdk_task(const char *name) {
    for (int i = 0; i < WEB_TASKS; i++)
        if (s_tasks[i].fn && !strcmp(s_tasks[i].name, name)) { s_tasks[i].fn(s_tasks[i].arg); return; }
}
