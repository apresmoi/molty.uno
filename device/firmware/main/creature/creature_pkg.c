/*
 * SPDX-License-Identifier: MIT
 *
 * creature_pkg.c: a creature package alive in the fluid (creature_pkg.h). The frame is the
 * lab renderer's for a package (renderer.mjs molty fill): pose, rig, raster and style,
 * the ambient over the body's coverage, the stamp, the fill.
 */
#include "creature_pkg.h"
#include "creature_clip.h"
#include "creature_dots.h"
#include "cJSON.h"
#include "creature_ring.h"

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
    cb_t *body;                 /* the channel creatures' body (NULL for a rigged one)                 */
    cc_t *clip;                 /* a rigged creature (docs/CREATURES.md section 12): its clips ...        */
    cd_t *dots;                 /* ... drawn straight on the braille dots (made at the first frame)      */
    uint8_t *cells;             /* ... and the rings over them (cpk_paint): the cells the drawer shows   */
    cr_t *ring; float dt;      /* the rings as dust (creature_ring.h); the last frame's dt                */
    unsigned act_n; uint32_t frame;
    mood_engine_t eng;
    bool work;
    /* its life: the inputs' slots in the player and what they read, each layer's signals (-1: weight 1) */
    int in_free, in_idle, in_life; float idle_s, life;
    int16_t lay_by[CC_LAYERS_MAX], lay_w[CC_LAYERS_MAX];
    cp_mark_t marks[MARKS_MAX];
};

/* the life layers' signals by name (creature.life.layers[i].by and .w), as the player's slots */
static bool bind_layers(cpk_t *k, const cJSON *cr) {
    const cJSON *L = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(cr, "life"), "layers");
    for (int i = 0; i < cc_layers(k->clip); i++) {
        const cJSON *l = cJSON_GetArrayItem(L, i), *by = cJSON_GetObjectItemCaseSensitive(l, "by"), *w = cJSON_GetObjectItemCaseSensitive(l, "w");
        k->lay_by[i] = (int16_t)cp_slot(k->cp, by->valuestring);
        k->lay_w[i] = (int16_t)(cJSON_IsString(w) ? cp_slot(k->cp, w->valuestring) : -1);
        if (k->lay_by[i] < 0 || (w && k->lay_w[i] < 0)) return false;
    }
    return true;
}

cpk_t *cpk_create(const char *json, size_t len, uint32_t seed, const char **err) {
    cpk_t *k = K_CALLOC(sizeof(cpk_t));
    if (!k) { *err = "oom"; return NULL; }
    k->cp = cp_create(json, len, err);
    if (k->cp && cp_rigged(k->cp)) {            /* a rigged creature: its clips and its dots, no channel body */
        cJSON *j = cJSON_ParseWithLength(json, len);
        k->clip = j ? cc_create(cJSON_GetObjectItemCaseSensitive(j, "creature"), err) : NULL;
        if (!j) *err = "package: json";
        if (k->clip && !bind_layers(k, cJSON_GetObjectItemCaseSensitive(j, "creature"))) { cc_destroy(k->clip); k->clip = NULL; *err = "life: a layer's signal"; }
        cJSON_Delete(j);
        if (!k->clip) { cpk_destroy(k); return NULL; }
        cp_seed(k->cp, seed);
        k->in_free = cp_slot(k->cp, "free"); k->in_idle = cp_slot(k->cp, "idle_time"); k->in_life = cp_slot(k->cp, "life");
        k->life = 1.f;
    } else if (k->cp && !(k->body = cb_create(k->cp))) *err = "oom";
    if (!k->body && !k->clip) { cpk_destroy(k); return NULL; }
    me_init(&k->eng, seed);
    float dur[ME_OWN_MAX]; const int n = cp_own_gestures(k->cp) < ME_OWN_MAX ? cp_own_gestures(k->cp) : ME_OWN_MAX;
    for (int i = 0; i < n; i++) dur[i] = cp_gesture_dur(k->cp, CUTE_N_ACTS + i);
    me_own(&k->eng, dur, n);
    return k;
}

void cpk_destroy(cpk_t *k) {
    if (!k) return;
    cb_destroy(k->body);
    cd_destroy(k->dots); K_FREE(k->cells); cr_destroy(k->ring);
    cc_destroy(k->clip);
    cp_destroy(k->cp);
    K_FREE(k);
}

mood_engine_t *cpk_engine(cpk_t *k) { return &k->eng; }
void cpk_set_work(cpk_t *k, bool on) { if (k) k->work = on; }
void cpk_set_idle(cpk_t *k, float idle_s, float life) { if (k) { k->idle_s = idle_s; k->life = life; } }
const cp_t *cpk_player(const cpk_t *k) { return k->cp; }
const cb_t *cpk_body(const cpk_t *k) { return k->body; }
int cpk_tris(const cpk_t *k) { return k && k->body ? cb_tris(k->body) : 0; }
void cpk_prepare(cpk_t *k, const fluid_sim_t *grid) {
    if (!k || !k->clip) return;
    if (!k->dots) k->dots = cd_create(k->clip, grid->W, grid->H);
    if (!k->ring) k->ring = cr_create(grid);
}
bool cpk_crisp(const cpk_t *k) { return k && k->clip; }
uint32_t cpk_draw_frame(const cpk_t *k) { return k ? FLUID_FRAME(k->frame, cr_ilace(k->ring)) : 0; }
void cpk_voice(cpk_t *k, float level) { if (k) cr_voice(k->ring, level); }
const uint8_t *cpk_cells_creature(const cpk_t *k) { return k && k->dots ? cd_creature(k->dots) : NULL; }
void cpk_listen(cpk_t *k, int32_t rgb, float ink) { if (k && k->ring) cr_listen(k->ring, rgb, ink); }
void cpk_set_share(cpk_t *k, int pct) { if (k && k->dots) cd_set_share(k->dots, pct); }
void cpk_come(cpk_t *k, const char *first) { if (k && k->clip) cc_come(k->clip, first); }
const cc_t *cpk_clip(const cpk_t *k) { return k ? k->clip : NULL; }

