/*
 * SPDX-License-Identifier: MIT
 *
 * mood_engine.c: the shared pose every creature speaks (mood_engine.h). Port of
 * device/creatures/js/mood.js (MoodEngine), moved here from cute_rig.c so the .molty
 * runtime and the creature package player both read it.
 */
#include "mood_engine.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#define PI_F  3.14159265f
#define TAU_F 6.28318531f

static inline float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static float rnd(mood_engine_t *e) {
    uint32_t x = e->rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; e->rng = x;
    return (float)(x >> 8) * (1.f / 16777216.f);
}
static float rand_ab(mood_engine_t *e, float a, float b) { return a + rnd(e) * (b - a); }

static float jit(mood_engine_t *e, float p) { return 1.f + rand_ab(e, -p, p); }
static jit_t new_jit(mood_engine_t *e) {
    jit_t j = { jit(e, 0.15f), jit(e, 0.15f), rnd(e) < 0.5f ? -1 : 1, rand_ab(e, 3, 8), 0, 1 };
    return j;
}
static void zero_pose(me_pose_t *p) { memset(p, 0, sizeof(*p)); p->eye = ME_EYE_NORMAL; }

static float speech_update(mood_engine_t *e, speech_t *s, float dt) {
    s->t += dt;
    if (s->t > s->phrase_end && s->pause_end == 0.f) s->pause_end = s->t + rand_ab(e, 0.25f, 0.7f);
    if (s->pause_end != 0.f && s->t > s->pause_end) { s->pause_end = 0; s->phrase_end = s->t + rand_ab(e, 1.2f, 3); s->rate = rand_ab(e, 4, 5); }
    bool speaking = s->pause_end == 0.f;
    float prev = s->phase;
    s->phase += dt * s->rate;
    if (floorf(s->phase) != floorf(prev)) s->syll_amp = rand_ab(e, 0.45f, 1);
    float sn = sinf(TAU_F * s->phase); if (sn < 0) sn = 0;
    float target = speaking ? powf(sn, 0.7f) * s->syll_amp : 0.f;
    float k = target > s->value ? 1.f - expf(-dt * 40) : 1.f - expf(-dt * 14);
    s->value += (target - s->value) * k;
    return s->value;
}

static void gen(mood_engine_t *e, int mood, me_pose_t *p, float t, jit_t *j, float env) {
    switch (mood) {
    case CM_IDLE:
        p->head[2] = 0.05f * j->a;
        p->head[1] = 0.08f * sinf(TAU_F * t / (9 * j->f));
        p->rot[2] = 0.015f * sinf(TAU_F * t / (6 * j->f));
        break;
    case CM_LISTENING:
        p->spine[0] = 0.14f * j->a; p->rot[0] = 0.05f; p->pos[2] = 0.05f;
        p->head[2] = 0.22f * j->a * j->side;
        p->head[0] = -0.06f + 0.05f * fmaxf(0.f, sinf(TAU_F * t / (2.6f * j->f)));
        p->eye = ME_EYE_WIDE; p->look[1] = 0.1f;
        break;
    case CM_THINKING:
        p->rot[2] = 0.07f * sinf(TAU_F * t / (3.2f * j->f));
        p->pos[1] = 0.015f * sinf(TAU_F * t / (2.4f * j->f));
        p->head[0] = -0.28f * j->a; p->head[1] = 0.32f * j->side; p->head[2] = 0.1f * j->side;
        p->look[0] = 0.7f * j->side; p->look[1] = 0.7f;
        break;
    case CM_SPEAKING:
        p->squash = 0.06f * env; p->pos[1] = 0.018f * env;
        p->head[0] = -0.05f * env + 0.03f * sinf(TAU_F * t / 1.7f);
        p->head[2] = 0.06f * sinf(TAU_F * t / (3.3f * j->f));
        p->mouth = env;
        if (t > j->next_glance) { j->glance_until = t + rand_ab(e, 0.5f, 0.9f); j->next_glance = t + rand_ab(e, 3, 8); j->glance_dir = rnd(e) < 0.5f ? -1 : 1; }
        if (t < j->glance_until) { p->look[0] = 0.8f * j->glance_dir; p->head[1] = 0.12f * j->glance_dir; }
        break;
    case CM_HAPPY: {
        float f = 2.1f * j->f, hop = fabsf(sinf(PI_F * f * t));
        p->pos[1] = 0.13f * j->a * hop;
        p->squash = hop < 0.25f ? -0.12f * (1 - hop / 0.25f) : 0.05f * hop;
        p->head[2] = 0.12f * sinf(TAU_F * f * 0.5f * t);
        p->rot[1] = 0.1f * sinf(TAU_F * f * 0.25f * t);
        p->eye = ME_EYE_HAPPY; p->mouth = 0.35f;
        break;
    }
    case CM_SLEEPY: {
        /* asleep, not down: eyes shut and a slight nod; a low head and half-open eyes read as sad */
        p->head[0] = 0.12f * j->a; p->spine[0] = 0.04f; p->rot[0] = 0.03f; p->pos[1] = -0.015f;
        float per = 7 * j->f, c = fmodf(t, per) / per;
        if (c > 0.75f) { float q = (c - 0.75f) / 0.25f; p->head[0] += 0.25f * sinf(PI_F * q) * (q < 0.8f ? 1.f : -0.5f); }
        p->lid = 1.f;
        p->eye = ME_EYE_SLEEPY;
        break;
    }
    case CM_SURPRISED:
        if (t < 0.12f) p->squash = -0.14f * sinf(PI_F * t / 0.12f);
        else if (t < 0.6f) { float q = (t - 0.12f) / 0.48f; p->pos[1] = 0.17f * sinf(PI_F * q); p->squash = 0.16f * (1 - q) - 0.04f; }
        else { float q = fminf(1.f, (t - 0.6f) / 0.6f); p->squash = -0.04f * (1 - q); }
        p->spine[0] = -0.1f; p->rot[0] = -0.05f; p->head[0] = -0.1f;
        p->eye = ME_EYE_WIDE; p->mouth = 0.55f;
        break;
    }
}

