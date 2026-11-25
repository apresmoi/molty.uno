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
 *   clock     what the speaker plays now: the samples made when the stream began, plus molty_speaker_stream_played.
 *   buzz      with buzz on, each main beat buzzes the motor RHYTHM_BUZZ_LEAD before it is heard (30 ms, the bar's first
 *             stronger): dropped like any buzz (no motor, the mic open).
 */
#include "rhythm.h"
#include "rhythm_dev.h"
#include "link_cmd.h"
#include "molty_board.h"
#include "haptic.h"
#include "mole_diag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <stdatomic.h>
#include <string.h>

static SemaphoreHandle_t s_lock;
static atomic_bool s_streaming;
static _Atomic uint64_t s_base;                  /* samples made when the stream began */
static _Atomic uint64_t s_written;               /* ... written since */

void rhythm_lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
void rhythm_unlock(void) { xSemaphoreGive(s_lock); }

esp_err_t rhythm_dev_init(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

uint64_t rhythm_heard(void) {   /* (inside the lock: never takes it) */
    if (!atomic_load(&s_streaming)) return UINT64_MAX;   /* (rhy_view clamps to what was made: all of it was heard) */
    const uint64_t played = molty_speaker_stream_played(), written = atomic_load(&s_written);
    /* a board that cannot tell: what was written less the DMA ring (about 90 ms) */
    if (!played && written > 2 * 1440) return atomic_load(&s_base) + written - 1440;
    return atomic_load(&s_base) + played;
}

#define BLOCK 256                                /* 16 ms */
void rhythm_dev_play(bool (*job_waiting)(void)) {
    rhythm_yield(false);                          /* back from a job: the count-in, then on */
    if (!rhythm_sounding()) return;
    if (molty_speaker_stream_begin(RHY_RATE) != ESP_OK) { vTaskDelay(pdMS_TO_TICKS(50)); return; }
    atomic_store(&s_base, rhythm_made());
    atomic_store(&s_written, 0);
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
        bool first;
        while (rhythm_take_buzz(rhythm_heard() + RHYTHM_BUZZ_LEAD, &first)) {
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
