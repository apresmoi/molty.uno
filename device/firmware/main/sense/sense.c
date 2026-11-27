/*
 * SPDX-License-Identifier: MIT
 *
 * sense.c — device events (sense.h). One small task, 100 Hz on core 0 (PSRAM stack;
 * no flash access here):
 *   imu      the board's cached IMU sample (a new one every 20 ms) → gesture.c's shake
 *            detector (deaf while the vibration motor runs and settles: haptic_quiet), and
 *            while the screen is off screen_nap.h's pick-up (it ends a nap);
 *            boards without "imu" skip it
 *   buttons  press edges of the accept / reject buttons
 *   touch    tap edges handed over by sense_touch (the fluid task polls touch)
 *   status   every 0.5 s: Wi-Fi / relay / auth → mole_fluid_set_status (the edge mark)
 * A tap while a question (link `ask`) is pending is its answer (link_ask.c), not a tap event.
 * A tap on a scene's spot shows the spot's reading and sends a tap event naming the spot,
 * whatever tap is bound to (a recording still stops first).
 * A tap during a recording stops it and sends (after its first second), a tap while a say
 * waits for its first audio cancels the wait; otherwise tap, double tap and the two buttons do
 * what the human bound them to (gesture.h: none, talk, event, stop, and a button's screen:
 * a click (pressed and let go) switches the screen, a double click do not disturb, held
 * 1.5 s it says goodbye and powers off (power_off); or hold: held to talk; pairing
 * and a pending question always come first). A talk names its input on the shake event. A shake's phantom taps are dropped (gesture_tap_guarded).
 * A press on a scene's wheel, its centre or a beat's dot (mole_fluid_zone) is the fluid task's alone: no spot, no tap
 * event; the centre and a dot buzz as a tap that does something, the wheel ticks once a click with its own firmer
 * tick (mole_fluid_take_ticks, haptic_wheel: at most one every HAPTIC_WHEEL_GAP_MS, so a fast turn feels fewer ticks
 * than it makes). It sends a `ring` event once it settles. While a skill's scene shows the reject button closes it
 * (mole_fluid_scene_close), whatever it is bound to; with a rhythm or a chord on it the accept button is its centre
 * (plays and stops it). A finger held still on the glass closes it too (mole_fluid.c), with the touch buzz.
 * A tap that does something buzzes the motor gently at once (acted): on a spot or beside an opened layout's spots;
 * a question's and a pairing's answers where they are taken (link_ask.c, link_pair.c). Never the water (a plain
 * tap, a drag), never a physical button (it clicks by itself).
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
#include "power.h"
#include "pkg_parse.h"
#include "screen_nap.h"
#include "haptic.h"
#include "rhythm.h"
#include "wave.h"

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
static atomic_uint     s_spot;               /* a tap on a scene's spot (mole_fluid_spot_tap) */

/* The human's tap did something: the touch feedback, at once. Taps closer than GESTURE_TAP_BOUNCE_MS are one (the
 * panel's bounce). An open mic drops it (haptic_buzz); do not disturb does not: it is the human's own touch. */
static void acted(void)
{
    static _Atomic uint32_t last;   /* ms: the touch poll runs on the drawer, or on the fluid's task while the screen is off */
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (now - atomic_exchange(&last, now) >= GESTURE_TAP_BOUNCE_MS) haptic_touch();
}

