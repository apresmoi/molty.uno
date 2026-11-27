/*
 * SPDX-License-Identifier: MIT
 *
 * rhythm_dev.c: the beat player on the device (rhythm.h): its lock, the speaker's clock, the audio task's share of
 * the speaker, the buzz on the beats and the link's commands.
 *
 *   speaker   link_audio.c's audio task calls rhythm_dev_play between its jobs: it owns the speaker stream while the
 *             rhythm sounds, writing 16 ms blocks (each write waits for room in the DMA ring, which paces it). A job
 *             (a say, a listen, the chirp) waiting ends it: the rhythm fades (RHY_FADE), the ring drains, the stream
 *             ends, the job runs; after it the rhythm comes back with a count-in at the bar it left.
 *   clock     what the speaker plays at a moment: the samples made when the stream began, plus the newest write less
 *             the DMA ring, plus the time since it (rhythm_heard, smoothed by rhy_clock_t).
 *   buzz      with buzz on, each main beat buzzes the motor RHYTHM_BUZZ_LEAD before it is heard (30 ms, the bar's first
 *             stronger): dropped like any buzz (no motor, the mic open).
 */
#include "rhythm.h"
#include "rhythm_dev.h"
#include "wave.h"
#include "link_cmd.h"
#include "molty_board.h"
#include "haptic.h"
#include "mole_diag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "nvs.h"
#include <stdatomic.h>
#include <string.h>

static SemaphoreHandle_t s_lock;
static atomic_bool s_streaming;
static _Atomic uint64_t s_base;                  /* samples made when the stream began */
static _Atomic uint64_t s_written;               /* ... written since */

void rhythm_lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
void rhythm_unlock(void) { xSemaphoreGive(s_lock); }

/* the trim, kept in NVS (rhythm.h) */
void rhythm_sync_saved(int ms) {
    nvs_handle_t h;
    if (nvs_open("rhythm", NVS_READWRITE, &h) == ESP_OK) { nvs_set_i32(h, "sync", ms); nvs_commit(h); nvs_close(h); }
}
static int sync_kept(void) {
    nvs_handle_t h;
    int32_t ms = 0;
    if (nvs_open("rhythm", NVS_READONLY, &h) == ESP_OK) { nvs_get_i32(h, "sync", &ms); nvs_close(h); }
    return (int)ms;
}

