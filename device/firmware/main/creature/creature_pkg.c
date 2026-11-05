/*
 * SPDX-License-Identifier: MIT
 *
 * creature_pkg.c: a creature package alive in the fluid (creature_pkg.h). The frame is the
 * lab renderer's for a package (renderer.mjs molty fill): pose, rig, raster and style,
 * the ambient over the body's coverage, the stamp, the fill.
 */
#include "creature_pkg.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define K_CALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM)
#define K_FREE(p)   heap_caps_free(p)
#else
#define K_CALLOC(n) calloc(1, (n))
#define K_FREE(p)   free(p)
#endif

#define MARKS_MAX 256

struct cpk {
    cp_t *cp;
    cb_t *body;
    mood_engine_t eng;
    bool work;
    cp_mark_t marks[MARKS_MAX];
};

cpk_t *cpk_create(const char *json, size_t len, uint32_t seed, const char **err) {
    cpk_t *k = K_CALLOC(sizeof(cpk_t));
    if (!k) { *err = "oom"; return NULL; }
    k->cp = cp_create(json, len, err);
    if (k->cp && !(k->body = cb_create(k->cp))) *err = "oom";
    if (!k->body) { cpk_destroy(k); return NULL; }
    me_init(&k->eng, seed);
    float dur[ME_OWN_MAX]; const int n = cp_own_gestures(k->cp) < ME_OWN_MAX ? cp_own_gestures(k->cp) : ME_OWN_MAX;
    for (int i = 0; i < n; i++) dur[i] = cp_gesture_dur(k->cp, CUTE_N_ACTS + i);
    me_own(&k->eng, dur, n);
    return k;
}

void cpk_destroy(cpk_t *k) {
    if (!k) return;
    cb_destroy(k->body);
    cp_destroy(k->cp);
    K_FREE(k);
}

mood_engine_t *cpk_engine(cpk_t *k) { return &k->eng; }
void cpk_set_work(cpk_t *k, bool on) { if (k) k->work = on; }
const cp_t *cpk_player(const cpk_t *k) { return k->cp; }
const cb_t *cpk_body(const cpk_t *k) { return k->body; }
int cpk_tris(const cpk_t *k) { return k ? cb_tris(k->body) : 0; }

static void impulse(void *ctx, float x, float y, float s, float r) { fluid_sim_impulse(ctx, x, y, s, r); }
static void push(void *ctx, float x, float y, float vx, float vy, float r) { fluid_sim_push(ctx, x, y, vx, vy, r); }

void cpk_frame(cpk_t *k, fluid_sim_t *s, float dt, float swell) {
    dt = fminf(dt, 1.f / 30.f);                         /* CreatureTarget.update clamps both */
    FLUID_PROF_BEGIN(c0);
    me_pose_t e;
    me_update(&k->eng, dt, &e);
    cp_pose_t p = { .squash = e.squash, .lid = e.lid, .mouth = e.mouth, .eye = e.eye, .mood = e.mood, .work = k->work };
    memcpy(p.pos, e.pos, sizeof(p.pos)); memcpy(p.rot, e.rot, sizeof(p.rot)); memcpy(p.head, e.head, sizeof(p.head));
    memcpy(p.spine, e.spine, sizeof(p.spine)); memcpy(p.look, e.look, sizeof(p.look));
    const mood_engine_t *g = &k->eng;
    const cp_action_t act = { g->act, g->act_t, g->act_n }, prev = { g->pact, g->pact_t, g->pact_n };
    static const cp_walk_t still;                       /* the device does not walk it anywhere */
    cp_pose(k->cp, &p, dt, g->act >= 0 ? &act : NULL, g->pact >= 0 ? &prev : NULL, g->pact_w, &still);
    FLUID_PROF_END(FP_C_UPDATE, c0);
    cb_raster(k->body, k->cp, s->W, s->H, g->cam_zoom, g->cam_focus);
    const cp_env_t env = { dt, cb_cov(k->body), s->W, s->H, CB_WINDOW, s, impulse, push };
    cb_stamp(k->body, k->marks, cp_ambient(k->cp, &env, k->marks, MARKS_MAX));
    cb_fill(k->body, s, swell);
}