void sense_touch(int n, const uint16_t *xs, const uint16_t *ys)
{
    static bool down;
    if (n > 0 && !down && link_pair_active()) {   /* a pairing waits: the tap may be its answer, never a `tap` event */
        link_pair_tap((float)xs[0] + 0.5f - molty_display_width() * 0.5f, (float)ys[0] + 0.5f - molty_display_height() * 0.5f);
    } else if (n > 0 && !down) {
        int w = molty_display_width(), h = molty_display_height();
        /* a pending question takes the tap as its answer: no `tap` event for it */
        if (link_ask_tap((float)xs[0] + 0.5f - w * 0.5f, (float)ys[0] + 0.5f - h * 0.5f)) { down = true; return; }
        /* a scene's wheel, its centre, a beat's dot: the fluid task's (it turns, plays and stops, mutes), never a spot or a tap */
        const int zone = link_audio_listening(NULL) ? 0 : mole_fluid_zone(xs[0], ys[0]);
        if (zone) { if (zone != 1) acted(); down = true; return; }
        uint32_t x = w > 1 ? (uint32_t)xs[0] * 65535u / (uint32_t)(w - 1) : 0;
        uint32_t y = h > 1 ? (uint32_t)ys[0] * 65535u / (uint32_t)(h - 1) : 0;
        atomic_store(&s_tap_xy, (x > 65535u ? 65535u : x) << 16 | (y > 65535u ? 65535u : y));
        /* a scene's spot takes the tap (its reading shows), unless it stops a recording */
        uint32_t age;
        const unsigned spot = link_audio_listening(&age) ? 0 : mole_fluid_spot_tap(xs[0], ys[0]);
        if (spot) { acted(); atomic_store(&s_spot, spot); }
        else atomic_store(&s_tap, true);
    }
    down = n > 0;
}

static void emit(cJSON *j)
{
    if (j && link_client_connected()) link_send_json(j);
    cJSON_Delete(j);
}

/* What the human bound this input to. `event` is the input's own frame, built by the caller.
 * A button's hold starts here; its release is sense_task's. */
static void gesture_do(gesture_input_t in, cJSON *event)
{
    const gesture_action_t a = gesture_binds_get(mole_fluid_gestures(), in);
    if (a == GESTURE_DO_EVENT) { emit(event); return; }
    cJSON_Delete(event);
    const bool button = in == GESTURE_IN_BUTTON_ACCEPT || in == GESTURE_IN_BUTTON_REJECT;
    if (a == GESTURE_DO_HOLD && button) link_audio_hold(true, GESTURE_INPUT_NAMES[in]);
    else if (a == GESTURE_DO_TALK || a == GESTURE_DO_HOLD) link_audio_gesture_talk(GESTURE_INPUT_NAMES[in]);
    else if (a == GESTURE_DO_STOP) link_audio_stop_any();
}

/* A button press. A pending question keeps its meaning (acknowledge / dismiss, as an event). */
static void button_do(bool accept)
{
    cJSON *ev = link_evt_button_frame(accept);
    if (link_ask_pending()) emit(ev);
    else gesture_do(accept ? GESTURE_IN_BUTTON_ACCEPT : GESTURE_IN_BUTTON_REJECT, ev);
}

/* The screen switch's clicks (gesture_tapper_t: one click waits out the double window). */
static void screen_do(gesture_taps_t presses)
{
    if (presses == GESTURE_TAPS_SINGLE) mole_fluid_screen(!mole_fluid_screen_on());
    else if (presses == GESTURE_TAPS_DOUBLE) power_dnd_set(!power_dnd());
}

/* The screen switch's press: a click (a single one acts after the double window), held it powers off. */
static void switch_do(gesture_sw_t click, gesture_tapper_t *presser, uint32_t now)
{
    if (click == GESTURE_SW_CLICK) screen_do(gesture_tapper_feed(presser, now, true));
    else if (click == GESTURE_SW_HOLD) { presser->pending = false; power_off(); }
}

/* A tap on a spot: Molty shows its reading, or opens the layout the spot names; the agent hears which spot (whatever
 * tap is bound to), and `opened` (that layout) when Molty opened it itself. A tap beside the spots of a layout so
 * opened takes Molty back under it: the agent hears `closed`. */
static void spot_do(unsigned spot, uint32_t now, gesture_limit_t *limit)
{
    if (!gesture_limit_allow(limit, now)) return;
    const uint32_t xy = atomic_load(&s_tap_xy);
    char id[PKG_STR_MAX + 1], open[PKG_KEY_MAX + 1];
    mole_fluid_spot_id(spot, id, sizeof id, open, sizeof open);
    cJSON *ev = link_evt_tap_frame((xy >> 16) / 65535.0, (xy & 0xFFFFu) / 65535.0), *data = cJSON_GetObjectItemCaseSensitive(ev, "data");
    if (data && id[0]) cJSON_AddStringToObject(data, "spot", id);
    if (data && open[0]) cJSON_AddStringToObject(data, "opened", open);
    if (data && spot == MOLE_SPOT_BACK) cJSON_AddBoolToObject(data, "closed", true);
    mole_diag_event(false, spot == MOLE_SPOT_BACK ? "sense.tap back" : "sense.tap spot %s", id);
    emit(ev);
}

