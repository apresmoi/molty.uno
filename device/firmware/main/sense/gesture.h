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
    bool     shook;                          /* a shake was felt (last_shake is valid)  */
    uint32_t last_shake;                     /* its last strong sample, cooldown included */
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

/** The touch panel reports phantom taps while Molty is being shaken: true while strokes are
 *  under way (2+ peaks, the last within GESTURE_TAP_MOTION_MS) or within GESTURE_TAP_SHAKE_MS
 *  of a shake. A single knock (a finger's own tap) never guards. */
#define GESTURE_TAP_MOTION_MS 500u
#define GESTURE_TAP_SHAKE_MS  1000u
bool gesture_tap_guarded(const gesture_t *g, uint32_t now_ms);

/* What a tap means right now, before any binding: a recording and a say still waiting for
 * its first audio own the tap. Pure, so the order of these rules is host-tested. */
typedef enum {
    GESTURE_TAP_GESTURE,      /* nothing owns it: tap / double tap detection, then the binding */
    GESTURE_TAP_IGNORE,       /* a shake's phantom, or the first second of a recording         */
    GESTURE_TAP_STOP_LISTEN,  /* stop recording and send                                       */
    GESTURE_TAP_DROP_LISTEN,  /* stop recording; the link is down, nothing can be sent         */
    GESTURE_TAP_CANCEL_SAY,   /* give up waiting for speech that has not started               */
} gesture_tap_route_t;
#define GESTURE_TAP_LISTEN_GUARD_MS 1000u
gesture_tap_route_t gesture_tap_route(bool listening, uint32_t listen_age_ms, bool connected,
                                      bool say_waiting, bool guarded);

/* Tap vs double tap: with a double tap bound a single tap waits out the window, otherwise it
 * goes at once. Touches closer than GESTURE_TAP_BOUNCE_MS are one touch. */
#define GESTURE_DOUBLE_TAP_MS 350u
#define GESTURE_TAP_BOUNCE_MS 60u
typedef enum { GESTURE_TAPS_NONE, GESTURE_TAPS_SINGLE, GESTURE_TAPS_DOUBLE } gesture_taps_t;
typedef struct { bool pending; uint32_t t0; } gesture_tapper_t;
gesture_taps_t gesture_tapper_feed(gesture_tapper_t *t, uint32_t now_ms, bool double_bound);
gesture_taps_t gesture_tapper_poll(gesture_tapper_t *t, uint32_t now_ms);

/* The human's bindings: what each input does (cmd gestures, /api/creature/state, NVS). */
typedef enum { GESTURE_IN_TAP, GESTURE_IN_DOUBLE_TAP, GESTURE_IN_BUTTON_ACCEPT, GESTURE_IN_BUTTON_REJECT, GESTURE_IN_COUNT } gesture_input_t;
typedef enum { GESTURE_DO_NONE, GESTURE_DO_TALK, GESTURE_DO_EVENT, GESTURE_DO_STOP, GESTURE_DO_COUNT } gesture_action_t;
extern const char *const GESTURE_INPUT_NAMES[GESTURE_IN_COUNT];
extern const char *const GESTURE_ACTION_NAMES[GESTURE_DO_COUNT];
/** One nibble per input. Default: everything sends its event, a double tap talks. */
uint32_t gesture_binds_default(void);
/** False for a stored value that holds anything but valid actions. */
bool gesture_binds_valid(uint32_t packed);
gesture_action_t gesture_binds_get(uint32_t packed, gesture_input_t in);
uint32_t gesture_binds_set(uint32_t packed, gesture_input_t in, gesture_action_t a);
/** Name lookups: -1 when unknown. */
int gesture_input_lookup(const char *name);
int gesture_action_lookup(const char *name);

/* At most one event per min_ms (e.g. taps ≤ 4/s: 250 ms). */
typedef struct { uint32_t min_ms, last_ms; bool used; } gesture_limit_t;
/** True if an event at now_ms may go out (and records it). */
bool gesture_limit_allow(gesture_limit_t *l, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
