/*
 * SPDX-License-Identifier: MIT
 *
 * fluid_fine.c: a scene's fine dust in high definition (fluid_fine.h).
 */
#include "fluid_fine.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_timer.h"
#define FINE_CALLOC(n)     heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM)
#define FINE_REALLOC(p, n) heap_caps_realloc((p), (n), MALLOC_CAP_SPIRAM)
#define FINE_FREE(p)       heap_caps_free(p)
static inline int64_t now_us(void) { return esp_timer_get_time(); }
#else
#include <time.h>
#define FINE_CALLOC(n)     calloc(1, (n))
#define FINE_REALLOC(p, n) realloc((p), (n))
#define FINE_FREE(p)       free(p)
static inline int64_t now_us(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000; }
#endif

#define DOT_REFR  3.3f                  /* fluid_draw.c: the dots' shift, sub-dots per unit of wave slope */
#define FAR       0xFFFFu
#define LUT_N     1024                  /* e^(-d / FALL), e^(-d / TINT_FALL), d in 0.1 px steps */
/* the calm drift's two waves: cycles across the stage (u, v) and their periods (of FINE_DRIFT_S): a (1.1, 0.6) going one
 * way, b (-0.7, 1.2) the other, a third slower. Each sways a cell's dust on a small loop (x by its sine, y by its
 * cosine), as water carries what floats on it; the two together never repeat a pattern the eye can follow. */
#define DRIFT_AU   1.1f
#define DRIFT_AV   0.6f
#define DRIFT_BU  -0.7f
#define DRIFT_BV   1.2f
#define DRIFT_B_T  0.73f
#define SHIFT_HOLD 0.8f                 /* a cell's shift steps a pixel once the water or the drift is this far past it (0.5 would
                                         * round: a slope hovering there would flip it back and forth) */

enum { PH_IDLE, PH_RASTER, PH_FWD, PH_BWD, PH_EXTRACT, PH_LEVELS };

/* tables in PSRAM, made once (internal RAM is the network's) */
static float *s_near, *s_tint, *s_sin;
/* EXPERIMENT scene dust (fluid_fine.h; delete once Juan picks) */
static volatile int s_dust_kind = FINE_DUST_SPECKS;   /* (the fine specks by default: Juan rejected the dots alone) */
static volatile int s_hard_pct = FINE_HARD_PCT;
void  fluid_fine_dust_kind(int kind) { s_dust_kind = kind >= FINE_DUST_SPECKS && kind <= FINE_DUST_FINE_DOTS ? kind : FINE_DUST_SPECKS; }
int   fluid_fine_dust_kind_get(void) { return s_dust_kind; }
void  fluid_fine_hard_density(int percent) { s_hard_pct = percent < 0 ? 0 : percent > FINE_HARD_MAX ? FINE_HARD_MAX : percent; }
int   fluid_fine_hard_density_get(void) { return s_hard_pct; }
static bool s_lut;

static inline float hash_u(int a, int b, uint32_t c) {     /* fluid_draw.c's, chart-engine.js hashU */
    uint32_t h = (uint32_t)a * 0x27d4eb2du ^ (uint32_t)b * 0x165667b1u ^ c * 0x9e3779b1u;
    h ^= h >> 15; h *= 0x85ebca6bu; h ^= h >> 13;
    return (float)(h & 0xFFFFu) * (1.f / 65536.f);
}
/* sin of t turns (a table, linear; |t| < 2^20). The drawer's frame makes no libm call (each is one on the device: a
 * root, a sine or a floor a cell, or a speck, was milliseconds a frame) */
static inline float sin_t(float t) {
    const int n = (int)t;
    t -= (float)(n - (t < (float)n));
    const float q = t * 1024.f;
    const int i = (int)q;
    return s_sin[i] + (s_sin[i + 1] - s_sin[i]) * (q - (float)i);
}
/* sqrt without a libm call (fluid_draw.c's fsqrt: two Newton steps, ~1e-5) */
static inline float fsqrt(float x) {
    if (x <= 0.f) return 0.f;
    union { float f; uint32_t i; } c = { x };
    c.i = 0x5f3759dfu - (c.i >> 1);
    float y = c.f;
    y *= 1.5f - 0.5f * x * y * y;
    y *= 1.5f - 0.5f * x * y * y;
    return x * y;
}
/* v in whole px, the step held (*q) kept until v is SHIFT_HOLD past it */
static inline int held(int8_t *q, float v) {
    while (v > *q + SHIFT_HOLD) (*q)++;
    while (v < *q - SHIFT_HOLD) (*q)--;
    return *q;
}
/* the backdrop's level for the speck at stage px (x, y): it thins to none round the box, a ragged edge, between dlo
 * and dhi px of it (the fronts that bring it and lift it); 255 untouched */
static inline uint8_t backdrop_lv(const float *box, float dlo, float dhi, int x, int y) {
    const float px = x + 0.5f, py = y + 0.5f;
    const float ex = px < box[0] ? box[0] - px : px > box[1] ? px - box[1] : 0.f, ey = py < box[2] ? box[2] - py : py > box[3] ? py - box[3] : 0.f;
    const float dd = fsqrt(ex * ex + ey * ey);
    if (dd < dlo || dd >= dhi) return 255;
    float t = (dd + (hash_u(x, y, 5) - 0.5f) * FINE_PATCH_RAG - FINE_PATCH0) * (1.f / (FINE_PATCH1 - FINE_PATCH0));
    t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
    const float keep = t * t * (3.f - 2.f * t);
    return keep <= 0.02f ? 0 : (uint8_t)(keep * 255.f + 0.5f);
}
static inline uint16_t pack565(float r, float g, float b) {
    const int R = r >= 255.f ? 255 : r <= 0.f ? 0 : (int)(r + 0.5f), G = g >= 255.f ? 255 : g <= 0.f ? 0 : (int)(g + 0.5f),
              B = b >= 255.f ? 255 : b <= 0.f ? 0 : (int)(b + 0.5f);
    return (uint16_t)((R & 0xF8) << 8 | (G & 0xFC) << 3 | B >> 3);
}
/* the stable identity of what a speck shows where: the same speck at the same place, level and colour hashes the same in
 * any set (a new set changes only the cells whose dust changed) */
static inline uint32_t mix(uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9e3779b1u ^ (b + 0x7f4a7c15u) * 0x85ebca6bu;
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12;
    return h;
}

/* ── sets ──────────────────────────────────────────────────────────────────── */

static void set_free(fine_set_t *st) {
    FINE_FREE(st->pos); FINE_FREE(st->col); FINE_FREE(st->start); FINE_FREE(st->nstat); FINE_FREE(st->ssig);
    *st = (fine_set_t){ 0 };
}
static bool set_room(fine_set_t *st, int n) {
    if (n <= st->cap) return true;
    const int cap = n > 2 * st->cap ? n : 2 * st->cap;
    uint32_t *p = FINE_REALLOC(st->pos, sizeof(uint32_t) * (size_t)cap);
    if (!p) return false;
    st->pos = p;
    uint16_t *c = FINE_REALLOC(st->col, sizeof(uint16_t) * (size_t)cap);
    if (!c) return false;
    st->col = c;
    st->cap = cap;
    return true;
}

static bool light_tables(int lg, int S);

