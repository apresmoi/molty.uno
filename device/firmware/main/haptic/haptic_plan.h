/*
 * SPDX-License-Identifier: MIT
 *
 * haptic_plan.h: what one buzz of the vibration motor is (haptic.h runs it).
 *
 * Plain C, no FreeRTOS / ESP-IDF: haptic.c builds a plan per POST /api/haptic or link buzz and plays it;
 * test/host/haptic_test.c checks the rules.
 *
 *   plan      up to HAPTIC_SEGS segments in ms, on, off, on, ... (one buzz: one segment);
 *             a zero-length segment is skipped. Strength 0..100 (clamped): the PWM duty.
 *   cap       one plan lasts at most HAPTIC_MAX_MS: the segment that crosses it is cut,
 *             those after it dropped.
 *   named     a few patterns by name (the link's buzz {pattern:"double"}, the device's own moments), each
 *             at most HAPTIC_NAMED_MAX_MS:
 *               tick    30            touch feedback, barely there (a press that acts)
 *               tap     60            a light touch
 *               double  60, 90, 60    look at me (a question with buttons, a pairing code)
 *               nudge   350           a longer push, for when a tap would be missed
 *   quiet     the motor shakes the IMU: shake detection ignores motion from the plan's start
 *             until HAPTIC_SETTLE_MS after its end (sense.c, haptic_quiet). A new plan
 *             replaces the running one and its window.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HAPTIC_SEGS       8
#define HAPTIC_MAX_MS     2000
#define HAPTIC_SETTLE_MS  250    /* the motor spun down, the case still: chosen, not measured */
#define HAPTIC_NAMED_MAX_MS 600  /* every named pattern is shorter */

typedef struct {
    uint16_t seg[HAPTIC_SEGS];   /* ms: even index on, odd off */
    int      n;
    uint8_t  strength;           /* 0..100                    */
} haptic_plan_t;

/** One buzz of ms (capped). False for ms < 1. */
bool haptic_plan_one(haptic_plan_t *p, int ms, int strength);
/** segs[0..n): on, off, on, ... False for n outside 1..HAPTIC_SEGS, a negative segment, or
 *  nothing on (every on segment 0). */
bool haptic_plan_pattern(haptic_plan_t *p, const int *segs, int n, int strength);
/** The named pattern `name` (above) at strength. False for an unknown name. */
bool haptic_plan_named(haptic_plan_t *p, const char *name, int strength);
/** The i-th named pattern's name, NULL past the last. */
const char *haptic_plan_name(int i);
/** The plan's length (ms), at most HAPTIC_MAX_MS. */
uint32_t haptic_plan_ms(const haptic_plan_t *p);
/** The PWM duty for a timer of `bits` resolution: strength 100 is 1 << bits (always on). */
uint32_t haptic_plan_duty(const haptic_plan_t *p, int bits);
/** Until when (us, the start's clock) shake detection stays deaf to a plan started at start_us. */
int64_t haptic_plan_quiet_until(const haptic_plan_t *p, int64_t start_us);

#ifdef __cplusplus
}
#endif
