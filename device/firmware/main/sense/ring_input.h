/*
 * SPDX-License-Identifier: MIT
 *
 * ring_input.h: a finger turning a wheel round the edge of the glass, in clicks (a scene's `dial`, scene_player.h).
 * Plain C, no FreeRTOS: mole_fluid.c feeds it the finger on the fluid task, test/host/beat_test.c checks it.
 *
 *   grab     a touch that comes down at r0 of the radius or further out is the wheel's: the whole touch is, until it
 *            lifts (it never stirs the water, never taps a spot or sends a tap).
 *   clicks   the finger's angle round the centre, unwrapped across 180 degrees: every `step` degrees turned is one click,
 *            clockwise positive. What is left over carries on, so a slow turn loses nothing and turning back undoes.
 *            Near the centre (under RING_MIN_R) the angle means nothing: those moves are skipped.
 *   fast     the turn's speed, smoothed: over RING_FAST_DPS a click counts `fast` instead of `by` (the caller's).
 */
#pragma once
#include <stdbool.h>

#define RING_MIN_R    0.3f              /* closer to the centre than this, a move turns nothing     */
#define RING_FAST_DPS 200.f             /* degrees a second: turning fast                            */

typedef struct {
    bool down;
    float a;                            /* the finger's angle last time, degrees                     */
    float acc;                          /* degrees turned and not yet a click (clockwise positive)  */
    float turned;                       /* degrees turned since it came down (clockwise positive)   */
    float dps;                          /* the turn's speed, smoothed                                */
    float t;                            /* s, the last move                                          */
} ring_drag_t;

/** On the wheel: (x, y) (glass units) at r0 of the radius or further out. */
bool ring_grabs(float r0, float x, float y);
void ring_down(ring_drag_t *d, float x, float y, float t);
/** The finger at (x, y) at t s: the clicks since the last move (clockwise positive); *fast: turning fast. */
int  ring_move(ring_drag_t *d, float step, float x, float y, float t, bool *fast);
void ring_up(ring_drag_t *d);