static void lerp_pose(me_pose_t *o, const me_pose_t *a, const me_pose_t *b, float w) {
    for (int k = 0; k < 3; k++) {
        o->pos[k] = a->pos[k] + (b->pos[k] - a->pos[k]) * w; o->rot[k] = a->rot[k] + (b->rot[k] - a->rot[k]) * w;
        o->head[k] = a->head[k] + (b->head[k] - a->head[k]) * w; o->spine[k] = a->spine[k] + (b->spine[k] - a->spine[k]) * w;
    }
    o->squash = a->squash + (b->squash - a->squash) * w; o->lid = a->lid + (b->lid - a->lid) * w; o->mouth = a->mouth + (b->mouth - a->mouth) * w;
    o->look[0] = a->look[0] + (b->look[0] - a->look[0]) * w; o->look[1] = a->look[1] + (b->look[1] - a->look[1]) * w;
    o->eye = w < 0.5f ? a->eye : b->eye;
}
static float spring_step(spring_t *s, float target, float dt) {
    float a = s->k * (target - s->x) - s->d * s->v; s->v += a * dt; s->x += s->v * dt; return s->x;
}

void me_init(mood_engine_t *e, uint32_t seed) {
    memset(e, 0, sizeof(*e));
    e->rng = seed ? seed : 0x9e3779b9u;
    e->mood = CM_IDLE; e->prev = -1; e->fade = 1; e->fade_dur = 0.7f;
    e->j = new_jit(e); e->pj = e->j;
    e->env.phrase_end = rand_ab(e, 1.2f, 3); e->env.rate = rand_ab(e, 4, 5); e->env.syll_amp = 1;
    e->eblink.next = rand_ab(e, 2, 6); e->eblink.phase = -1;
    e->saccade.next = rand_ab(e, 0.2f, 0.6f);
    for (int k = 0; k < 3; k++) { e->sp_h[k] = (spring_t){ 90, 9, 0, 0 }; e->sp_b[k] = (spring_t){ 45, 7, 0, 0 }; }
    e->sp_sq = (spring_t){ 160, 9, 0, 0 };
    e->sp_py = (spring_t){ 220, 14, 0, 0 };
    e->act = e->pact = -1;
}

void me_set_mood(mood_engine_t *e, int mood) {
    if (!e || mood < 0 || mood >= CUTE_N_MOODS || mood == e->mood) return;
    e->prev = e->mood; e->pj = e->j; e->prev_t = e->et;
    e->mood = mood; e->et = 0; e->fade = 0; e->fade_dur = rand_ab(e, 0.5f, 1.0f);
    e->j = new_jit(e);
    e->anticip = 0.0001f;
}
int me_mood(const mood_engine_t *e) { return e ? e->mood : 0; }

void me_act(mood_engine_t *e, int act) {
    if (!e || act < 0 || act >= CUTE_N_ACTS) return;
    if (e->act >= 0) { e->pact = e->act; e->pact_t = e->act_t; e->pact_w = 1.f; e->pact_n = e->act_n; }   /* fades out */
    e->act = act; e->act_t = 0.f; e->act_n = ++e->plays;
}
int me_acting(const mood_engine_t *e) { return e ? e->act : -1; }
float me_act_time(const mood_engine_t *e) { return e && e->act >= 0 ? e->act_t : 0.f; }

/* the action layer: offsets added after the springs (the curves are smooth already, and a
 * nod must not be swallowed by the head's 1.5 Hz spring); look before the saccade clamp,
 * lid after the blink (taken as max) */
