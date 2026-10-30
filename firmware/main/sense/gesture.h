/*
 * SPDX-License-Identifier: MIT
 *
 * gesture.h — shake detection on accelerometer samples, plus a small event rate limit.
 *
 * Plain C, no FreeRTOS / ESP-IDF: sense.c feeds it the board's IMU samples (device frame,
 * g, ~50 Hz); test/host/gesture_test.c feeds it synthetic traces.
 *
 *   gravity   g ← g + α·(a − g) per sample (α 0.1 at 50 Hz ≈ a 0.2 s time constant);
 *             dyn = a − g. A slow tilt moves g along with a, so dyn stays small.
 *   peak      |dyn| > peak_g starts an excursion on the axis where |dyn| is largest;
 *             it ends when |dyn| falls under rearm_g, or the sign on that axis flips
 *             (a vigorous shake never rests near zero between two strokes).
 *   shake     ≥ reversals sign flips between successive peaks on the same axis, each
 *             ≥ min_gap_ms after the last, all within window_ms. A single knock (one
 *             spike, or a few samples of ringing faster than a hand) is not a shake.
 *   cooldown  after a shake, peaks are ignored for cooldown_ms.
 *
 * Every constant is in gesture_cfg_t; GESTURE_CFG_DEFAULT holds the shipped values.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float    alpha;          /* gravity low-pass per sample                       0.1  */
    float    peak_g;         /* dynamic |a| that makes a peak (g)                 1.2  */
    float    rearm_g;        /* fall under this to end an excursion (g)           0.6  */
    int      reversals;      /* sign flips on the dominant axis for a shake         3  */
    uint32_t window_ms;      /* ...all within this                                800  */
    uint32_t min_gap_ms;     /* peaks closer than this are one stroke ringing      50  */
    uint32_t cooldown_ms;    /* after a shake                                    2000  */
} gesture_cfg_t;
extern const gesture_cfg_t GESTURE_CFG_DEFAULT;

#define GESTURE_PEAKS 8

typedef struct { uint32_t ms; float mag; int8_t axis, sign; } gesture_peak_t;

typedef struct {
    gesture_cfg_t cfg;
    bool     primed;                         /* gravity estimate seeded           */
    float    g[3];                           /* gravity estimate (g)              */
    bool     in_peak;                        /* inside an excursion               */
    int      npeaks;
    gesture_peak_t peaks[GESTURE_PEAKS];     /* oldest first                      */
    bool     cooling;
    uint32_t cool_until;
} gesture_t;

typedef struct {
    float    peak_g;                         /* largest dynamic |a| of the shake (g) */
    int      reversals;                      /* sign flips counted                    */
    uint32_t ms;                             /* first peak → detection               */
} gesture_shake_t;

/** cfg NULL = GESTURE_CFG_DEFAULT. */
void gesture_init(gesture_t *g, const gesture_cfg_t *cfg);
/** Forget peaks and the gravity estimate (e.g. while pairing owns the device). */
void gesture_reset(gesture_t *g);
/** One sample: acceleration in g, its timestamp in ms. True (and *out) on a shake. */
bool gesture_feed(gesture_t *g, float ax, float ay, float az, uint32_t ms, gesture_shake_t *out);

/* At most one event per min_ms (e.g. taps ≤ 4/s: 250 ms). */
typedef struct { uint32_t min_ms, last_ms; bool used; } gesture_limit_t;
/** True if an event at now_ms may go out (and records it). */
bool gesture_limit_allow(gesture_limit_t *l, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