static const char *const MOOD_NAMES[7] = { "idle", "listening", "thinking", "speaking", "happy", "sleepy", "surprised" };

/* a rigged creature's frame: the engine's mood and each new gesture play go to the clip player, which
 * routes and plays; then the dots (cells: braille bytes and one colour each) */
static void clip_frame(cpk_t *k, fluid_sim_t *s, float dt) {
    k->dt = dt;
    FLUID_PROF_BEGIN(c0);
    me_pose_t e;
    me_update(&k->eng, dt, &e);
    const mood_engine_t *g = &k->eng;
    if (g->mood >= 0 && g->mood < 7) cc_mood(k->clip, MOOD_NAMES[g->mood]);
    if (g->act >= 0 && g->act_n != k->act_n && !cc_coming(k->clip)) { cc_act(k->clip, cp_gesture_name(k->cp, g->act)); }   /* a call first */
    k->act_n = g->act_n;
    if (cp_life(k->cp)) {                          /* its life: the device's inputs, its signals, its layers' places */
        cp_set(k->cp, k->in_free, cc_free(k->clip) ? 1.f : 0.f);
        cp_set(k->cp, k->in_idle, k->idle_s);
        cp_set(k->cp, k->in_life, k->life);
        cp_pose_t p = { .mood = g->mood, .work = k->work };
        static const cp_walk_t still;
        cp_pose(k->cp, &p, dt, NULL, NULL, 0.f, &still);
        for (int i = 0; i < cc_layers(k->clip); i++) cc_layer(k->clip, i, cp_get(k->cp, k->lay_by[i]), k->lay_w[i] < 0 ? 1.f : cp_get(k->cp, k->lay_w[i]));
    }
    cc_update(k->clip, dt);
    if (!k->dots) k->dots = cd_create(k->clip, s->W, s->H);
    if (!k->dots) { FLUID_PROF_END(FP_K_PLAY, c0); return; }
    FLUID_PROF_END(FP_K_PLAY, c0);
    cd_draw(k->dots, k->clip, k->frame++);
    {   /* the dots' stages into the fluid's profile (GET /api/creature/state prof_ms.k_*) */
        uint32_t st[CD_STAGES] = { 0 };
        cd_prof(k->dots, st);
        for (int i = 0; i < CD_STAGES; i++) fluid_prof[FP_K_PROJ + i] += st[i];
    }
    for (int i = 0; i < s->N; i++) s->T[i] = 0;    /* no ink: the dots are the creature */
}

void cpk_cells(const cpk_t *k, const uint8_t **dots, const uint32_t **rgb) {
    if (!k || !k->dots) { *dots = NULL; *rgb = NULL; return; }
    cd_cells(k->dots, dots, rgb);
    if (k->cells) *dots = k->cells;
}

void cpk_paint(cpk_t *k, fluid_sim_t *s) {
    if (!k || !k->dots) return;
    const uint8_t *dots; const uint32_t *rgb;
    cd_cells(k->dots, &dots, &rgb);
    if (!k->cells && !(k->cells = K_CALLOC((size_t)s->N))) return;
    for (int i = 0; i < s->N; i++) {               /* each cell shows its dots' colour at once, no flow */
        const uint32_t c = rgb[i];
        s->col[i].r = (uint16_t)((c >> 16 & 255) * 257); s->col[i].g = (uint16_t)((c >> 8 & 255) * 257); s->col[i].b = (uint16_t)((c & 255) * 257);
        s->col[i].a = 0;
    }
    memcpy(k->cells, dots, (size_t)s->N);
    /* the rings (a listen's, a question's, the agent's progress: the target's ink at the rim, after the creature's frame
     * cleared it) over them, as dust */
    if (!k->ring) k->ring = cr_create(s);
    if (k->ring) cr_paint(k->ring, s, k->dt, k->frame, cd_creature(k->dots), k->cells, s->col);
}

static void impulse(void *ctx, float x, float y, float s, float r) { fluid_sim_impulse(ctx, x, y, s, r); }
static void push(void *ctx, float x, float y, float vx, float vy, float r) { fluid_sim_push(ctx, x, y, vx, vy, r); }

void cpk_frame(cpk_t *k, fluid_sim_t *s, float dt, float swell) {
    dt = fminf(dt, 1.f / 30.f);                         /* CreatureTarget.update clamps both */
    if (k->clip) { FLUID_PROF_BEGIN(c0); clip_frame(k, s, dt); FLUID_PROF_END(FP_C_UPDATE, c0); return; }
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