fluid_fine_t *fluid_fine_create(int W, int H, int fb_w, int fb_h) {
    if (W <= 2 || H <= 2 || W > 255 || H > 255 || fb_w < 32 || fb_h < 32 || fb_w > 1024 || fb_h > 1024) return NULL;
    if (!s_lut) {
        if (!s_near) s_near = FINE_CALLOC(sizeof(float) * (2 * LUT_N + 1025));
        if (!s_near) return NULL;
        s_tint = s_near + LUT_N; s_sin = s_tint + LUT_N;
        for (int i = 0; i < LUT_N; i++) { s_near[i] = expf(-i * 0.1f / FINE_FALL); s_tint[i] = expf(-i * 0.1f / FINE_TINT_FALL); }
        for (int i = 0; i <= 1024; i++) s_sin[i] = sinf(6.2831853f * i / 1024.f);
        s_lut = true;
    }
    const int S = fb_w < fb_h ? fb_w : fb_h;
    if (S > 511) return NULL;                                   /* a speck's x and y: 9 bits each */
    fluid_fine_t *f = FINE_CALLOC(sizeof(fluid_fine_t));
    if (!f) return NULL;
    f->W = W; f->H = H; f->N = W * H; f->S = S;
    f->ox = (fb_w - S) / 2; f->oy = (fb_h - S) / 2;
    f->lg = S / FINE_LIGHT_GRID + 3;
    f->S2 = (S + 1) / 2;
    int maxh = 0;
    f->cx = FINE_CALLOC(sizeof(int16_t) * (size_t)(W + 1)); f->cy = FINE_CALLOC(sizeof(int16_t) * (size_t)(H + 1));
    f->cellx = FINE_CALLOC((size_t)S); f->celly = FINE_CALLOC((size_t)S);
    f->shp = FINE_CALLOC(sizeof(fluid_shape_t) * 2 * FLUID_MAX_SHAPES); f->box = FINE_CALLOC(sizeof(int16_t) * 4 * 2 * FLUID_MAX_SHAPES);
    f->rowbuf = FINE_CALLOC(sizeof(uint16_t) * (size_t)S);
    f->sx = FINE_CALLOC((size_t)f->N); f->sy = FINE_CALLOC((size_t)f->N);
    f->sig = FINE_CALLOC(sizeof(uint32_t) * (size_t)f->N); f->sig0 = FINE_CALLOC(sizeof(uint32_t) * (size_t)f->N);
    f->lgrid = FINE_CALLOC((size_t)f->lg * (size_t)f->lg);
    f->px0 = FINE_CALLOC(sizeof(int16_t) * FINE_PARTICLES * 4); f->pc = FINE_CALLOC(sizeof(uint16_t) * FINE_PARTICLES * 2);
    f->pa = FINE_CALLOC(3 * FINE_PARTICLES);
    if (!f->cx || !f->cy || !f->cellx || !f->celly || !f->shp || !f->box || !f->rowbuf || !f->sx || !f->sy || !f->sig || !f->sig0 ||
        !f->lgrid || !f->px0 || !f->pc || !f->pa) { fluid_fine_destroy(f); return NULL; }
    f->py0 = f->px0 + FINE_PARTICLES; f->ppx = f->py0 + FINE_PARTICLES; f->ppy = f->ppx + FINE_PARTICLES; f->ppc = f->pc + FINE_PARTICLES;
    f->pg = f->pa + FINE_PARTICLES; f->pang = f->pg + FINE_PARTICLES;
    const float cw = (float)S / W, ch = (float)S / H;
    for (int i = 0; i <= W; i++) f->cx[i] = (int16_t)lrintf(i * cw);   /* fluid_draw_create's edges, less ox, oy */
    for (int j = 0; j <= H; j++) f->cy[j] = (int16_t)lrintf(j * ch);
    for (int i = 0; i < W; i++) for (int x = f->cx[i]; x < f->cx[i + 1] && x < S; x++) f->cellx[x] = (uint8_t)i;
    for (int j = 0; j < H; j++) {
        for (int y = f->cy[j]; y < f->cy[j + 1] && y < S; y++) f->celly[y] = (uint8_t)j;
        if (f->cy[j + 1] - f->cy[j] > maxh) maxh = f->cy[j + 1] - f->cy[j];
    }
    f->bcap = maxh * S;
    f->bpos = FINE_CALLOC(sizeof(uint32_t) * (size_t)f->bcap); f->bcol = FINE_CALLOC(sizeof(uint16_t) * (size_t)f->bcap);
    f->bcell = FINE_CALLOC(sizeof(uint16_t) * (size_t)f->bcap);
    f->dq = FINE_CALLOC(8 * (size_t)f->N); f->wq = f->dq + 2 * f->N; f->ride = (uint8_t *)(f->wq + 2 * f->N);
    f->sxd = (int8_t *)f->ride + f->N; f->syd = f->sxd + f->N; f->dok = (uint8_t *)f->syd + f->N; f->dcol = FINE_CALLOC(sizeof(float) * 4 * (size_t)W); f->vis = FINE_CALLOC((size_t)f->N); f->imm = FINE_CALLOC((size_t)f->N); f->why = FINE_CALLOC((size_t)f->N);
    f->tot = FINE_CALLOC(sizeof(uint32_t) * (size_t)f->N); f->stat = FINE_CALLOC(sizeof(uint32_t) * (size_t)f->N);
    f->eff = FINE_CALLOC(sizeof(uint32_t) * (size_t)f->N); f->tot2 = FINE_CALLOC(sizeof(uint32_t) * (size_t)f->N);
    f->fresh = FINE_CALLOC((size_t)f->N); f->lgrid2 = FINE_CALLOC((size_t)f->lg * (size_t)f->lg);
    /* the frame ahead (each array its own, as the drawer's: the two swap), its word's specks in one block */
    f->b.sig = FINE_CALLOC(sizeof(uint32_t) * (size_t)f->N); f->b.why = FINE_CALLOC((size_t)f->N); f->b.vis = FINE_CALLOC((size_t)f->N);
    f->b.imm = FINE_CALLOC((size_t)f->N); f->b.sx = FINE_CALLOC((size_t)f->N); f->b.sy = FINE_CALLOC((size_t)f->N);
    f->lvchg = FINE_CALLOC((size_t)f->N); f->pmem = FINE_CALLOC(sizeof(int16_t) * FINE_PARTICLES * 3);
    if (!f->bpos || !f->bcol || !f->bcell || !f->dq || !f->dcol || !f->vis || !f->imm || !f->why || !f->tot || !f->stat || !f->eff || !f->tot2 || !f->fresh || !f->lgrid2 ||
        !f->b.sig || !f->b.why || !f->b.vis || !f->b.imm || !f->b.sx || !f->b.sy || !f->lvchg || !f->pmem) {
        fluid_fine_destroy(f); return NULL;
    }
    if (!light_tables(f->lg, S)) { fluid_fine_destroy(f); return NULL; }   /* (one stage size a boot) */
    for (int i = 0; i < W; i++) {                               /* the drift waves' column parts (fluid_fine_frame) */
        const float u = (f->cx[i] + f->cx[i + 1]) * 0.5f / S;
        f->dcol[4 * i] = sinf(6.2831853f * DRIFT_AU * u); f->dcol[4 * i + 1] = cosf(6.2831853f * DRIFT_AU * u);
        f->dcol[4 * i + 2] = sinf(6.2831853f * DRIFT_BU * u); f->dcol[4 * i + 3] = cosf(6.2831853f * DRIFT_BU * u);
    }
    f->b.ppx = f->pmem; f->b.ppy = f->pmem + FINE_PARTICLES; f->b.ppc = (uint16_t *)f->pmem + 2 * FINE_PARTICLES;
    f->lvall = true;
    f->live = f->ready = f->build = -1;
    f->cbox[1] = f->pbox[1] = f->want_box[1] = f->bbox[1] = -1.f;   /* none */
    f->pbb[1] = f->pbb0[1] = -1;
    f->dim[5] = -1.f; f->dim[6] = 1e9f;
    return f;
}

void fluid_fine_destroy(fluid_fine_t *f) {
    if (!f) return;
    for (int q = 0; q < FINE_SETS; q++) set_free(&f->set[q]);
    FINE_FREE(f->cx); FINE_FREE(f->cy); FINE_FREE(f->cellx); FINE_FREE(f->celly); FINE_FREE(f->shp); FINE_FREE(f->box);
    FINE_FREE(f->rowbuf); FINE_FREE(f->sx); FINE_FREE(f->sy); FINE_FREE(f->sig); FINE_FREE(f->sig0); FINE_FREE(f->lgrid);
    FINE_FREE(f->px0); FINE_FREE(f->pc); FINE_FREE(f->pa); FINE_FREE(f->bpos); FINE_FREE(f->bcol); FINE_FREE(f->bcell);
    FINE_FREE(f->dq); FINE_FREE(f->dcol); FINE_FREE(f->vis); FINE_FREE(f->imm); FINE_FREE(f->why); FINE_FREE(f->tot); FINE_FREE(f->stat); FINE_FREE(f->eff);
    FINE_FREE(f->dist); FINE_FREE(f->src); FINE_FREE(f->lit); FINE_FREE(f->lv); FINE_FREE(f->lv2);
    FINE_FREE(f->tot2); FINE_FREE(f->fresh); FINE_FREE(f->lgrid2); FINE_FREE(f->lvd); FINE_FREE(f->lvd2);
    FINE_FREE(f->b.sig); FINE_FREE(f->b.why); FINE_FREE(f->b.vis); FINE_FREE(f->b.imm); FINE_FREE(f->b.sx); FINE_FREE(f->b.sy);
    FINE_FREE(f->b.lv); FINE_FREE(f->lvchg); FINE_FREE(f->pmem);
    FINE_FREE(f);
}

void fluid_fine_clear(fluid_fine_t *f) {
    if (!f) return;
    for (int q = 0; q < FINE_SETS; q++) set_free(&f->set[q]);
    FINE_FREE(f->dist); FINE_FREE(f->src); FINE_FREE(f->lit); FINE_FREE(f->lv); FINE_FREE(f->lv2);
    f->dist = NULL; f->src = NULL; f->lit = NULL; f->lv = f->lv2 = NULL; f->lvcap = f->lv2cap = 0;
    f->live = f->ready = f->build = -1; f->levels_ok = false; f->lv_base = NULL;
    f->phase = PH_IDLE; f->keep = 0; f->bkey = 0;
    f->drawn = NULL;
    memset(f->eff, 0, sizeof(uint32_t) * (size_t)f->N); memset(f->tot, 0, sizeof(uint32_t) * (size_t)f->N);
    memset(f->vis, 0, (size_t)f->N); memset(f->dok, 0, (size_t)f->N);
    f->word = 0; f->np = f->npd = 0; f->ncap = 0; f->end = false;
    f->cbox[1] = f->pbox[1] = f->want_box[1] = f->bbox[1] = -1.f; f->cbox[0] = f->pbox[0] = f->want_box[0] = f->bbox[0] = 0.f;
    f->dim[4] = 0.f; f->dim[5] = -1.f; f->dim[6] = 1e9f; f->dark_on = false; f->pbb[1] = f->pbb0[1] = -1;
    FINE_FREE(f->b.lv); f->b.lv = NULL; f->b.lvcap = 0; f->b.npd = 0; f->lvall = true; f->bready = f->ahead = false;
    memset(f->lvchg, 0, (size_t)f->N);
}

/* ── the build (sim task) ──────────────────────────────────────────────────── */

/* The field is kept at half the panel's resolution (2 x 2 px blocks: a quarter of the memory the build moves, which is
 * what it costs on the device) with a bit a panel pixel for the drawing itself (no speck on it). */

