/*
 * SPDX-License-Identifier: MIT
 *
 * screen_nap.h: the screen goes dark by itself a while after the creature fell asleep, so a Molty left alone saves
 * its battery (and its AMOLED). Plain C, no FreeRTOS / ESP-IDF: the fluid task (mole_fluid.c) owns one nap_t and
 * sense.c one nap_lift_t; test/host/nap_test.c drives both with a fake clock.
 *
 *   timer    counts while the creature sleeps on screen and nothing else is up (`busy`: a scene, a question, a
 *            listen, a say, the progress ring, an update's or the pairing's words, the boot word, the bench);
 *            anything else stops it, and it counts from 0 the next time. Its length is the human's setting for the
 *            power Molty is on: battery_s on battery, usb_s on USB (or a board that cannot tell); 0 = never.
 *            Power not known yet (before the first reading; the web module): never.
 *   nap      at its end the screen goes off as the button's off does (mole_fluid_screen): a nap.
 *   wake     a touch, a button, a shake and what the human must see (words, a question, a listen) wake the screen
 *            from any off, as they always did. A nap also ends when Molty is picked up (nap_lift_feed) and for
 *            anything the agent shows (a mood, a gesture, a scene, progress); an off by hand waits for the human.
 *   by hand  an off by the button or /api/power is never a nap (nap_hand_off); any on starts the timer again
 *            (nap_on).
 *
 *   pick-up  the IMU's gravity, low-passed (NAP_LIFT_ALPHA a sample, ~0.2 s at 50 Hz), against the one it had
 *            NAP_LIFT_SETTLE_MS after the screen went off: tilted past NAP_LIFT_DEG for NAP_LIFT_HOLD_MS is a
 *            pick-up. A knock, a desk bump or a truck going by shakes the board but does not turn it: the filter
 *            leaves a few degrees of it, and a bump's tilt is gone before the hold.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NAP_BATTERY_S_DEFAULT 60     /* on battery: 1 min after it fell asleep */
#define NAP_USB_S_DEFAULT     600    /* on USB: 10 min                         */
#define NAP_MIN_S             10     /* a time the setting takes: 0 (never) or NAP_MIN_S..NAP_MAX_S */
#define NAP_MAX_S             3600

enum { NAP_POWER_UNKNOWN, NAP_POWER_BATTERY, NAP_POWER_USB };

/* Why the screen would come back on (nap_wakes). */
typedef enum { NAP_WAKE_TOUCH, NAP_WAKE_BUTTON, NAP_WAKE_SHAKE, NAP_WAKE_SHOW, NAP_WAKE_LIFT, NAP_WAKE_AGENT } nap_wake_t;

typedef struct {
    bool     counting;               /* the timer runs, from since_ms */
    uint32_t since_ms;
    bool     napping;                /* the screen is off because of the timer, not by hand */
} nap_t;

/** A setting's value the device takes: 0 (never) or NAP_MIN_S..NAP_MAX_S. */
bool nap_valid_s(long s);
/** Once a frame with the screen on: true = turn it off now (a nap starts). power: NAP_POWER_*. */
bool nap_tick(nap_t *n, uint32_t now_ms, bool asleep, bool busy, int power, uint32_t battery_s, uint32_t usb_s);
/** The screen came on (for whatever reason): the nap is over, the timer starts again. */
void nap_on(nap_t *n);
/** The human turned the screen off by hand (now or while it napped): from here it is not a nap. */
void nap_hand_off(nap_t *n);
/** With the screen off: whether this wakes it. */
bool nap_wakes(const nap_t *n, nap_wake_t why);

#define NAP_LIFT_ALPHA     0.1f
#define NAP_LIFT_SETTLE_MS 1000u
#define NAP_LIFT_DEG       30.f
#define NAP_LIFT_HOLD_MS   400u

typedef struct {
    bool     primed, ref_ok, tilted;
    float    g[3], ref[3];
    uint32_t t0, tilt_ms;
} nap_lift_t;

/** Forget the rest it knew (the screen came on). */
void nap_lift_reset(nap_lift_t *l);
/** One IMU sample with the screen off (g, its ms): true once it was picked up. */
bool nap_lift_feed(nap_lift_t *l, float ax, float ay, float az, uint32_t ms);

#ifdef __cplusplus
}
#endif
