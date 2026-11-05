/*
 * SPDX-License-Identifier: MIT
 *
 * mood_engine.h: the shared pose (js/mood.js MoodEngine): a mood's body language with its
 * jitter, blinks, saccades, springs (eyes lead, head follows, body last), the fake speech
 * envelope, and the one-shot actions of cute_act.h over it (the one playing, and the one it
 * replaced fading out over 0.25 s). Every creature reads it: the .molty runtime (cute_rig.c)
 * and the creature package player (creature_player.h). Plain C, host-testable.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cute_act.h"
#include "cute_spec.h"   /* CM_* moods */

#ifdef __cplusplus
extern "C" {
#endif

enum { ME_EYE_NORMAL, ME_EYE_WIDE, ME_EYE_HAPPY, ME_EYE_SLEEPY };

typedef struct {
    float pos[3], rot[3], head[3], spine[3];
    float squash, lid, look[2], mouth;
    int   eye;                 /* ME_EYE_*                                                 */
    int   mood;                /* CM_*                                                     */
} me_pose_t;

#define ME_OWN_MAX 32
typedef struct { float k, d, x, v; } spring_t;
typedef struct { float a, f; int side; float next_glance, glance_until; int glance_dir; } jit_t;
typedef struct { float t, phrase_end, pause_end, rate, phase, value, syll_amp; } speech_t;
typedef struct { float next, phase, hold, value; bool dbl, second; } eblink_t;

typedef struct {
    uint32_t rng;
    int mood, prev; float et, prev_t, fade, fade_dur, anticip, time;
    jit_t j, pj; speech_t env; float env_value;
    eblink_t eblink;
    struct { float next, x, y; } saccade;
    spring_t sp_h[3], sp_b[3], sp_py, sp_sq;
    /* actions: the one playing and the one it replaced; n numbers each play from 1 */
    int act, pact; float act_t, pact_t, pact_w; unsigned act_n, pact_n, plays;
    float cam_zoom, cam_focus;   /* the actions' camera this frame (cute_act_pose_t)       */
    int nown; float own_dur[ME_OWN_MAX];   /* the creature's own gestures: ids CUTE_N_ACTS + i */
} mood_engine_t;

void  me_init(mood_engine_t *e, uint32_t seed);
/* fades from the mood it was in (the same mood is ignored) */
void  me_set_mood(mood_engine_t *e, int mood);
int   me_mood(const mood_engine_t *e);
/* the creature's own gestures (a package's, creature_player.h cp_gesture): their lengths; ids CUTE_N_ACTS + i
 * play them with no shared pose (the package moves the body) */
void  me_own(mood_engine_t *e, const float *dur, int n);
/* a gesture's length: cute_act.h's, or the creature's own; 0 for none */
float me_act_dur(const mood_engine_t *e, int act);
/* plays a gesture (cute_act.h, or the creature's own) over the mood; a newer one replaces it */
void  me_act(mood_engine_t *e, int act);
int   me_acting(const mood_engine_t *e);
float me_act_time(const mood_engine_t *e);
/* the speech envelope (0..1) while speaking */
float me_env(const mood_engine_t *e);
/* advance by dt (at most 0.05 s) and write this frame's pose */
void  me_update(mood_engine_t *e, float dt, me_pose_t *p);

#ifdef __cplusplus
}
#endif