bool fluid_fine_want(fluid_fine_t *f, uint32_t scene, uint32_t key, bool keep, const fluid_shape_t *sh, int n, int palette) {
    if (!f) return true;
    if (!key) key = 1;
    if (keep) f->keep = key;
    f->scene = scene;
    f->clock++;
    if (f->live >= 0 && f->set[f->live].key == key) {            /* drawn now */
        f->set[f->live].base = scene; f->set[f->live].used = f->clock;
        f->ready = -1; f->build = -1; f->phase = PH_IDLE;
        return true;
    }
    for (int q = 0; q < FINE_SETS; q++)
        if (q != f->live && f->set[q].key == key && q != f->build) {   /* built before: its levels, then it swaps in */
            if (f->ready != q) {
                f->ready = q; f->levels_ok = false; f->phase = PH_LEVELS; f->row = 0;
                f->build_t0 = now_us(); f->build_us = 0; f->build_calls = 0;
            }
            f->set[q].base = scene; f->set[q].used = f->clock;
            f->build = -1;
            return true;
        }
    if (f->build >= 0 && f->bkey == key) return true;            /* building */
    /* the scene's calm set is never given up for a tapped spot's: that one waits its turn */
    if (f->build >= 0 && f->bkey == f->keep && key != f->keep) return false;
    /* a slot: not the one drawn nor the calm drawing's; an empty one, else the one used longest ago */
    int slot = -1;
    for (int q = 0; q < FINE_SETS; q++) {
        if (q == f->live || (f->set[q].key && f->set[q].key == f->keep)) continue;
        if (slot < 0 || !f->set[q].key || (f->set[slot].key && f->set[q].used < f->set[slot].used)) slot = q;
        if (!f->set[q].key) break;
    }
    if (slot < 0) return true;
    const int S2 = f->S2;
    if (!f->dist) f->dist = FINE_CALLOC(sizeof(uint16_t) * (size_t)S2 * S2);
    if (!f->src) f->src = FINE_CALLOC(sizeof(uint16_t) * (size_t)S2 * S2);
    if (!f->lit) f->lit = FINE_CALLOC(((size_t)f->S * f->S + 7) / 8);
    if (!f->dist || !f->src || !f->lit) return true;             /* no memory: no dust */
    fine_set_t *nset = &f->set[slot];
    if (!nset->start) nset->start = FINE_CALLOC(sizeof(uint32_t) * (size_t)(f->N + 1));
    if (!nset->nstat) nset->nstat = FINE_CALLOC(sizeof(uint16_t) * (size_t)f->N);
    if (!nset->ssig) nset->ssig = FINE_CALLOC(sizeof(uint32_t) * (size_t)f->N);
    if (!nset->start || !nset->nstat || !nset->ssig) return true;
    /* the drawing's shapes (not the caption), then a copy of its symbols alone */
    int m = 0, ns = 0;
    for (int q = 0; q < n && m < FLUID_MAX_SHAPES; q++) if (sh[q].tag != FLUID_TAG_CAPTION) f->shp[m++] = sh[q];
    for (int q = 0; q < m; q++) if (f->shp[q].k == FLUID_SHAPE_MASK || f->shp[q].k == FLUID_SHAPE_CHAR) f->shp[m + ns++] = f->shp[q];
    for (int q = 0; q < m + ns; q++) fluid_shape_box(&f->shp[q], 0, 0, f->box[q]);
    f->nshp = m; f->nsym = ns; f->pal = palette;
    nset->key = 0; nset->n = 0; nset->base = scene; nset->used = f->clock;
    if (f->ready == slot) f->ready = -1;
    f->build = slot; f->bkey = key; f->phase = PH_RASTER; f->row = 0;
    f->build_t0 = now_us(); f->build_us = 0; f->build_calls = 0;
    memset(f->lit, 0, ((size_t)f->S * f->S + 7) / 8);
    return true;
}

/* a raster row's lit pixels into its half-resolution row (dist 0 and the colour, rgb444) and the lit bits; sym: mark
 * the lit ones as a symbol's instead */
static void fold_row(fluid_fine_t *f, int y, bool sym) {
    const int S = f->S;
    const uint16_t *row = f->rowbuf;
    uint16_t *dist = f->dist + (size_t)(y >> 1) * f->S2, *src = f->src + (size_t)(y >> 1) * f->S2;
    uint8_t *lit = f->lit;
    for (int x = 0; x < S; x++) {
        if (!row[x]) continue;
        const uint16_t le = (uint16_t)(row[x] >> 8 | row[x] << 8);
        const int r5 = le >> 11, g6 = le >> 5 & 63, b5 = le & 31;
        if (r5 <= FINE_DRAWN >> 3 && g6 <= FINE_DRAWN >> 2 && b5 <= FINE_DRAWN >> 3) continue;
        const int x2 = x >> 1;
        if (sym) { if (!dist[x2]) src[x2] |= 0x8000; continue; }
        const size_t bit = (size_t)y * S + x;
        lit[bit >> 3] |= (uint8_t)(1u << (bit & 7));
        const uint16_t c = (uint16_t)((r5 >> 1) << 8 | (g6 >> 2) << 4 | b5 >> 1);
        if (dist[x2] || (c & 0x0F0) > (src[x2] & 0x0F0)) src[x2] = c;   /* the brighter of the block's */
        dist[x2] = 0;
    }
}
/* half-resolution row y2: its two panel rows of the drawing, then of its symbols alone */
static void raster_row(fluid_fine_t *f, int y2) {
    const int S = f->S;
    uint16_t *row = f->rowbuf, *dist = f->dist + (size_t)y2 * f->S2, *src = f->src + (size_t)y2 * f->S2;
    for (int x = 0; x < f->S2; x++) { dist[x] = FAR; src[x] = 0; }
    for (int pass = 0; pass < (f->nsym ? 2 : 1); pass++)
        for (int y = 2 * y2; y < 2 * y2 + 2 && y < S; y++) {
            memset(row, 0, sizeof(uint16_t) * (size_t)S);
            if (!pass) fluid_shapes_row(f->shp, f->nshp, (const int16_t (*)[4])f->box, 0, 0, f->pal, NULL, 0.f, row, 0, y, 0, S);
            else fluid_shapes_row(f->shp + f->nshp, f->nsym, (const int16_t (*)[4])(f->box + f->nshp), 0, 0, f->pal, NULL, 0.f, row, 0, y, 0, S);
            fold_row(f, y, pass != 0);
        }
}

/* the chamfer pass over half-resolution row y (a block 2 px, its diagonal 2.83, in 1/100 px): forward from the left
 * and the row above, or back */
static void chamfer_row(fluid_fine_t *f, int y, bool fwd) {
    const int S = f->S2;
    uint16_t *D = f->dist, *C = f->src;
    const int o = fwd ? -1 : 1, oy = fwd ? -S : S;
    const bool rowok = fwd ? y > 0 : y < S - 1;
    for (int q = 0; q < S; q++) {
        const int x = fwd ? q : S - 1 - q, i = y * S + x;
        int d = D[i];
        if (!d) continue;
        int s = C[i];
        if ((fwd ? x > 0 : x < S - 1) && D[i + o] + 200 < d) { d = D[i + o] + 200; s = C[i + o]; }
        if (rowok) {
            if (D[i + oy] + 200 < d) { d = D[i + oy] + 200; s = C[i + oy]; }
            if (x > 0 && D[i + oy - 1] + 283 < d) { d = D[i + oy - 1] + 283; s = C[i + oy - 1]; }
            if (x < S - 1 && D[i + oy + 1] + 283 < d) { d = D[i + oy + 1] + 283; s = C[i + oy + 1]; }
        }
        D[i] = (uint16_t)(d > (int)FAR ? FAR : d); C[i] = (uint16_t)s;
    }
}

