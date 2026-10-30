/*
 * SPDX-License-Identifier: MIT
 *
 * cute_act.h — the creature's one-shot ACTIONS: short gestures played over whatever mood
 * it is in (a nod, a glance, a yawn). Header-only: the names, durations and pose curves
 * are shared by the runtime (cute_rig.c, which adds them to the mood engine's pose), the
 * director (fluid_director.c, idle life) and the link (link_protocol.c, `act` and say
 * marks). The JS twin is device/creatures/js/actions.js: the same curves, sample for
 * sample (test/host/act_golden.txt, written by device/creatures/tools/act-golden.mjs).
 *
 * An action is a pose OFFSET (cute_act_pose_t) as a function of its own time t in
 * 0..dur: zero at both ends, so it starts and ends without a jump. It never changes the
 * mood, never makes a sound, and is a handful of sinf per frame.
 *
 * Pose conventions (js/mood.js): pos/rot/head/spine euler in the creature's frame (x =
 * pitch forward/down, y = yaw, z = roll), squash > 0 = taller, lid 0 open .. 1 closed,
 * look x right / y up in -1..1, mouth 0..1 open.
 */
#pragma once

#include <math.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* name, duration (s). Agents' `act {action}` names; the order is the enum's. */
#define CUTE_ACTIONS(X)            \
    X(WIGGLE, "wiggle", 1.4f)      \
    X(NOD, "nod", 1.2f)            \
    X(SHAKE_HEAD, "shake_head", 1.3f) \
    X(BOUNCE, "bounce", 1.2f)      \
    X(PEEK, "peek", 2.2f)          \
    X(GLANCE_LEFT, "glance_left", 1.4f) \
    X(GLANCE_RIGHT, "glance_right", 1.4f) \
    X(BLINK, "blink", 0.35f)       \
    X(WOBBLE, "wobble", 1.6f)      \
    X(STRETCH, "stretch", 1.8f)    \
    X(LOOK_UP, "look_up", 1.6f)    \
    X(YAWN, "yawn", 2.6f)

#define CUTE_ACT_ENUM(id, name, dur) CA_##id,
enum { CUTE_ACTIONS(CUTE_ACT_ENUM) CUTE_N_ACTS };
#undef CUTE_ACT_ENUM

static inline const char *cute_act_name(int a) {
#define CUTE_ACT_NAME(id, name, dur) name,
    static const char *const names[] = { CUTE_ACTIONS(CUTE_ACT_NAME) };
#undef CUTE_ACT_NAME
    return a >= 0 && a < CUTE_N_ACTS ? names[a] : "";
}

static inline float cute_act_dur(int a) {
#define CUTE_ACT_DUR(id, name, dur) dur,
    static const float durs[] = { CUTE_ACTIONS(CUTE_ACT_DUR) };
#undef CUTE_ACT_DUR
    return a >= 0 && a < CUTE_N_ACTS ? durs[a] : 0.f;
}

/** The action named `s` (exact, lower case), or -1. */
static inline int cute_act_lookup(const char *s) {
    if (!s) return -1;
    for (int a = 0; a < CUTE_N_ACTS; a++) if (!strcmp(s, cute_act_name(a))) return a;
    return -1;
}

typedef struct {
    float pos[3], rot[3], head[3], spine[3];
    float squash, lid, look[2], mouth;
} cute_act_pose_t;

/* smoothstep 0..1 of x */
static inline float cute_act_ss(float x) { x = x < 0.f ? 0.f : x > 1.f ? 1.f : x; return x * x * (3.f - 2.f * x); }
/* in over `a` s, out over the last `r` s of `d` */
static inline float cute_act_env(float t, float d, float a, float r) { return cute_act_ss(t / a) * cute_act_ss((d - t) / r); }
/* half a sine over a..b, 0 outside */
static inline float cute_act_bump(float t, float a, float b) {
    return (t < a || t > b) ? 0.f : sinf(3.14159265f * (t - a) / (b - a));
}

/**
 * The pose offset of action `a` at its time t (0..cute_act_dur(a)); all zero outside.
 * Keep in step with device/creatures/js/actions.js actionPose (act_golden.txt pins both).
 */
