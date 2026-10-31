/*
 * SPDX-License-Identifier: MIT
 *
 * shake_settle.h — when has a shake settled? (shake to talk: the handling noise window)
 *
 * Plain C, no FreeRTOS / ESP-IDF: link_audio.c holds one, the sense task feeds it the IMU
 * samples (~50 Hz); test/host/settle_test.c feeds it synthetic shakes.
 *
 * A listen begun by a shake starts while the device is still being shaken: the first few
 * hundred ms of its audio are handling noise (thumps, rattle), which the STT took for speech
 * (a hard 8 g shake once transcribed as Hindi script). The listen therefore keeps only the
 * audio after the shake settled:
 *
 *   still     | |a| - 1 g | < tol_g  (the device rests or is held: only gravity)
 *   settled   still continuously for still_ms, measured from the listen's start (t0):
 *             the settle point is where that stillness is CONFIRMED (the run's first still
 *             sample + still_ms) — cutting at the run's start let the shake's ringing tail
 *             through (27% of the noise energy of a 3 g shake, measured in tools/sim)
 *   cap       nothing settled by cap_ms after t0: settled at cap_ms (capped) — a human who
 *             keeps waving Molty around still gets heard
 *
 * settle_ms() is the offset from t0 (ms) of the first audio the listen keeps.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    float    tol_g;      /* | |a| - 1 | under this is still                0.15 */
    uint32_t still_ms;   /* a still run this long settles it                150 */
    uint32_t cap_ms;     /* settled at the latest this long after t0        600 */
} settle_cfg_t;
#define SETTLE_CFG_DEFAULT { .tol_g = 0.15f, .still_ms = 150, .cap_ms = 600 }

typedef struct {
    settle_cfg_t cfg;
    uint32_t t0;         /* the listen's start (IMU clock, ms)               */
    bool     run;        /* inside a still run                               */
    uint32_t run_from;   /* its first still sample (ms)                      */
    bool     done, capped;
    uint32_t at;         /* the settle offset from t0 (ms), once done        */
} settle_t;

/** cfg NULL = SETTLE_CFG_DEFAULT. */
void settle_init(settle_t *s, const settle_cfg_t *cfg, uint32_t t0_ms);
/** One IMU sample (g, its timestamp ms). True once settled (sticky). */
bool settle_feed(settle_t *s, float ax, float ay, float az, uint32_t ms);
/** The cap, by the clock (no sample needed): true once settled. */
bool settle_tick(settle_t *s, uint32_t now_ms);