/* cell row j's specks, in cell order, onto the set */
static bool extract(fluid_fine_t *f, fine_set_t *st, int j) {
    const int S = f->S, W = f->W, S2 = f->S2;
    const float c = S * 0.5f, R = S * 0.5f - 2.f;
    int nb = 0;
    for (int y = f->cy[j]; y < f->cy[j + 1] && y < S; y++) {
        if (y < 1 || y > S - 2) continue;
        const float dy = y + 0.5f - c, h2 = R * R - dy * dy;
        if (h2 <= 0.f) continue;
        const float hw = sqrtf(h2);
        int x0 = (int)ceilf(c - hw - 0.5f), x1 = (int)floorf(c + hw - 0.5f);
        x0 = x0 < 1 ? 1 : x0; x1 = x1 > S - 2 ? S - 2 : x1;
        const uint16_t *D = f->dist + (size_t)(y >> 1) * S2, *C = f->src + (size_t)(y >> 1) * S2;
        const uint8_t *lit = f->lit;
        const float gy = 0.00583715f * (float)y;
        for (int x = x0; x <= x1; x++) {
            const size_t bit = (size_t)y * S + x;
            if (lit[bit >> 3] >> (bit & 7) & 1) continue;       /* on the drawing */
            /* its distance: a block by the drawing's is a pixel or so from it, the others from their block's centre */
            const int db = D[x >> 1], li = db ? (db - 50) / 10 : 10;
            const float near = li < LUT_N ? s_near[li] : 0.f;
            const uint16_t cs = C[x >> 1];
            const bool sym = cs & 0x8000;
            const float dens = FINE_AMBIENT + FINE_PEAK * near * (sym ? FINE_SYM : 1.f);
            /* interleaved gradient noise (ign()), with no call for its two floors: the pixel's ordered threshold */
            float g = 0.06711056f * (float)x + gy;
            g = 52.9829189f * (g - (float)(int)g);
            g -= (float)(int)g;
            if (g >= dens) continue;
            const float lv = (FINE_LEVEL_FAR + (1.f - FINE_LEVEL_FAR) * near) * (sym ? 0.6f : 1.f), tint = sym || li >= LUT_N ? 0.f : s_tint[li];
            const float sr = (float)((cs >> 8 & 15) * 17), sg = (float)((cs >> 4 & 15) * 17), sb = (float)((cs & 15) * 17);
            const uint16_t col = pack565((FINE_WATER_R + (sr - FINE_WATER_R) * tint) * lv, (FINE_WATER_G + (sg - FINE_WATER_G) * tint) * lv,
                                         (FINE_WATER_B + (sb - FINE_WATER_B) * tint) * lv);
            int q = (int)(g / dens * 127.f), nr = (int)(near * 0.8f * 127.f + 0.5f);
            q = q > 127 ? 127 : q; nr = nr > 127 ? 127 : nr;
            if (nb >= f->bcap) break;
            f->bpos[nb] = (uint32_t)x | (uint32_t)y << 9 | (uint32_t)q << 18 | (uint32_t)nr << 25;
            /* the light never hides it when its threshold is under the least density it can have here */
            const bool dyn = (float)q >= 63.5f * (1.f + nr * (1.f / 127.f));
            f->bcol[nb] = col; f->bcell[nb] = (uint16_t)(f->cellx[x] * 2 + dyn);
            nb++;
        }
    }
    if (!set_room(st, st->n + nb)) return false;
    /* counting sort by cell column, those the light never hides first; their calm signature */
    int cnt[2 * 256 + 1];
    memset(cnt, 0, sizeof(int) * (size_t)(2 * W + 1));
    for (int q = 0; q < nb; q++) cnt[f->bcell[q] + 1]++;
    for (int c2 = 0; c2 < 2 * W; c2++) cnt[c2 + 1] += cnt[c2];
    for (int i = 0; i < W; i++) {
        st->start[j * W + i] = (uint32_t)(st->n + cnt[2 * i]);
        st->nstat[j * W + i] = (uint16_t)(cnt[2 * i + 1] - cnt[2 * i]);
        st->ssig[j * W + i] = 0;
    }
    for (int q = 0; q < nb; q++) {
        const int o = st->n + cnt[f->bcell[q]]++;
        st->pos[o] = f->bpos[q]; st->col[o] = f->bcol[q];
        if (!(f->bcell[q] & 1)) st->ssig[j * W + (f->bcell[q] >> 1)] += mix(f->bpos[q] & 0x3FFFF, 255u << 16 | f->bcol[q]);
    }
    st->n += nb;
    return true;
}

static void light_grid(const fluid_fine_t *f, uint8_t *grid, float T, int parity);

/* Cell k of the set that waits (f->ready): is it the cell the drawer shows now (the same specks, worked out)? Then it
 * keeps its levels at the swap. Else its levels afresh into f->lv2 and its signature into f->tot2: those the light never
 * hides whole, the others against the light as it is now (f->lgrid2). */
static void levels_cell(fluid_fine_t *f, const fine_set_t *st, const fine_set_t *old, int k) {
    const uint32_t a = st->start[k], e = st->start[k + 1], sm = a + st->nstat[k];
    const bool same = old && f->eff[k] && old->start[k + 1] - old->start[k] == e - a && old->nstat[k] == st->nstat[k] && old->ssig[k] == st->ssig[k];
    f->fresh[k] = !same;
    if (same || a == e) return;
    const int LG = f->lg;
    memset(f->lv2 + a, 255, sm - a);
    uint32_t acc = st->ssig[k];
    for (uint32_t n = sm; n < e; n++) {
        const uint32_t p = st->pos[n];
        const int x = (int)(p & 511), y = (int)(p >> 9 & 511), q = (int)(p >> 18 & 127);
        const float gx = (x + 0.5f) * (1.f / FINE_LIGHT_GRID), gy = (y + 0.5f) * (1.f / FINE_LIGHT_GRID);
        const int ix = (int)gx, iy = (int)gy;
        const float ax = gx - ix, ay = gy - iy;
        const uint8_t *g0 = f->lgrid2 + iy * LG + ix;
        const float l = ((g0[0] * (1.f - ax) + g0[1] * ax) * (1.f - ay) + (g0[LG] * (1.f - ax) + g0[LG + 1] * ax) * ay) * (1.f / 255.f);
        const float nr = (float)(p >> 25) * (1.f / 127.f);
        const uint8_t lv = (float)q < (FINE_LIGHT_FLOOR + (1.f - FINE_LIGHT_FLOOR) * (l + (1.f - l) * nr)) * 127.f ? 255 : 0;
        f->lv2[n] = lv;
        if (lv) acc += mix(p & 0x3FFFF, 255u << 16 | st->col[n]);
    }
    f->tot2[k] = acc;
}

bool fluid_fine_work(fluid_fine_t *f, int budget_us) {
    if (!f) return false;
    if (f->build < 0 && (f->ready < 0 || f->levels_ok)) return f->ready >= 0;
    const int64_t t0 = now_us();
    const int S2 = f->S2;
    f->build_calls++;
    for (;;) {
        if (budget_us > 0 && now_us() - t0 >= budget_us) return false;
        fine_set_t *st = f->build >= 0 ? &f->set[f->build] : NULL;
        switch (f->phase) {
        case PH_RASTER: raster_row(f, f->row); if (++f->row >= S2) { f->phase = PH_FWD; f->row = 0; } break;
        case PH_FWD:    chamfer_row(f, f->row, true); if (++f->row >= S2) { f->phase = PH_BWD; f->row = S2 - 1; } break;
        case PH_BWD:    chamfer_row(f, f->row, false); if (--f->row < 0) { f->phase = PH_EXTRACT; f->row = 0; st->n = 0; } break;
        case PH_EXTRACT:
            if (!extract(f, st, f->row)) { f->phase = PH_IDLE; f->build = -1; return false; }   /* no memory */
            if (++f->row >= f->H) {
                st->start[f->N] = (uint32_t)st->n;
                st->key = f->bkey;
                if (st->cap > st->n + 64) {             /* its room, down to what it holds */
                    uint32_t *p = FINE_REALLOC(st->pos, sizeof(uint32_t) * (size_t)(st->n + 1));
                    uint16_t *c = FINE_REALLOC(st->col, sizeof(uint16_t) * (size_t)(st->n + 1));
                    if (p) st->pos = p;
                    if (c) st->col = c;
                    if (p || c) st->cap = st->n + 1;
                }
                f->ready = f->build; f->build = -1;
                f->levels_ok = false; f->phase = PH_LEVELS; f->row = 0;
            }
            break;
        case PH_LEVELS: {
            const fine_set_t *nw = &f->set[f->ready];
            if (!f->row) {                              /* room for its levels; the light as it is now */
                if (nw->n > f->lv2cap) {
                    FINE_FREE(f->lv2);
                    f->lv2 = FINE_CALLOC((size_t)nw->n + 1);
                    f->lv2cap = f->lv2 ? nw->n : 0;
                }
                if (nw->n > f->lv2cap) { f->ready = -1; f->phase = PH_IDLE; return false; }   /* no memory: no dust */
                f->lv_base = f->drawn && f->drawn->base == f->scene ? f->drawn : NULL;   /* (as the swap will find it) */
                light_grid(f, f->lgrid2, f->last_t * FINE_LIGHT_SPEED, -1);
            }
            for (int q = 0; q < 64 && f->row < f->N; q++, f->row++) levels_cell(f, nw, f->lv_base, f->row);
            if (f->row >= f->N) {
                f->levels_ok = true; f->phase = PH_IDLE;
                f->build_us = (int)(now_us() - f->build_t0);
                return true;
            }
            break;
        }
        default: return f->ready >= 0 && f->levels_ok;
        }
    }
}

int fluid_fine_built(const fluid_fine_t *f, int *calls) {
    if (calls) *calls = f ? f->build_calls : 0;
    return f ? f->build_us : 0;
}

bool fluid_fine_swap(fluid_fine_t *f) {
    if (!f) return false;
    if (f->bready && fluid_fine_swap_due(f)) fluid_fine_publish(f, false);   /* (a frame worked out for the old set: shown first) */
    /* the dust on the glass is the scene's own: a set made for another drawing goes at once */
    if (f->drawn && f->drawn->base != f->scene) { f->drawn = NULL; f->live = -1; }
    if (f->ready < 0 || !f->levels_ok) return false;
    if (f->lv_base != f->drawn) { f->levels_ok = false; f->phase = PH_LEVELS; f->row = 0; return false; }   /* (worked out against another) */
    const fine_set_t *old = f->drawn, *nw = &f->set[f->ready];
    /* a cell whose specks are the same in both sets keeps its levels and its signature: nothing of it is drawn again;
     * the others take those worked out for them (fluid_fine_work) */
    int kept = 0;
    for (int k = 0; k < f->N; k++) {
        const uint32_t a = nw->start[k], n = nw->start[k + 1] - a;
        if (!f->fresh[k]) { memcpy(f->lv2 + a, f->lv + old->start[k], n); kept++; }
        else { f->eff[k] = n ? 1u : 0u; f->tot[k] = n ? f->tot2[k] : 0u; f->stat[k] = nw->ssig[k]; }
    }
    uint8_t *t = f->lv; f->lv = f->lv2; f->lv2 = t;
    const int c = f->lvcap; f->lvcap = f->lv2cap; f->lv2cap = c;
    f->live = f->ready; f->ready = -1; f->levels_ok = false;
    f->drawn = nw;
    f->set[f->live].used = ++f->clock;
    f->lvall = true;                                    /* (the frame ahead's levels: the old set's) */
    /* the specks as drawn (fluid_fine_move): a kept cell's go on, at the new set's places (a tap's set would otherwise
     * have its wave redraw every cell it crosses whole once); the others' are not known */
    if (nw->n > f->lvd2cap) {
        FINE_FREE(f->lvd2);
        f->lvd2 = FINE_CALLOC((size_t)nw->n + 1);
        f->lvd2cap = f->lvd2 ? nw->n : 0;
    }
    for (int k = 0; k < f->N; k++) {
        if (!f->lvd2 || !old || !f->lvd || f->fresh[k] || !f->dok[k]) { f->dok[k] = 0; continue; }
        memcpy(f->lvd2 + nw->start[k], f->lvd + old->start[k], nw->start[k + 1] - nw->start[k]);
    }
    uint8_t *ld = f->lvd; f->lvd = f->lvd2; f->lvd2 = ld;
    const int lc = f->lvdcap; f->lvdcap = f->lvd2cap; f->lvd2cap = lc;
    return kept * 2 < f->N;
}