static void act_add(me_pose_t *p, const cute_act_pose_t *a, float w) {
    for (int k = 0; k < 3; k++) {
        p->pos[k] += w * a->pos[k]; p->rot[k] += w * a->rot[k];
        p->head[k] += w * a->head[k]; p->spine[k] += w * a->spine[k];
    }
    p->squash += w * a->squash; p->mouth = fmaxf(p->mouth, w * a->mouth);
    p->look[0] += w * a->look[0]; p->look[1] += w * a->look[1];
}
static float act_layer(mood_engine_t *e, float dt, me_pose_t *p) {
    float lid = 0.f;
    cute_act_pose_t a;
    e->cam_zoom = e->cam_focus = 0.f;
    if (e->pact >= 0) {                               /* the replaced one, out over 0.25 s */
        e->pact_t += dt; e->pact_w -= dt / 0.25f;
        if (e->pact_w <= 0.f || e->pact_t > cute_act_dur(e->pact)) e->pact = -1;
        else { cute_act_pose(e->pact, e->pact_t, &a); act_add(p, &a, e->pact_w); lid = e->pact_w * a.lid; e->cam_zoom = e->pact_w * a.zoom; e->cam_focus = e->pact_w * a.focus; }
    }
    if (e->act >= 0) {
        e->act_t += dt;
        if (e->act_t > cute_act_dur(e->act)) e->act = -1;
        else { cute_act_pose(e->act, e->act_t, &a); act_add(p, &a, 1.f); lid = fmaxf(lid, a.lid); e->cam_zoom += a.zoom; e->cam_focus += a.focus; }
    }
    return lid;
}
float me_env(const mood_engine_t *e) { return e ? e->env_value : 0.f; }

void me_update(mood_engine_t *e, float dt, me_pose_t *p) {
    dt = fminf(dt, 0.05f);
    e->time += dt; e->et += dt; e->prev_t += dt;
    e->env_value = (e->mood == CM_SPEAKING || e->prev == CM_SPEAKING) ? speech_update(e, &e->env, dt) : 0.f;
    me_pose_t cur; zero_pose(&cur); gen(e, e->mood, &cur, e->et, &e->j, e->env_value);
    *p = cur;
    if (e->fade < 1.f && e->prev >= 0) {
        e->fade = fminf(1.f, e->fade + dt / e->fade_dur);
        me_pose_t old; zero_pose(&old); gen(e, e->prev, &old, e->prev_t, &e->pj, e->env_value);
        float w = e->fade * e->fade * (3 - 2 * e->fade);
        lerp_pose(p, &old, &cur, w);
    }
    bool sleepy = e->mood == CM_SLEEPY;
    float period = sleepy ? 5 * e->j.f : 3.5f * e->j.f, amp = sleepy ? 0.04f : 0.025f;
    p->squash += amp * sinf(TAU_F * e->time / period);
    if (e->anticip > 0) {
        e->anticip += dt; float q = e->anticip / 0.18f;
        if (q >= 1) e->anticip = 0; else { p->squash -= 0.07f * sinf(PI_F * q); p->head[0] += 0.08f * sinf(PI_F * q); }
    }
    for (int k = 0; k < 3; k++) p->head[k] = spring_step(&e->sp_h[k], p->head[k], dt);
    for (int k = 0; k < 3; k++) p->rot[k] = spring_step(&e->sp_b[k], p->rot[k], dt);
    p->pos[1] = spring_step(&e->sp_py, p->pos[1], dt);
    p->squash = spring_step(&e->sp_sq, p->squash, dt);
    const float act_lid = act_layer(e, dt, p);
    e->saccade.next -= dt;
    if (e->saccade.next <= 0) { e->saccade.next = rand_ab(e, 0.2f, 0.6f); e->saccade.x = rand_ab(e, -0.08f, 0.08f); e->saccade.y = rand_ab(e, -0.06f, 0.06f); }
    p->look[0] = clampf(p->look[0] + e->saccade.x, -1, 1);
    p->look[1] = clampf(p->look[1] + e->saccade.y, -1, 1);
    /* blinks: 2–6 s, 80 ms close / 50–100 hold / 150 open; 1 in 8 doubles */
    eblink_t *b = &e->eblink;
    b->next -= dt;
    if (b->phase < 0 && b->next <= 0) { b->phase = 0; b->hold = rand_ab(e, 0.05f, 0.1f); b->dbl = !b->second && rnd(e) < 1.f / 8; b->second = false; }
    if (b->phase >= 0) {
        b->phase += dt; float c = 0.08f, h = c + b->hold, o = h + 0.15f;
        b->value = b->phase < c ? b->phase / c : b->phase < h ? 1.f : b->phase < o ? 1.f - (b->phase - h) / 0.15f : 0.f;
        if (b->phase >= o) {
            b->phase = -1; b->value = 0;
            bool engaged = e->mood == CM_LISTENING || e->mood == CM_SPEAKING;
            b->next = b->dbl ? rand_ab(e, 0.2f, 0.4f) : engaged ? rand_ab(e, 2, 4) : rand_ab(e, 4, 7);
            if (b->dbl) { b->dbl = false; b->second = true; }
        }
    }
    p->lid = clampf(fmaxf(p->lid, 0) + b->value * (1 - p->lid), 0, 1);
    p->lid = fmaxf(p->lid, act_lid);
    p->mood = e->mood;
}