esp_err_t rhythm_dev_init(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    rhythm_sync_init(sync_kept());
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

/* The sample heard ahead_us from now (inside the lock: never takes it). The newest write's measure: the samples written
 * less what waits in the DMA ring (molty_speaker_stream_played: the whole ring, I2S_CHANNEL_DEFAULT_CONFIG's 6 x 240
 * frames, 90 ms), plus RHYTHM_DMA_HALF_DESC (a write returns when a buffer frees, so on average half of one is not yet
 * queued ahead of it), plus the time since that write; smoothed by rhy_clock_heard. */
#define RHYTHM_RING      1440                    /* a board that cannot tell: the default ring, frames */
#define RHYTHM_DMA_HALF_DESC 120                 /* half a 240-frame DMA buffer (7.5 ms)                */
static rhy_clock_t s_clk;
static _Atomic int64_t s_wt;                     /* when the last write returned                       */
uint64_t rhythm_heard(int32_t ahead_us) {
    if (!atomic_load(&s_streaming)) return UINT64_MAX;   /* (rhy_view clamps to what was made: all of it was heard) */
    const uint64_t written = atomic_load(&s_written);
    if (!written) return atomic_load(&s_base);
    const uint32_t played = molty_speaker_stream_played();
    const double behind = played ? (double)(written - played) : RHYTHM_RING, now = (double)esp_timer_get_time();
    const double measured = (double)(atomic_load(&s_base) + written) - behind + RHYTHM_DMA_HALF_DESC + (now - (double)atomic_load(&s_wt)) * (RHY_RATE / 1e6);
    const double n = rhy_clock_heard(&s_clk, measured, (int64_t)now) + ahead_us * (RHY_RATE / 1e6);
    return n > 0.0 ? (uint64_t)n : 0;
}

#define BLOCK 256                                /* 16 ms */
void rhythm_dev_play(bool (*job_waiting)(void)) {
    rhythm_yield(false);                          /* back from a job: the count-in, then on */
    if (!rhythm_sounding()) return;
    if (molty_speaker_stream_begin(RHY_RATE) != ESP_OK) { vTaskDelay(pdMS_TO_TICKS(50)); return; }
    atomic_store(&s_base, rhythm_made());
    atomic_store(&s_written, 0);
    s_clk.on = false;                             /* (a new stream: the clock steps to it) */
    atomic_store(&s_streaming, true);
    int16_t buf[BLOCK];
    esp_err_t e = ESP_OK;
    bool yielded = false;
    unsigned buzzes = 0;
    const int64_t t0 = esp_timer_get_time();
    int64_t busy = 0;
    for (;;) {
        if (!yielded && job_waiting()) { rhythm_yield(true); yielded = true; }   /* a say, a listen: it fades, then gives way */
        const int64_t f0 = esp_timer_get_time();
        const size_t n = rhythm_fill(buf, BLOCK);
        busy += esp_timer_get_time() - f0;
        if (!n) break;
        if ((e = molty_speaker_stream_write(buf, n)) != ESP_OK) break;
        atomic_fetch_add(&s_written, n);
        atomic_store(&s_wt, esp_timer_get_time());
        bool first;
        rhythm_lock();
        const uint64_t heard = rhythm_heard(0);
        rhythm_unlock();
        while (rhythm_take_buzz(heard + RHYTHM_BUZZ_LEAD, &first)) {
            haptic_plan_t pl;
            if (haptic_plan_one(&pl, 30, first ? 100 : 70) && haptic_buzz(&pl) == ESP_OK) buzzes++;
        }
    }
    if (e == ESP_OK) e = molty_speaker_stream_drain();
    atomic_store(&s_streaming, false);
    molty_speaker_stream_end();
    const int64_t took = esp_timer_get_time() - t0;
#ifdef ESP_PLATFORM
    const unsigned stack_free = (unsigned)uxTaskGetStackHighWaterMark(NULL);
#else
    const unsigned stack_free = 0;
#endif
    /* its share of the task's time (the fill: the drums and the mix), and the stack left */
    mole_diag_event(e != ESP_OK, "rhythm.play %s %s took=%ums cpu=%u.%02u%% buzz=%u stack_free=%u", yielded ? "yielded" : "ended",
                    esp_err_to_name(e), (unsigned)(took / 1000), took ? (unsigned)(busy * 100 / took) : 0,
                    took ? (unsigned)(busy * 10000 / took % 100) : 0, buzzes, stack_free);
}

static esp_err_t answer(const char *err) {
    if (!err) return ESP_OK;
    if (!strcmp(err, "invalid_args")) return ESP_ERR_INVALID_ARG;
    if (!strcmp(err, "oom")) return ESP_ERR_NO_MEM;
    return link_cmd_refuse(err);
}
esp_err_t rhythm_cmd_play(const cJSON *args, cJSON **out) {
    if (!molty_board_has(MOLTY_CAP_SPEAKER)) return ESP_ERR_NOT_SUPPORTED;
    return answer(rhythm_play(args, out));
}
esp_err_t rhythm_cmd_set(const cJSON *args, cJSON **out) {
    if (!molty_board_has(MOLTY_CAP_SPEAKER)) return ESP_ERR_NOT_SUPPORTED;
    return answer(rhythm_set(args, out));
}
esp_err_t rhythm_cmd_stop(const cJSON *args, cJSON **out) {
    (void)args; (void)out;
    if (!molty_board_has(MOLTY_CAP_SPEAKER)) return ESP_ERR_NOT_SUPPORTED;
    return answer(rhythm_stop());
}
esp_err_t wave_cmd_play(const cJSON *args, cJSON **out) {
    if (!molty_board_has(MOLTY_CAP_SPEAKER)) return ESP_ERR_NOT_SUPPORTED;
    return answer(wave_play(args, out));
}
esp_err_t wave_cmd_set(const cJSON *args, cJSON **out) {
    if (!molty_board_has(MOLTY_CAP_SPEAKER)) return ESP_ERR_NOT_SUPPORTED;
    return answer(wave_set(args, out));
}
esp_err_t wave_cmd_stop(const cJSON *args, cJSON **out) {
    (void)args; (void)out;
    if (!molty_board_has(MOLTY_CAP_SPEAKER)) return ESP_ERR_NOT_SUPPORTED;
    return answer(wave_stop());
}