void fluid_fine_rim(fluid_fine_t *f, bool on) { if (f) f->rim = on; }

void fluid_fine_caption_box(fluid_fine_t *f, const float box[4]) {
    if (!f) return;
    if (box) memcpy(f->want_box, box, sizeof(f->want_box));
    else { f->want_box[0] = 0.f; f->want_box[1] = -1.f; }
}

/* ── the drawer's frame ────────────────────────────────────────────────────── */

/* Light on moving water over the grid at T = t x speed (chart-engine.js light()): 0..1, bright in thin ridges where two
 * slowly warping wave fronts cross zero:
 *   a = sin(2 pi (1.4u + 0.5v) + 0.31T + 1.6 sin(2 pi (0.8v - 0.55u) + 0.21T)),
 *   b = sin(2 pi (-0.7u + 1.25v) - 0.27T + 1.6 sin(2 pi (0.95u + 0.35v) - 0.17T)), light = min(1, (1-|a|)^3 + (1-|b|)^3).
 * Each phase is a sum of a column's part and a row's, so sin and cos of it come from per-column and per-row tables
 * (sin(x + y) = sx cy + cx sy), and sin / cos of 1.6 s from a table: a few multiplies a grid point. */
#define LG_MAX 136
static float (*s_cu)[LG_MAX], (*s_k16)[258];           /* per column: sin, cos of each wave's two u parts (PSRAM) */
/* its tables, once (fluid_fine_create: before the sim task and the drawer both use them) */
static bool light_tables(int lg, int S) {
    if (s_cu) return true;
    s_cu = FINE_CALLOC(sizeof(float) * (8 * LG_MAX + 2 * 258));
    if (!s_cu) return false;
    s_k16 = (float (*)[258])(s_cu + 8);
    const float tau = 6.2831853f, g = (float)FINE_LIGHT_GRID / S;
    static const float ku[4] = { 1.4f, -0.55f, -0.7f, 0.95f };
    for (int x = 0; x < (lg > LG_MAX ? LG_MAX : lg); x++) for (int q = 0; q < 4; q++) { s_cu[2 * q][x] = sinf(tau * ku[q] * x * g); s_cu[2 * q + 1][x] = cosf(tau * ku[q] * x * g); }
    for (int i = 0; i <= 257; i++) { const float sv = -1.f + 2.f * i / 256.f; s_k16[0][i] = sinf(1.6f * sv); s_k16[1][i] = cosf(1.6f * sv); }
    return true;
}
static void light_grid(const fluid_fine_t *f, uint8_t *grid, float T, int parity) {   /* rows parity, parity + FINE_PHASES ..; < 0: every row */
    float (*const cu)[LG_MAX] = s_cu, (*const k16)[258] = s_k16;
    const int LG = f->lg > LG_MAX ? LG_MAX : f->lg;
    const float tau = 6.2831853f, g = (float)FINE_LIGHT_GRID / f->S;
    static const float kv[4] = { 0.5f, 0.8f, 1.25f, 0.35f }, kt[4] = { 0.31f, 0.21f, -0.27f, -0.17f };
    for (int y = parity > 0 ? parity : 0; y < LG; y += parity < 0 ? 1 : FINE_PHASES) {
        float sv[4], cv[4];
        for (int q = 0; q < 4; q++) { const float ph = kv[q] * y * g + kt[q] * T * (1.f / tau); sv[q] = sin_t(ph); cv[q] = sin_t(ph + 0.25f); }   /* (turns) */
        uint8_t *out = grid + y * f->lg;
        for (int x = 0; x < LG; x++) {
            float r = 0.f;
            for (int w = 0; w < 2; w++) {
                const int p = 2 * w, q = 2 * w + 1;               /* the outer phase's tables, the inner's */
                const float sq = sv[q] * cu[2 * q + 1][x] + cv[q] * cu[2 * q][x];   /* sin(inner) */
                const float fi = (sq + 1.f) * 128.f;
                int ii = (int)fi;
                ii = ii < 0 ? 0 : ii > 255 ? 255 : ii;
                const float fr = fi - ii, s16 = k16[0][ii] + (k16[0][ii + 1] - k16[0][ii]) * fr, c16 = k16[1][ii] + (k16[1][ii + 1] - k16[1][ii]) * fr;
                const float sp = sv[p] * cu[2 * p + 1][x] + cv[p] * cu[2 * p][x], cp = cv[p] * cu[2 * p + 1][x] - sv[p] * cu[2 * p][x];
                const float a = sp * c16 + cp * s16, ia = 1.f - fabsf(a);
                r += ia * ia * ia;
            }
            out[x] = (uint8_t)((r > 1.f ? 1.f : r) * 255.f + 0.5f);
        }
    }
}

/* how near cell (i, j) comes to box (stage px [x0, x1, y0, y1]) and how far its farthest corner is, squared */
static void cell_box_dist(const fluid_fine_t *f, int i, int j, const float *box, float *lo2, float *hi2) {
    const float x0 = f->cx[i], x1 = f->cx[i + 1], y0 = f->cy[j], y1 = f->cy[j + 1];
    const float ex = x1 < box[0] ? box[0] - x1 : x0 > box[1] ? x0 - box[1] : 0.f, ey = y1 < box[2] ? box[2] - y1 : y0 > box[3] ? y0 - box[3] : 0.f;
    const float ax = box[0] - x0 > x1 - box[1] ? box[0] - x0 : x1 - box[1], ay = box[2] - y0 > y1 - box[3] ? box[2] - y0 : y1 - box[3];
    const float fx = ax > 0.f ? ax : 0.f, fy = ay > 0.f ? ay : 0.f;
    *lo2 = ex * ex + ey * ey; *hi2 = fx * fx + fy * fy;
}
/* a distance (given squared) under v px */
static inline bool under(float d2, float v) { return v > 0.f && d2 < v * v; }

/* the caption's word: its box (stage px, the letters' whole boxes) from its letters' shapes; false: none */
static bool caption_box(const fluid_shape_t *sh, int n, float b[4]) {
    bool any = false;
    for (int q = 0; q < n; q++) {
        if (sh[q].tag != FLUID_TAG_CAPTION || sh[q].k != FLUID_SHAPE_CHAR) continue;
        const float hw = sh[q].w / 0.8f * 0.5f, hh = sh[q].h / 0.72f * 0.5f;   /* a letter's glyph is 0.8 x 0.72 of its box */
        const float x0 = sh[q].x - hw, x1 = sh[q].x + hw, y0 = sh[q].y - hh, y1 = sh[q].y + hh;
        if (!any) { b[0] = x0; b[1] = x1; b[2] = y0; b[3] = y1; any = true; continue; }
        b[0] = fminf(b[0], x0); b[1] = fmaxf(b[1], x1); b[2] = fminf(b[2], y0); b[3] = fmaxf(b[3], y1);
    }
    return any;
}

/* the word before as specks: its pixels covered 0.3 or more (GRAIN of them), each to scatter from its centre */
static void break_up(fluid_fine_t *f, int pal) {
    /* Straight from each letter's 5 x 7 bits (no letter is drawn again for it: on the device that raster and a root,
     * an angle and three divisions a pixel were ~15 ms a word, a frame late at every word): the pixels of its lit
     * bits, FINE_GRAIN of the inner ones (a drawn letter's edge is soft: about half of a bit's square is lit enough
     * to have counted), each in the letter's colour, leaving along the way out from the word's middle. */
    const int S = f->S;
    const float cx = (f->pbox[0] + f->pbox[1]) * 0.5f, cy = (f->pbox[2] + f->pbox[3]) * 0.5f;
    f->np = 0;
    for (int q = 0; q < f->ncap; q++) {
        const fluid_shape_t *sh = &f->cap[q];
        if (sh->k != FLUID_SHAPE_CHAR || !sh->mask || sh->mw < 1 || sh->mh < 1) continue;
        const uint16_t col = fluid_shape_565(sh, pal);
        if (!col) continue;
        const float bw = sh->w / sh->mw, bh = sh->h / sh->mh, x0 = sh->x - sh->w * 0.5f, y0 = sh->y - sh->h * 0.5f;
        for (int r = 0; r < sh->mh; r++) for (int c = 0; c < sh->mw; c++) {
            if (!(sh->mask[r] >> (sh->mw - 1 - c) & 1)) continue;
            const int xa = (int)(x0 + c * bw + 0.5f), xb = (int)(x0 + (c + 1) * bw + 0.5f), ya = (int)(y0 + r * bh + 0.5f), yb = (int)(y0 + (r + 1) * bh + 0.5f);
            const int base = (int)((fluid_atan2((ya + yb) * 0.5f - cy, (xa + xb) * 0.5f - cx) + 6.2831853f) * (256.f / 6.2831853f));
            for (int y = ya; y < yb; y++) for (int x = xa; x < xb && f->np < FINE_PARTICLES; x++) {
                if (x < 1 || y < 1 || x >= S - 1 || y >= S - 1) continue;
                uint32_t h = (uint32_t)x * 0x27d4eb2du ^ (uint32_t)y * 0x165667b1u ^ 5u * 0x9e3779b1u;
                h ^= h >> 15; h *= 0x85ebca6bu; h ^= h >> 13;
                if ((h & 0xFFFFu) >= (uint32_t)(FINE_GRAIN * 0.5f * 65536.f)) continue;
                const int p = f->np++;
                f->px0[p] = (int16_t)x; f->py0[p] = (int16_t)y;
                f->pc[p] = col; f->pa[p] = 255; f->pg[p] = (uint8_t)(h >> 16);
                f->pang[p] = (uint8_t)(base + (((int)(h >> 24) - 128) * 90 >> 8));   /* +- 2.2 / 2 rad round the way out */
            }
        }
    }
}

