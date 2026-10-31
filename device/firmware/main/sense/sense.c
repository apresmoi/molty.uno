/*
 * SPDX-License-Identifier: MIT
 *
 * sense.c — device events (sense.h). One small task, 100 Hz on core 0 (PSRAM stack;
 * no flash access here):
 *   imu      the board's cached IMU sample (a new one every 20 ms) → gesture.c's shake
 *            detector; boards without "imu" skip it
 *   buttons  press edges of the accept / reject buttons
 *   touch    tap edges handed over by sense_touch (the fluid task polls touch)
 *   status   every 0.5 s: Wi-Fi / relay / auth → mole_fluid_set_status (the edge mark)
 * A tap while a question (link `ask`) is pending is its answer (link_ask.c), not a tap event.
 * Nothing leaves while a pairing is pending (its tap and buttons decide the pairing) or
 * while the link is down; events are not queued for later. A shake with "shake to talk" on
 * goes to the link task (link_audio_shake): Molty starts listening at once and the shake
 * event names that listen. Rate limits on the device:
 * ≤ 1 shake per 2 s, ≤ 4 taps and ≤ 4 button presses per second.
 */
#include "sense.h"
#include "gesture.h"
#include "molty_board.h"
#include "mole_fluid.h"
#include "fluid_director.h"
#include "link_client.h"
#include "link_pair.h"
#include "link_ask.h"
#include "link_protocol.h"
#include "link_audio.h"
#include "mole_diag.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <stdatomic.h>
#include <stdbool.h>

#define SENSE_PERIOD_MS  10      /* twice the IMU cache rate: no sample is skipped        */
#define STATUS_MS        500
#define TAP_MIN_MS       250     /* taps ≤ 4/s                                             */
#define BUTTON_MIN_MS    250
#define SHAKE_MIN_MS     2000    /* ≤ 1 shake per 2 s (the detector's cooldown, enforced)  */

static atomic_bool     s_tap;
static _Atomic uint32_t s_tap_xy;            /* x << 16 | y, each 0..65535 of the panel    */

void sense_touch(int n, const uint16_t *xs, const uint16_t *ys)
{
    static bool down;
    if (n > 0 && !down && !link_pair_active()) {
        int w = molty_display_width(), h = molty_display_height();
        /* a pending question takes the tap as its answer: no `tap` event for it */
        if (link_ask_tap((float)xs[0] + 0.5f - w * 0.5f, (float)ys[0] + 0.5f - h * 0.5f)) { down = true; return; }
        uint32_t x = w > 1 ? (uint32_t)xs[0] * 65535u / (uint32_t)(w - 1) : 0;
        uint32_t y = h > 1 ? (uint32_t)ys[0] * 65535u / (uint32_t)(h - 1) : 0;
        atomic_store(&s_tap_xy, (x > 65535u ? 65535u : x) << 16 | (y > 65535u ? 65535u : y));
        atomic_store(&s_tap, true);
    }
    down = n > 0;
}

static void emit(cJSON *j)
{
    if (j && link_client_connected()) link_send_json(j);
    cJSON_Delete(j);
}

static void sense_task(void *arg)
{
    (void)arg;
    gesture_t g;
    gesture_init(&g, NULL);
    gesture_limit_t tap_l = { .min_ms = TAP_MIN_MS }, btn_l = { .min_ms = BUTTON_MIN_MS },
                    shake_l = { .min_ms = SHAKE_MIN_MS };
    const bool imu = molty_board_has(MOLTY_CAP_IMU);
    bool prev[2] = { molty_button_pressed(MOLTY_BTN_ACCEPT), molty_button_pressed(MOLTY_BTN_REJECT) };
    uint32_t last_imu = 0, next_status = 0;
    const TickType_t period = pdMS_TO_TICKS(SENSE_PERIOD_MS) > 0 ? pdMS_TO_TICKS(SENSE_PERIOD_MS) : 1;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        bool pairing = link_pair_active();
        if ((int32_t)(now - next_status) >= 0) {
            bool wifi, relay, auth;
            link_client_health(&wifi, &relay, &auth);
            mole_fluid_set_status(fd_status_from(wifi, relay, auth));
            next_status = now + STATUS_MS;
        }

        molty_imu_sample_t s;
        if (imu && molty_imu_get(&s) && s.ms != last_imu) {
            last_imu = s.ms;
            link_audio_imu(s.ax, s.ay, s.az, s.ms);       /* a shake's listen: has the handling stopped? */
            gesture_shake_t sh;
            if (pairing) gesture_reset(&g);
            else if (gesture_feed(&g, s.ax, s.ay, s.az, s.ms, &sh)) {
                mole_fluid_event(MOLE_FLUID_SHAKE);          /* reacts with or without an agent */
                mole_diag_event(false, "sense.shake peak=%dmg reversals=%d ms=%u",
                                (int)(sh.peak_g * 1000.f), sh.reversals, (unsigned)sh.ms);
                /* shake to talk (the console's setting, on out of the box): the link task starts
                 * listening and sends the event; otherwise the plain shake goes from here */
                if (gesture_limit_allow(&shake_l, now) && !link_audio_shake(sh.peak_g, sh.reversals, sh.ms))
                    emit(link_evt_shake_frame(sh.peak_g, sh.reversals, sh.ms, NULL));
            }
        }

        for (int b = 0; b < 2; b++) {
            bool on = molty_button_pressed((molty_button_t)b);
            if (on && !prev[b] && !pairing) {
                mole_fluid_event(MOLE_FLUID_BUTTON);
                if (gesture_limit_allow(&btn_l, now)) emit(link_evt_button_frame(b == MOLTY_BTN_ACCEPT));
            }
            prev[b] = on;
        }

        if (atomic_exchange(&s_tap, false) && !link_pair_active() && gesture_limit_allow(&tap_l, now)) {
            uint32_t xy = atomic_load(&s_tap_xy);
            emit(link_evt_tap_frame((xy >> 16) / 65535.0, (xy & 0xFFFFu) / 65535.0));
        }
        xTaskDelayUntil(&wake, period);
    }
}

esp_err_t sense_start(void)
{
    /* PSRAM stack like link_audio: it sends over TLS (link_send_json), never touches flash */
    return xTaskCreatePinnedToCoreWithCaps(sense_task, "sense", 6144, NULL, 2, NULL, 0, MALLOC_CAP_SPIRAM) == pdPASS
        ? ESP_OK : ESP_ERR_NO_MEM;
}