static inline void cute_act_pose(int a, float t, cute_act_pose_t *o) {
    const float TAU = 6.28318531f;
    memset(o, 0, sizeof(*o));
    const float d = cute_act_dur(a);
    if (a < 0 || a >= CUTE_N_ACTS || t < 0.f || t > d) return;
    switch (a) {
    case CA_WIGGLE: {            /* a quick side-to-side shimmy */
        float e = cute_act_env(t, d, 0.12f, 0.3f), s = sinf(TAU * 3.2f * t);
        o->rot[2] = 0.14f * e * s;
        o->squash = 0.05f * e * sinf(TAU * 6.4f * t);
        o->head[2] = 0.1f * e * sinf(TAU * 3.2f * t + 1.4f);
        break;
    }
    case CA_NOD: {               /* two nods: the head dips forward */
        float e = cute_act_env(t, d, 0.1f, 0.25f), n = fmaxf(0.f, sinf(TAU * 1.8f * t));
        o->head[0] = 0.26f * e * n;
        o->spine[0] = 0.04f * e * n;
        o->look[1] = -0.2f * e * n;
        break;
    }
    case CA_SHAKE_HEAD: {        /* no-no: the head turns side to side, the eyes lead */
        float e = cute_act_env(t, d, 0.1f, 0.3f), s = sinf(TAU * 2.2f * t);
        o->head[1] = 0.34f * e * s;
        o->look[0] = 0.4f * e * s;
        break;
    }
    case CA_BOUNCE: {            /* two hops, squash on each landing */
        float q = fmodf(t, 0.6f) / 0.6f, hop = sinf(3.14159265f * q), k = t < 0.6f ? 1.f : 0.65f;
        float e = cute_act_ss(t / 0.08f) * cute_act_ss((d - t) / 0.15f);
        o->pos[1] = 0.14f * k * hop * e;
        o->squash = k * e * (hop < 0.25f ? -0.12f * (1.f - hop / 0.25f) : 0.06f * hop);
        break;
    }
    case CA_PEEK: {              /* duck down, then peek up and look around */
        float e = cute_act_env(t, d, 0.3f, 0.5f), dip = cute_act_bump(t, 0.f, 0.9f);
        float up = cute_act_ss((t - 0.6f) / 0.3f) * e, look = cute_act_ss((t - 0.7f) / 0.2f) * e;
        float s = sinf(TAU * 0.9f * (t - 0.7f));
        o->pos[1] = -0.13f * dip + 0.03f * up;
        o->squash = -0.12f * dip;
        o->look[0] = 0.75f * s * look;
        o->head[1] = 0.18f * s * look;
        o->look[1] = 0.2f * up;
        break;
    }
    case CA_GLANCE_LEFT: case CA_GLANCE_RIGHT: {   /* eyes snap aside, the head follows a little */
        float e = cute_act_env(t, d, 0.15f, 0.35f), side = a == CA_GLANCE_LEFT ? -1.f : 1.f;
        o->look[0] = 0.85f * side * e;
        o->head[1] = 0.22f * side * e;
        o->head[2] = -0.04f * side * e;
        break;
    }
    case CA_BLINK:               /* 80 ms close, 70 ms hold, 150 ms open (mood.js blink) */
        o->lid = t < 0.08f ? t / 0.08f : t < 0.15f ? 1.f : t < 0.3f ? 1.f - (t - 0.15f) / 0.15f : 0.f;
        break;
    case CA_WOBBLE: {            /* a jelly wobble that dies down */
        float k = expf(-2.2f * t) * cute_act_ss(t / 0.08f) * cute_act_ss((d - t) / 0.3f);
        o->rot[2] = 0.12f * k * sinf(TAU * 2.2f * t);
        o->squash = 0.05f * k * sinf(TAU * 2.2f * t + 1.2f);
        break;
    }
    case CA_STRETCH: {           /* tall, head back, eyes relaxed */
        float e = cute_act_env(t, d, 0.6f, 0.6f);
        o->squash = 0.14f * e;
        o->head[0] = -0.18f * e;
        o->spine[0] = -0.08f * e;
        o->lid = 0.35f * e;
        break;
    }
    case CA_LOOK_UP: {
        float e = cute_act_env(t, d, 0.25f, 0.4f);
        o->head[0] = -0.32f * e;
        o->look[1] = 0.8f * e;
        o->spine[0] = -0.05f * e;
        break;
    }
    case CA_YAWN: {              /* head back, eyes half shut, mouth wide */
        float e = cute_act_env(t, d, 0.5f, 0.7f), m = cute_act_env(t, 2.2f, 0.5f, 0.5f);
        o->head[0] = -0.26f * e;
        o->spine[0] = -0.06f * e;
        o->squash = 0.08f * e;
        o->lid = 0.65f * e;
        o->mouth = t < 2.2f ? 0.85f * m : 0.f;
        break;
    }
    default: break;
    }
}

#ifdef __cplusplus
}
#endif