static void taps_do(gesture_taps_t taps, uint32_t now, gesture_limit_t *limit)
{
    if (taps == GESTURE_TAPS_NONE || !gesture_limit_allow(limit, now)) return;
    const uint32_t xy = atomic_load(&s_tap_xy);
    const double x = (xy >> 16) / 65535.0, y = (xy & 0xFFFFu) / 65535.0;
    if (taps == GESTURE_TAPS_DOUBLE) gesture_do(GESTURE_IN_DOUBLE_TAP, link_evt_double_tap_frame(x, y));
    else gesture_do(GESTURE_IN_TAP, link_evt_tap_frame(x, y));
}

static void sense_task(void *arg)
{
    (void)arg;
    gesture_t g;
    gesture_init(&g, NULL);
    nap_lift_t lift = { 0 };
    gesture_tapper_t tapper = { 0 }, presser[2] = { 0 };
    gesture_limit_t tap_l = { .min_ms = TAP_MIN_MS }, btn_l = { .min_ms = BUTTON_MIN_MS },
                    shake_l = { .min_ms = SHAKE_MIN_MS };
    const bool imu = molty_board_has(MOLTY_CAP_IMU);
    bool prev[2] = { molty_button_pressed(MOLTY_BTN_ACCEPT), molty_button_pressed(MOLTY_BTN_REJECT) };
    gesture_switch_t sw[2] = { { .down = prev[0] }, { .down = prev[1] } };
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
            if (mole_fluid_screen_on()) { if (lift.primed) nap_lift_reset(&lift); }
            else if (nap_lift_feed(&lift, s.ax, s.ay, s.az, s.ms)) {
                mole_fluid_lifted();                          /* (the fluid wakes only from a nap) */
                mole_diag_event(false, "sense.lift");
                nap_lift_reset(&lift);
            }
            gesture_shake_t sh;
            if (pairing || haptic_quiet()) gesture_reset(&g);   /* Molty's own motor shakes the IMU */
            else if (gesture_feed(&g, s.ax, s.ay, s.az, s.ms, &sh)) {
                mole_fluid_event(MOLE_FLUID_SHAKE);          /* reacts with or without an agent */
                mole_diag_event(false, "sense.shake peak=%dmg reversals=%d ms=%u",
                                (int)(sh.peak_g * 1000.f), sh.reversals, (unsigned)sh.ms);
                /* shake to talk (the console's setting, on out of the box): the link task starts
                 * listening and sends the event; otherwise the plain shake goes from here */
                if (gesture_limit_allow(&shake_l, now) && !link_audio_shake(sh.peak_g, sh.reversals, sh.ms))
                    emit(link_evt_shake_frame(sh.peak_g, sh.reversals, sh.ms, NULL, NULL, false));
            }
        }

        /* a skill's scene shows: the reject button closes it (the creature comes back), whatever it is bound to; with a
         * rhythm or a chord on it the accept button does what its centre does. Never a buzz: a button clicks by itself */
        const bool up = !pairing && !link_ask_pending() && mole_fluid_scene_up(), beat = up && (rhythm_active() || wave_active());
        for (int b = 0; b < 2; b++) {
            bool on = molty_button_pressed((molty_button_t)b);
            if (up && (b == MOLTY_BTN_REJECT || beat)) {
                if (on && !prev[b] && gesture_limit_allow(&btn_l, now)) { if (b == MOLTY_BTN_ACCEPT) { if (rhythm_active()) rhythm_local_play(); else wave_local_centre(); } else mole_fluid_scene_close(); }
                presser[b].pending = false; gesture_switch_reset(&sw[b]); sw[b].down = on;
                prev[b] = on;
                continue;
            }
            const bool screen = gesture_binds_get(mole_fluid_gestures(), b == MOLTY_BTN_ACCEPT ? GESTURE_IN_BUTTON_ACCEPT
                                                  : GESTURE_IN_BUTTON_REJECT) == GESTURE_DO_SCREEN && !link_ask_pending();
            const gesture_sw_t click = gesture_switch_feed(&sw[b], on, now);
            if (pairing || !screen) { presser[b].pending = false; gesture_switch_reset(&sw[b]); }
            else switch_do(click, &presser[b], now);
            if (on && !prev[b] && !pairing && !screen) {
                mole_fluid_event(MOLE_FLUID_BUTTON);
                if (gesture_limit_allow(&btn_l, now)) button_do(b == MOLTY_BTN_ACCEPT);
            }
            screen_do(gesture_tapper_poll(&presser[b], now));
            if (!on && prev[b]) link_audio_hold(false, NULL);   /* let go: a held talk sends */
            prev[b] = on;
        }

        const bool guarded = gesture_tap_guarded(&g, now);
        if (guarded || pairing) tapper.pending = false;   /* a tap waiting for its pair was a phantom */
        if (atomic_exchange(&s_tap, false) && !link_pair_active()) {
            uint32_t age = 0;
            const uint32_t gen = link_audio_job_gen();
            const bool listening = link_audio_listening(&age);
            switch (gesture_tap_route(listening, age, link_client_connected() || link_audio_held(), link_audio_say_waiting(), guarded)) {
            case GESTURE_TAP_STOP_LISTEN: mole_diag_event(false, "sense.tap stop listen"); link_audio_request(gen, LINK_AUDIO_REQ_STOP); break;
            case GESTURE_TAP_DROP_LISTEN: mole_diag_event(false, "sense.tap drop listen"); link_audio_request(gen, LINK_AUDIO_REQ_CANCEL); break;
            case GESTURE_TAP_CANCEL_SAY:  mole_diag_event(false, "sense.tap cancel say");  link_audio_request(gen, LINK_AUDIO_REQ_CANCEL); break;
            case GESTURE_TAP_GESTURE:
                taps_do(gesture_tapper_feed(&tapper, now, gesture_binds_get(mole_fluid_gestures(), GESTURE_IN_DOUBLE_TAP) != GESTURE_DO_NONE), now, &tap_l);
                break;
            case GESTURE_TAP_IGNORE: break;
            }
        }
        taps_do(gesture_tapper_poll(&tapper, now), now, &tap_l);
        const unsigned spot = atomic_exchange(&s_spot, 0);
        if (spot && !pairing) spot_do(spot, now, &tap_l);
        mole_scene_evt_t sv;                            /* the screen changed on the device itself: the relay hears what is up */
        if (mole_fluid_scene_event(&sv)) emit(link_evt_scene_frame(sv.pkg, sv.layout, sv.why, sv.spot));
        if (mole_fluid_take_closed()) acted();         /* a hold on the glass closed a scene: the touch buzz */
        {   /* a wheel's clicks: its own tick (haptic_plan.h HAPTIC_WHEEL_*, or the dial's), one at most every 60 ms (the
             * clicks in between felt as one at its end) */
            static uint32_t tick_at;
            int ms, strength;
            if (mole_fluid_ticks_waiting() && haptic_wheel_due(&tick_at, (uint32_t)(esp_timer_get_time() / 1000)) && mole_fluid_take_ticks(&ms, &strength))
                haptic_wheel(ms, strength);
        }
        mole_ring_evt_t re;                             /* a wheel settled */
        if (mole_fluid_ring_event(&re)) emit(link_evt_ring_frame(re.spot, re.value, re.index, re.label, re.id));
        rhythm_ev_t rv;                                 /* the beat player's own: a mute, a pause, a stop */
        while (rhythm_take_event(&rv)) emit(rhythm_event_frame(&rv));
        wave_ev_t wv;                                   /* the tone player's own: a mute, a play, a stop */
        while (wave_take_event(&wv)) emit(wave_event_frame(&wv));
        xTaskDelayUntil(&wake, period);
    }
}

esp_err_t sense_start(void)
{
    mole_fluid_gestures();   /* the bindings out of NVS here: the task's PSRAM stack never reads flash */
    /* PSRAM stack like link_audio: it sends over TLS (link_send_json), never touches flash */
    return xTaskCreatePinnedToCoreWithCaps(sense_task, "sense", 6144, NULL, 2, NULL, 0, MALLOC_CAP_SPIRAM) == pdPASS
        ? ESP_OK : ESP_ERR_NO_MEM;
}