/* b's levels as the drawer's (lv) where they are behind it: the cells it changed since b was last worked on, or all of
 * them once a set swapped in. False: no room (no specks this frame) */
static bool back_levels(fluid_fine_t *f) {
    fine_back_t *B = &f->b;
    const fine_set_t *st = f->drawn;
    if (!f->lv || !st) return false;
    if (f->lvall) {
        if (B->lvcap < f->lvcap) {
            FINE_FREE(B->lv);
            B->lv = FINE_CALLOC((size_t)f->lvcap + 1);
            B->lvcap = B->lv ? f->lvcap : 0;
        }
        if (!B->lv) return false;
        memcpy(B->lv, f->lv, (size_t)f->lvcap);
        memset(f->lvchg, 0, (size_t)f->N);
        f->lvall = false;
        return true;
    }
    for (int k = 0; k < f->N; k++)
        if (f->lvchg[k] & 2) { memcpy(B->lv + st->start[k], f->lv + st->start[k], st->start[k + 1] - st->start[k]); f->lvchg[k] &= 1; }
    return true;
}

void fluid_fine_frame(fluid_fine_t *f, const fluid_frame_t *s, const fluid_draw_opts_t *o) {
    fluid_fine_frame_ahead(f, s, o);
    fluid_fine_publish(f, false);
}

void fluid_fine_publish(fluid_fine_t *f, bool ahead) {
    if (!f) return;
    f->ahead = ahead && f->bready;
    if (!f->bready) return;
    fine_back_t *B = &f->b;
#define FINE_SWAP(a, b) do { __typeof__(a) t_ = (a); (a) = (b); (b) = t_; } while (0)
    FINE_SWAP(f->sig, B->sig); FINE_SWAP(f->why, B->why); FINE_SWAP(f->vis, B->vis); FINE_SWAP(f->imm, B->imm);
    FINE_SWAP(f->sx, B->sx); FINE_SWAP(f->sy, B->sy); FINE_SWAP(f->ppx, B->ppx); FINE_SWAP(f->ppy, B->ppy); FINE_SWAP(f->ppc, B->ppc);
    if (B->lv) { FINE_SWAP(f->lv, B->lv); FINE_SWAP(f->lvcap, B->lvcap); }
#undef FINE_SWAP
    f->npd = B->npd;
    memcpy(f->pbb, B->pbb, sizeof(f->pbb)); memcpy(f->pbb0, B->pbb0, sizeof(f->pbb0)); memcpy(f->dim, B->dim, sizeof(f->dim));
    for (int k = 0; k < f->N; k++) f->lvchg[k] = f->lvchg[k] & 1 ? 2 : 0;   /* (b is now the frame before: behind where it changed) */
    f->bready = false;
}

