/*
 * SPDX-License-Identifier: MIT
 *
 * web_sdk.h: the chip's SDK as the fluid (main/fluid/mole_fluid.c) uses it, for a computer
 * and for WebAssembly: one thread, no flash, no radio. Every other header in this folder (the
 * SDK's names: esp_timer.h, freertos/task.h, nvs.h, ...) is this one. web_sdk.c is the other half.
 *
 * Only what mole_fluid.c calls is here. A call it gains later fails the build until it is
 * given a meaning here: nothing is ever drawn by a stand-in.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── esp_err.h ───────────────────────────────────────────────────────────── */
typedef int esp_err_t;
#define ESP_OK                 0
#define ESP_FAIL               (-1)
#define ESP_ERR_NO_MEM         0x101
#define ESP_ERR_INVALID_ARG    0x102
#define ESP_ERR_INVALID_STATE  0x103
#define ESP_ERR_INVALID_SIZE   0x104
#define ESP_ERR_NOT_FOUND      0x105
#define ESP_ERR_NOT_SUPPORTED  0x106
#define ESP_ERR_TIMEOUT        0x107
#define ESP_ERR_NVS_NOT_FOUND  0x1102
const char *esp_err_to_name(esp_err_t e);
#define ESP_ERROR_CHECK(x) do { if ((x) != ESP_OK) abort(); } while (0)

/* ── esp_log.h: the log goes nowhere ─────────────────────────────────────── */
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGD(tag, ...) ((void)(tag))

/* ── esp_timer.h: the clock is the caller's (web_sdk_clock) ───────────────── */
int64_t esp_timer_get_time(void);
typedef struct web_timer *esp_timer_handle_t;
typedef struct { void (*callback)(void *arg); void *arg; const char *name; } esp_timer_create_args_t;
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out);
esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us);
esp_err_t esp_timer_stop(esp_timer_handle_t t);
esp_err_t esp_timer_delete(esp_timer_handle_t t);

/* ── esp_heap_caps.h: one heap ───────────────────────────────────────────── */
#define MALLOC_CAP_8BIT     (1u << 0)
#define MALLOC_CAP_DMA      (1u << 1)
#define MALLOC_CAP_SPIRAM   (1u << 2)
#define MALLOC_CAP_INTERNAL (1u << 3)
static inline void *heap_caps_malloc(size_t n, uint32_t caps) { (void)caps; return malloc(n); }
static inline void *heap_caps_calloc(size_t k, size_t n, uint32_t caps) { (void)caps; return calloc(k, n); }
static inline void *heap_caps_realloc(void *p, size_t n, uint32_t caps) { (void)caps; return realloc(p, n); }
static inline void  heap_caps_free(void *p) { free(p); }
static inline size_t heap_caps_get_free_size(uint32_t caps) { (void)caps; return 0; }

/* ── esp_random.h, esp_task_wdt.h, esp_private/esp_clk.h ─────────────────── */
uint32_t esp_random(void);                         /* seeded: web_sdk_seed */
static inline esp_err_t esp_task_wdt_add(void *t) { (void)t; return ESP_OK; }
static inline esp_err_t esp_task_wdt_reset(void) { return ESP_OK; }
static inline esp_err_t esp_task_wdt_delete(void *t) { (void)t; return ESP_OK; }
static inline int esp_clk_cpu_freq(void) { return 240000000; }

/* ── nvs.h: nothing is kept, every setting reads as never set ────────────── */
typedef int nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
static inline esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h) { (void)ns; (void)m; (void)h; return ESP_ERR_NVS_NOT_FOUND; }
static inline void      nvs_close(nvs_handle_t h) { (void)h; }
static inline esp_err_t nvs_get_i32(nvs_handle_t h, const char *k, int32_t *v) { (void)h; (void)k; (void)v; return ESP_ERR_NOT_FOUND; }
static inline esp_err_t nvs_set_i32(nvs_handle_t h, const char *k, int32_t v) { (void)h; (void)k; (void)v; return ESP_ERR_NOT_FOUND; }
static inline esp_err_t nvs_erase_key(nvs_handle_t h, const char *k) { (void)h; (void)k; return ESP_ERR_NOT_FOUND; }
static inline esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_ERR_NOT_FOUND; }

/* ── esp_http_server.h: names only (mole_diag.h mentions them) ───────────── */
typedef void *httpd_handle_t;
typedef struct httpd_req httpd_req_t;

/* ── freertos: one thread. A task is a function the caller runs (web_sdk_task); a semaphore
 *    is a count, and taking an empty one for ever is a bug here (it aborts) ─────────────── */
typedef int      BaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE         1
#define pdFALSE        0
#define pdPASS         1
#define portMAX_DELAY  0xffffffffu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
typedef struct web_sem  *SemaphoreHandle_t;
typedef struct web_task *TaskHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
void       vSemaphoreDelete(SemaphoreHandle_t s);
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, uint32_t stack, void *arg, int prio, TaskHandle_t *out, int core);
static inline void vTaskDelete(TaskHandle_t t) { (void)t; }
void vTaskDelay(TickType_t t);
static inline TaskHandle_t xTaskGetCurrentTaskHandle(void) { return NULL; }
static inline BaseType_t xTaskNotifyGive(TaskHandle_t t) { (void)t; return pdPASS; }
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait);

/* ── the caller's side (web_molty.c) ─────────────────────────────────────── */
void web_sdk_seed(uint32_t seed);                  /* esp_random from here on */
void web_sdk_clock(int64_t us);                    /* esp_timer_get_time from here on */
void web_sdk_clock_once(int64_t us);               /* ... but its next answer is this */
/** Run the task `name` (as given to xTaskCreatePinnedToCore) from its top until it returns. */
void web_sdk_task(const char *name);
/** Called when a task would wait (a delay, a notification, a semaphore with a time out). */
extern void (*web_sdk_on_wait)(void);

#ifdef __cplusplus
}
#endif