void fluid_fine_frame_ahead(fluid_fine_t *f, const fluid_frame_t *s, const fluid_draw_opts_t *o) {
    const int64_t t_in = now_us();
    const int W = f->W, H = f->H, S = f->S, N = f->N;
    fine_back_t *B = &f->b;
    const bool lv_ok = back_levels(f);
    memset(B->sig, 0, sizeof(uint32_t) * (size_t)N);         /* f->sig0: as each cell was last drawn (fluid_draw_cells) */
    memset(B->imm, 0, (size_t)N);
    memset(B->vis, 0, (size_t)N);
    memset(B->why, 0, (size_t)N);
    memcpy(B->sx, f->sx, (size_t)N); memcpy(B->sy, f->sy, (size_t)N);   /* (cells with no speck, or a pause, keep theirs) */
    memcpy(B->dim, f->dim, sizeof(f->dim));
    const float now = o->time;
    if (!f->lgrid_ok) f->light_t = now;
    else if (!f->pause) { const float dt = now - f->last_t; f->light_t += dt < 0.f ? 0.f : dt > 0.1f ? 0.1f : dt; }
    f->last_t = now;

    /* the caption: a new word (or none) breaks the one before into specks */
    float b[4];
    const bool has = o->cap_word && caption_box(o->shapes, o->shapes ? o->nshapes : 0, b);
    const unsigned word = has ? o->cap_word : 0;
    bool word_new = false;
    if (word != f->word) {
        if (f->word && f->ncap && f->cbox[1] >= f->cbox[0]) {
            memcpy(f->pbox, f->cbox, sizeof(f->pbox));
            f->prev_t = now; f->end = !word;
            break_up(f, o->palette);
        }
        f->word = word; f->word_t = now;
        word_new = true;
    }
    f->ncap = 0;
    if (has) {
        memcpy(f->cbox, b, sizeof(b));
        for (int q = 0; q < o->nshapes && f->ncap < FLUID_MAX_LETTERS; q++) if (o->shapes[q].tag == FLUID_TAG_CAPTION) f->cap[f->ncap++] = o->shapes[q];
        /* its backdrop: the whole reading's box when given (the dark holds still from word to word), else the word's */
        if (f->want_box[1] >= f->want_box[0]) {
            f->bbox[0] = fminf(f->want_box[0], b[0]); f->bbox[1] = fmaxf(f->want_box[1], b[1]);
            f->bbox[2] = fminf(f->want_box[2], b[2]); f->bbox[3] = fmaxf(f->want_box[3], b[3]);
        } else memcpy(f->bbox, b, sizeof(b));
    } else f->cbox[0] = 0.f, f->cbox[1] = -1.f;
    /* the backdrop: dark round the box, between dlo and dhi px of it. It comes as a front from the box outward over
     * FINE_DARK_IN_S as the reading begins, and after the last word it lifts the same way while the word breaks up:
     * a frame changes only the cells the front crosses */
    const float *box = NULL, reach = FINE_PATCH1 + FINE_PATCH_RAG * 0.5f;
    float dlo = -1.f, dhi = 1e9f;
    if (has) {
        box = f->bbox;
        if (!f->dark_on) { f->dark_on = true; f->dark_t0 = now; }
        const float u = (now - f->dark_t0) / FINE_DARK_IN_S;
        if (u < 1.f) dhi = u * (reach + 1.f);
    } else if (f->end && f->bbox[1] >= f->bbox[0]) {
        const float u = (now - f->prev_t) / FINE_END_S;
        if (u >= 0.f && u < 1.f) { box = f->bbox; dlo = u * (reach + 1.f); }
        else f->end = false;
    }
    if (!box) f->dark_on = false;
    const uint32_t gen = box ? mix((uint32_t)lrintf(box[0]) | (uint32_t)lrintf(box[1]) << 16, (uint32_t)lrintf(box[2]) | (uint32_t)lrintf(box[3]) << 16) | 2u : 1u;
    const uint32_t mgen = mix(gen, (uint32_t)lrintf(dlo * 4.f) ^ (uint32_t)lrintf(fminf(dhi, 4096.f) * 4.f) << 14);   /* (a cell the front crosses) */
    f->gen = gen;
    {   /* the drawing under it darker too (between dlo and dhi): its cells go again when that changes */
        B->dim[4] = box ? FINE_DIM : 0.f; B->dim[5] = dlo; B->dim[6] = dhi;
        if (box) {
            const float R = FLUID_DIM_REACH;
            memcpy(B->dim, box, sizeof(float) * 4);
            const int i0 = f->cellx[(int)fmaxf(0.f, fminf(S - 1.f, box[0] - R))], i1 = f->cellx[(int)fmaxf(0.f, fminf(S - 1.f, box[1] + R))];
            const int j0 = f->celly[(int)fmaxf(0.f, fminf(S - 1.f, box[2] - R))], j1 = f->celly[(int)fmaxf(0.f, fminf(S - 1.f, box[3] + R))];
            for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) {
                float lo2, hi2;
                cell_box_dist(f, i, j, box, &lo2, &hi2);
                if (under(hi2, dlo) || !under(lo2, dhi)) continue;   /* none of it dark */
                B->sig[j * W + i] += mix(0xD1Bu, !under(lo2, dlo) && under(hi2, dhi) ? gen : mgen);
                B->why[j * W + i] |= 1u << FLUID_ANIM_WORD;
            }
        }
    }
    if (has) {   /* the word forming: its cells go again at each step of it, at once */
        const uint32_t step = (uint32_t)lrintf(fminf(1.f, fmaxf(0.f, o->cap_unformed)) * 64.f) + 1u;
        const bool changed = step != f->step_key || word_new;
        const int i0 = f->cellx[(int)fmaxf(0.f, fminf(S - 1.f, f->cbox[0]))], i1 = f->cellx[(int)fmaxf(0.f, fminf(S - 1.f, f->cbox[1]))];
        const int j0 = f->celly[(int)fmaxf(0.f, fminf(S - 1.f, f->cbox[2]))], j1 = f->celly[(int)fmaxf(0.f, fminf(S - 1.f, f->cbox[3]))];
        for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) { B->sig[j * W + i] += mix(step, 0x51u); B->why[j * W + i] |= 1u << FLUID_ANIM_WORD; if (changed) B->imm[j * W + i] = 1; }
        f->step_key = step;
    } else f->step_key = 0;

    /* each cell's shift, stage px: the water's (its slope as the dots take it, DOT_REFR, times FINE_RIDE) in whole px, at most a sub-dot
     * across and two down, everywhere, plus the calm drift's pixel either way. Its dust rides inside the cell (what leaves
     * one side comes in at the other: fluid_fine_rect), so a shift costs no speck any work and changes only its own cell.
     * Calm water adds nothing: the dust is where the drift alone puts it. While the picture changes it holds still (as
     * the light: the change is the drawer's, the water under it is not its business), then takes the water again. */
    const float subx = (float)S / (2 * W), suby = (float)S / (4 * H);
    const float kx = FINE_RIDE * DOT_REFR / FQ_H * 2.f * subx, ky = FINE_RIDE * DOT_REFR / FQ_H * 4.f * H / W * suby, mx = (int)(subx + 0.5f), my = (int)(2.f * suby + 0.5f);
    const float ph = f->light_t * (1.f / FINE_DRIFT_S), A = FINE_DRIFT_PX * 0.5f;
    const int64_t t_dr = now_us();
    const fine_set_t *ds = fluid_fine_specks_kind(s_dust_kind) ? f->drawn : NULL;
    for (int j = 1; !(f->pause && f->drawn) && j < H - 1; j++) {
        const float v = (f->cy[j] + f->cy[j + 1]) * 0.5f / S, ra = DRIFT_AV * v - ph, rb = DRIFT_BV * v + DRIFT_B_T * ph;   /* (turns) */
        const float sa = sin_t(ra), ca = sin_t(ra + 0.25f), sb = sin_t(rb), cb = sin_t(rb + 0.25f);
        for (int i = 1; i < W - 1; i++) {
            const int k = j * W + i;
            if (ds && ds->start[k] == ds->start[k + 1]) continue;   /* (no speck to move) */
            const float *c = f->dcol + 4 * i;
            const float dx = A * (c[0] * ca + c[1] * sa + c[2] * cb + c[3] * sb), dy = A * (c[1] * ca - c[0] * sa - c[3] * cb + c[2] * sb);
            float wx = (float)(s->h[k - 1] - s->h[k + 1]) * kx, wy = (float)(s->h[k - W] - s->h[k + W]) * ky;
            wx = wx < -mx ? -mx : wx > mx ? mx : wx; wy = wy < -my ? -my : wy > my ? my : wy;
            /* the water's front only: a cell rides from the frame the water would first move its dust, FINE_RIDE_FRAMES,
             * then rests (back in place, the ripples behind the front leave it) till the water there is calm again */
            uint8_t *rd = f->ride + k;
            const bool calm = wx < 0.5f && wx > -0.5f && wy < 0.5f && wy > -0.5f;
            if (!*rd && (wx > SHIFT_HOLD || wx < -SHIFT_HOLD || wy > SHIFT_HOLD || wy < -SHIFT_HOLD)) *rd = 1;
            if (*rd > FINE_RIDE_FRAMES) {
                wx = wy = 0.f;
                *rd = !calm ? FINE_RIDE_FRAMES + 1 : *rd + 1 >= FINE_RIDE_FRAMES + FINE_RIDE_REST ? 0 : *rd + 1;
            } else if (*rd) (*rd)++;
            const int8_t qx = f->wq[k], qy = f->wq[N + k];
            const int8_t sx = (int8_t)(held(f->dq + k, dx) + held(f->wq + k, wx)), sy = (int8_t)(held(f->dq + N + k, dy) + held(f->wq + N + k, wy));
            if (qx != f->wq[k] || qy != f->wq[N + k]) B->why[k] |= 1u << FLUID_ANIM_WATER;   /* (the water's: the drift is no animation) */
            B->sx[k] = sx; B->sy[k] = sy;
        }
    }

    f->prof_drift += (uint32_t)(now_us() - t_dr);
    /* the specks. A cell's own are worked out on its row's turn (a quarter of the rows a frame): those the light never
     * hides only when the backdrop by them changed, the others against the light. Until then the cell shows what it
     * showed. A cell never worked out for this set (a set just swapped in) is, off its turn too, while FINE_CATCHUP lasts. */
    /* the light: for the specks, or for the creature's kind of dots (the drawer reads it: fluid_fine_light) */
    if ((s_dust_kind != FINE_DUST_SPECKS || f->drawn) && (!f->pause || !f->lgrid_ok)) {
        f->lrow = (uint8_t)((f->lrow + 1) % FINE_PHASES);
        light_grid(f, f->lgrid, f->light_t * FINE_LIGHT_SPEED, f->lgrid_ok ? f->lrow : -1);   /* a quarter of its rows a frame, in turn */
        f->lgrid_ok = true;
    }
    const fine_set_t *st = fluid_fine_specks_kind(s_dust_kind) && lv_ok ? f->drawn : NULL;   /* (EXPERIMENT: dots instead, the drawer's) */
    const int64_t t_sk = now_us();
    if (st) {
        const int LG = f->lg;
        uint8_t *const L = B->lv;
        const float reach = FINE_PATCH1 + FINE_PATCH_RAG * 0.5f;
        int budget = FINE_CATCHUP, worked = 0;
        for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) {
            const int k = j * W + i;
            const uint32_t a = st->start[k], e = st->start[k + 1], sm = a + st->nstat[k];
            if (a == e) continue;
            if (o->kdots && o->kdots[k]) continue;              /* a ball's or an image's own dots */
            /* by the backdrop: dark, the front crossing it (its specks worked out each frame meanwhile), or none */
            bool near_box = false;
            uint32_t key = 1u;
            if (box) {
                float lo2, hi2;
                cell_box_dist(f, i, j, box, &lo2, &hi2);
                near_box = under(lo2, reach + 1.f) && !under(hi2, dlo) && under(lo2, dhi);
                if (near_box) key = !under(lo2, dlo) && under(hi2, dhi) ? gen : mgen;
            }
            /* the light's changes on the cell's turn (a quarter of the rows a frame: it drifts ~0.3 px a frame, a turn
             * is never a pixel late); the backdrop's at once */
            bool turn = (!f->pause && j % FINE_PHASES == f->lrow) || (f->eff[k] && f->eff[k] != key);
            if (!f->eff[k]) {                                   /* never worked out: now, while the budget lasts */
                if (budget <= 0) continue;
                budget -= (int)(e - a);
                turn = true;
            }
            if (near_box) B->why[k] |= 1u << FLUID_ANIM_WORD;
            if (turn) {
                f->lvchg[k] |= 1;
                if (f->eff[k] != key) {                         /* those the light never hides */
                    uint32_t acc = 0;
                    if (!near_box) { memset(L + a, 255, sm - a); acc = st->ssig[k]; }
                    else for (uint32_t n = a; n < sm; n++) {
                        const uint32_t p = st->pos[n];
                        const uint8_t lv = backdrop_lv(box, dlo, dhi, (int)(p & 511), (int)(p >> 9 & 511));
                        L[n] = lv;
                        if (lv) acc += mix(p & 0x3FFFF, (uint32_t)lv << 16 | st->col[n]);
                    }
                    f->stat[k] = acc; f->eff[k] = key;
                    worked += (int)(sm - a);
                }
                uint32_t acc = f->stat[k];
                for (uint32_t n = sm; n < e; n++) {             /* the others: against the light, bilinear on its grid;
                                                                 * near a line it matters less (the line stays sharp) */
                    const uint32_t p = st->pos[n];
                    const int x = (int)(p & 511), y = (int)(p >> 9 & 511), q = (int)(p >> 18 & 127);
                    const float gx = (x + 0.5f) * (1.f / FINE_LIGHT_GRID), gy = (y + 0.5f) * (1.f / FINE_LIGHT_GRID);
                    const int ix = (int)gx, iy = (int)gy;
                    const float ax = gx - ix, ay = gy - iy;
                    const uint8_t *g0 = f->lgrid + iy * LG + ix;
                    const float l = ((g0[0] * (1.f - ax) + g0[1] * ax) * (1.f - ay) + (g0[LG] * (1.f - ax) + g0[LG + 1] * ax) * ay) * (1.f / 255.f);
                    const float nr = (float)(p >> 25) * (1.f / 127.f);
                    uint8_t lv = (float)q < (FINE_LIGHT_FLOOR + (1.f - FINE_LIGHT_FLOOR) * (l + (1.f - l) * nr)) * 127.f ? 255 : 0;
                    if (lv && near_box) lv = backdrop_lv(box, dlo, dhi, x, y);
                    L[n] = lv;
                    if (lv) acc += mix(p & 0x3FFFF, (uint32_t)lv << 16 | st->col[n]);
                }
                f->tot[k] = acc;
                worked += (int)(e - sm);
            }
            B->vis[k] = 1;
            if (f->tot[k]) B->sig[k] += f->tot[k] + ((B->sx[k] | B->sy[k]) ? mix(0x5a17u, (uint32_t)(uint8_t)B->sx[k] << 8 | (uint8_t)B->sy[k]) : 0u);
        }
        f->prof_eval += (uint32_t)worked;
    }
    f->prof_specks += (uint32_t)(now_us() - t_sk);

    /* the word before, breaking up: its specks scatter from its centre, ride the water and fade into it */
    int live_p = 0;
    memcpy(B->pbb0, f->pbb, sizeof(f->pbb));              /* (the box drawn last) */
    B->pbb[0] = B->pbb[2] = 32767; B->pbb[1] = B->pbb[3] = -1;
    if (f->np) {
        const float D = f->end ? FINE_END_S : FINE_SWAP_S, spread = f->end ? FINE_END_SPREAD : FINE_SWAP_SPREAD, u = (now - f->prev_t) / D;
        if (u >= 0.f && u < 1.f) {
            const float fade = (1.f - u) * sqrtf(1.f - u), ease = 1.f - (1.f - u) * (1.f - u), mw = ease * 0.7f;
            for (int q = 0; q < f->np; q++) {
                B->ppx[q] = -1;
                const int x0 = f->px0[q], y0 = f->py0[q], k0 = f->celly[y0] * W + f->cellx[x0];
                const float ang = f->pang[q] * (1.f / 256.f), d = ease * spread * (0.4f + f->pg[q] * (1.f / 255.f));
                const float x = x0 + 0.5f + sin_t(ang + 0.25f) * d + B->sx[k0] * ease, y = y0 + 0.5f + sin_t(ang) * d * 0.7f + B->sy[k0] * ease;
                if (x < 1.f || y < 1.f || x >= S - 2 || y >= S - 2) continue;
                const float v = f->pa[q] * (1.f / 255.f) * fade;
                const uint16_t c = f->pc[q];
                const float r = (c >> 11) * (255.f / 31.f), g = (c >> 5 & 63) * (255.f / 63.f), bb = (c & 31) * (255.f / 31.f);
                const uint16_t col = pack565((r + (FINE_WATER_R - r) * mw) * v, (g + (FINE_WATER_G - g) * mw) * v, (bb + (FINE_WATER_B - bb) * mw) * v);
                if (!col) continue;
                const int X = (int)x, Y = (int)y;
                B->ppx[q] = (int16_t)X; B->ppy[q] = (int16_t)Y; B->ppc[q] = col;
                B->sig[f->celly[Y] * W + f->cellx[X]] += mix((uint32_t)(X | Y << 9 | 1u << 20), col);
                B->why[f->celly[Y] * W + f->cellx[X]] |= 1u << FLUID_ANIM_WORD;
                if (X < B->pbb[0]) B->pbb[0] = (int16_t)X;
                if (X > B->pbb[1]) B->pbb[1] = (int16_t)X;
                if (Y < B->pbb[2]) B->pbb[2] = (int16_t)Y;
                if (Y > B->pbb[3]) B->pbb[3] = (int16_t)Y;
                live_p = q + 1;
            }
        } else if (u >= 1.f) f->np = 0;
    }
    B->npd = live_p;
    f->bready = true;
    f->prof_frame += (uint32_t)(now_us() - t_in); f->prof_n++;
}

/* c (rgb565 little-endian) at lv / 255 onto a big-endian pixel: on black or a dim one (added), never on a lit one */
static inline void put(uint16_t *px, uint16_t c, int lv) {
    int r = c >> 11, g = c >> 5 & 63, b = c & 31;
    if (lv < 255) { r = (r * lv + 127) / 255; g = (g * lv + 127) / 255; b = (b * lv + 127) / 255; }
    const uint16_t le = (uint16_t)(*px >> 8 | *px << 8);
    const int r0 = le >> 11, g0 = le >> 5 & 63, b0 = le & 31;
    if (r0 > FINE_DRAWN >> 3 || g0 > FINE_DRAWN >> 2 || b0 > FINE_DRAWN >> 3) return;   /* the drawing's own */
    r += r0; g += g0; b += b0;
    const uint16_t o = (uint16_t)((r > 31 ? 31 : r) << 11 | (g > 63 ? 63 : g) << 5 | (b > 31 ? 31 : b));
    *px = (uint16_t)(o >> 8 | o << 8);
}

void fluid_fine_rect(const fluid_fine_t *f, const uint8_t *dirty, const fluid_rect_t *t, bool all) {
    const int64_t t_in = now_us();
    const int W = f->W, S = f->S;
    const int X0 = t->x0 - f->ox, X1 = t->x1 - f->ox, Y0 = t->y0 - f->oy, Y1 = t->y1 - f->oy;   /* the window, stage px */
    if (X1 <= 0 || Y1 <= 0 || X0 >= S || Y0 >= S) return;
    const fine_set_t *st = f->drawn;
    if (st) {
        const int i0 = f->cellx[X0 < 0 ? 0 : X0], i1 = f->cellx[X1 > S ? S - 1 : X1 - 1], j0 = f->celly[Y0 < 0 ? 0 : Y0], j1 = f->celly[Y1 > S ? S - 1 : Y1 - 1];
        for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) {
            const int k = j * W + i;
            if (!f->vis[k] || (!all && !dirty[k])) continue;
            /* its specks, shifted inside the cell: what leaves one side comes in at the other */
            const int dx = f->sx[k], dy = f->sy[k], xa = f->cx[i], xb = f->cx[i + 1], ya = f->cy[j], yb = f->cy[j + 1];
            fluid_fine_t *fm = (fluid_fine_t *)f;          /* (as drawn, for fluid_fine_move: the cell whole in t) */
            fm->dok[k] = f->lvd && xa >= X0 && xb <= X1 && ya >= Y0 && yb <= Y1;
            if (fm->dok[k]) { fm->sxd[k] = (int8_t)dx; fm->syd[k] = (int8_t)dy; memcpy(fm->lvd + st->start[k], f->lv + st->start[k], st->start[k + 1] - st->start[k]); }
            for (uint32_t n = st->start[k]; n < st->start[k + 1]; n++) {
                const int lv = f->lv[n];
                if (!lv) continue;
                const uint32_t p = st->pos[n];
                int X = (int)(p & 511) + dx, Y = (int)(p >> 9 & 511) + dy;
                if (X < xa) X += xb - xa; else if (X >= xb) X -= xb - xa;
                if (Y < ya) Y += yb - ya; else if (Y >= yb) Y -= yb - ya;
                if (X < X0 || X >= X1 || Y < Y0 || Y >= Y1) continue;
                put(t->px + (size_t)(Y - Y0) * t->stride + (X - X0), st->col[n], lv);
            }
        }
    }
    if (f->npd && f->pbb[1] >= X0 && f->pbb[0] < X1 && f->pbb[3] >= Y0 && f->pbb[2] < Y1)
        for (int q = 0; q < f->npd; q++) {
            const int X = f->ppx[q], Y = f->ppy[q];
            if (X < X0 || X >= X1 || Y < Y0 || Y >= Y1 || X < 0) continue;
            if (!all && !dirty[f->celly[Y] * W + f->cellx[X]]) continue;
            put(t->px + (size_t)(Y - Y0) * t->stride + (X - X0), f->ppc[q], 255);
        }
    ((fluid_fine_t *)f)->prof_rect += (uint32_t)(now_us() - t_in);   /* the drawer's own profile (fluid_fine_t.prof_*) */
}

bool fluid_fine_move(fluid_fine_t *f, int k, uint16_t *fb, const uint16_t *under, int stride) {
    const fine_set_t *st = f->drawn;
    if (!st || !f->dok[k] || !f->vis[k]) return false;
    const int i = k % f->W, j = k / f->W, xa = f->cx[i], xb = f->cx[i + 1], ya = f->cy[j], yb = f->cy[j + 1];
    for (int q = 0; q < 2; q++) {                       /* the word's specks breaking up, now or last frame: not alone */
        const int16_t *b = q ? f->pbb0 : f->pbb;
        if (b[1] >= xa && b[0] < xb && b[3] >= ya && b[2] < yb) return false;
    }
    const size_t o = (size_t)f->oy * stride + f->ox;
    const uint32_t a = st->start[k], e = st->start[k + 1];
    for (int pass = 0; pass < 2; pass++) {              /* as drawn taken off (to what is under them), then as now put on */
        const int dx = pass ? f->sx[k] : f->sxd[k], dy = pass ? f->sy[k] : f->syd[k];
        const uint8_t *lv = pass ? f->lv : f->lvd;
        for (uint32_t n = a; n < e; n++) {
            if (!lv[n]) continue;
            const uint32_t p = st->pos[n];
            int X = (int)(p & 511) + dx, Y = (int)(p >> 9 & 511) + dy;
            if (X < xa) X += xb - xa; else if (X >= xb) X -= xb - xa;
            if (Y < ya) Y += yb - ya; else if (Y >= yb) Y -= yb - ya;
            const size_t q = o + (size_t)Y * stride + X;
            if (pass) put(fb + q, st->col[n], lv[n]); else fb[q] = under ? under[q] : 0;
        }
    }
    memcpy(f->lvd + a, f->lv + a, e - a);
    f->sxd[k] = f->sx[k]; f->syd[k] = f->sy[k];
    return true;
}
void fluid_fine_drawn(fluid_fine_t *f) {
    const fine_set_t *st = f->drawn;
    if (!st || !f->lvd) return;
    memcpy(f->lvd, f->lv, (size_t)st->n);
    memcpy(f->sxd, f->sx, (size_t)f->N); memcpy(f->syd, f->sy, (size_t)f->N);
    memset(f->dok, 1, (size_t)f->N);
}
