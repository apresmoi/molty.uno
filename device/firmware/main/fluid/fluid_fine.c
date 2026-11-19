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

enum { PH_IDLE, PH_RASTER, PH_FWD, PH_BWD, PH_EXTRACT, PH_LEVELS };

/* tables in PSRAM, made once (internal RAM is the network's) */
static float *s_near, *s_tint, *s_sin;
static bool s_lut;

static inline float hash_u(int a, int b, uint32_t c) {     /* fluid_draw.c's, chart-engine.js hashU */
    uint32_t h = (uint32_t)a * 0x27d4eb2du ^ (uint32_t)b * 0x165667b1u ^ c * 0x9e3779b1u;
    h ^= h >> 15; h *= 0x85ebca6bu; h ^= h >> 13;
    return (float)(h & 0xFFFFu) * (1.f / 65536.f);
}
/* sin of t turns (a table, linear) */
static inline float sin_t(float t) {
    t -= floorf(t);
    const float q = t * 1024.f;
    const int i = (int)q;
    return s_sin[i] + (s_sin[i + 1] - s_sin[i]) * (q - (float)i);
}
static inline float sstep(float a, float b, float x) {
    float t = (x - a) / (b - a);
    t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
    return t * t * (3.f - 2.f * t);
}
static inline uint16_t pack565(float r, float g, float b) {
    const int R = r >= 255.f ? 255 : r <= 0.f ? 0 : (int)(r + 0.5f), G = g >= 255.f ? 255 : g <= 0.f ? 0 : (int)(g + 0.5f),
              B = b >= 255.f ? 255 : b <= 0.f ? 0 : (int)(b + 0.5f);
    return (uint16_t)((R & 0xF8) << 8 | (G & 0xFC) << 3 | B >> 3);
}
/* a big-endian rgb565 pixel's channels, 0..255 */
static inline void rgb_of(uint16_t be, int *r, int *g, int *b) {
    const uint16_t le = (uint16_t)(be >> 8 | be << 8);
    *r = (le >> 11) * 255 / 31; *g = (le >> 5 & 63) * 255 / 63; *b = (le & 31) * 255 / 31;
}
/* the stable identity of what a speck shows where: the same speck at the same place and level hashes the same in
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
    f->ride = FINE_CALLOC((size_t)f->N); f->vis = FINE_CALLOC((size_t)f->N); f->imm = FINE_CALLOC((size_t)f->N);
    f->tot = FINE_CALLOC(sizeof(uint32_t) * (size_t)f->N); f->stat = FINE_CALLOC(sizeof(uint32_t) * (size_t)f->N);
    f->eff = FINE_CALLOC(sizeof(uint32_t) * (size_t)f->N); f->tot2 = FINE_CALLOC(sizeof(uint32_t) * (size_t)f->N);
    f->fresh = FINE_CALLOC((size_t)f->N); f->lgrid2 = FINE_CALLOC((size_t)f->lg * (size_t)f->lg);
    if (!f->bpos || !f->bcol || !f->bcell || !f->ride || !f->vis || !f->imm || !f->tot || !f->stat || !f->eff || !f->tot2 || !f->fresh || !f->lgrid2) {
        fluid_fine_destroy(f); return NULL;
    }
    if (!light_tables(f->lg, S)) { fluid_fine_destroy(f); return NULL; }   /* (one stage size a boot) */
    f->live = f->ready = f->build = -1;
    f->cbox[1] = f->pbox[1] = f->want_box[1] = f->bbox[1] = -1.f;   /* none */
    f->pbb[1] = -1;
    f->reveal = 1.f;
    return f;
}

void fluid_fine_destroy(fluid_fine_t *f) {
    if (!f) return;
    for (int q = 0; q < FINE_SETS; q++) set_free(&f->set[q]);
    FINE_FREE(f->cx); FINE_FREE(f->cy); FINE_FREE(f->cellx); FINE_FREE(f->celly); FINE_FREE(f->shp); FINE_FREE(f->box);
    FINE_FREE(f->rowbuf); FINE_FREE(f->sx); FINE_FREE(f->sy); FINE_FREE(f->sig); FINE_FREE(f->sig0); FINE_FREE(f->lgrid);
    FINE_FREE(f->px0); FINE_FREE(f->pc); FINE_FREE(f->pa); FINE_FREE(f->bpos); FINE_FREE(f->bcol); FINE_FREE(f->bcell);
    FINE_FREE(f->ride); FINE_FREE(f->vis); FINE_FREE(f->imm); FINE_FREE(f->tot); FINE_FREE(f->stat); FINE_FREE(f->eff);
    FINE_FREE(f->dist); FINE_FREE(f->src); FINE_FREE(f->lit); FINE_FREE(f->lv); FINE_FREE(f->lv2);
    FINE_FREE(f->tot2); FINE_FREE(f->fresh); FINE_FREE(f->lgrid2);
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
    memset(f->vis, 0, (size_t)f->N);
    f->word = 0; f->np = f->npd = 0; f->ncap = 0; f->end = false;
    f->cbox[1] = f->pbox[1] = f->want_box[1] = f->bbox[1] = -1.f; f->cbox[0] = f->pbox[0] = f->want_box[0] = f->bbox[0] = 0.f;
    f->dim[4] = 0.f; f->pbb[1] = -1;
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
        if (!(f->bcell[q] & 1)) st->ssig[j * W + (f->bcell[q] >> 1)] += mix(f->bpos[q] & 0x3FFFF, 255u << 16);
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
        if (lv) acc += mix(p & 0x3FFFF, 255u << 16);
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

void fluid_fine_swap(fluid_fine_t *f, bool rise) {
    if (!f) return;
    /* the dust on the glass is the scene's own: a set made for another drawing goes at once */
    if (f->drawn && f->drawn->base != f->scene) { f->drawn = NULL; f->live = -1; }
    if (f->ready < 0 || !f->levels_ok) return;
    if (f->lv_base != f->drawn) { f->levels_ok = false; f->phase = PH_LEVELS; f->row = 0; return; }   /* (worked out against another) */
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
    if (rise && kept * 2 < f->N) f->reveal = 0.f;               /* a new drawing's dust on a scene that is up: it rises */
}

void fluid_fine_rim(fluid_fine_t *f, bool on) { if (f) f->rim = on; }

void fluid_fine_caption_box(fluid_fine_t *f, const float box[4]) {
    if (!f) return;
    if (box) memcpy(f->want_box, box, sizeof(f->want_box));
    else { f->want_box[0] = 0.f; f->want_box[1] = -1.f; }
}

float fluid_fine_turn(int i, int j, int H) {
    const float t = (float)((i * 3) % 40) * (1.f / 40.f), wave = 4.f * t * (1.f - t);   /* humps along the row, 0..1 */
    return 0.03f + 0.94f * ((float)j + 0.5f + 1.2f * wave) / ((float)H + 1.2f);
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
        for (int q = 0; q < 4; q++) { const float ph = tau * kv[q] * y * g + kt[q] * T; sv[q] = sinf(ph); cv[q] = cosf(ph); }
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
    const int S = f->S;
    int16_t bx[FLUID_MAX_LETTERS][4];
    for (int q = 0; q < f->ncap; q++) fluid_shape_box(&f->cap[q], 0, 0, bx[q]);
    const float cx = (f->pbox[0] + f->pbox[1]) * 0.5f, cy = (f->pbox[2] + f->pbox[3]) * 0.5f;
    int x0 = (int)floorf(f->pbox[0]) - 1, x1 = (int)ceilf(f->pbox[1]) + 1, y0 = (int)floorf(f->pbox[2]) - 1, y1 = (int)ceilf(f->pbox[3]) + 1;
    x0 = x0 < 0 ? 0 : x0; y0 = y0 < 0 ? 0 : y0; x1 = x1 > S ? S : x1; y1 = y1 > S ? S : y1;
    f->np = 0;
    for (int y = y0; y < y1; y++) {
        memset(f->rowbuf + x0, 0, sizeof(uint16_t) * (size_t)(x1 - x0));
        fluid_shapes_row(f->cap, f->ncap, (const int16_t (*)[4])bx, 0, 0, pal, NULL, 0.f, f->rowbuf, 0, y, x0, x1);
        for (int x = x0; x < x1 && f->np < FINE_PARTICLES; x++) {
            if (!f->rowbuf[x]) continue;
            int r, g, b;
            rgb_of(f->rowbuf[x], &r, &g, &b);
            const int m = r > g ? (r > b ? r : b) : (g > b ? g : b);
            const float a = m / 255.f, h = hash_u(x, y, 5);
            if (a < 0.3f || h >= FINE_GRAIN) continue;
            const float ang = fluid_atan2(y - cy, x - cx) + (hash_u(x, y, 6) - 0.5f) * 2.2f;
            const int p = f->np++;
            f->px0[p] = (int16_t)x; f->py0[p] = (int16_t)y;
            f->pc[p] = pack565(r * 255.f / m, g * 255.f / m, b * 255.f / m);
            f->pa[p] = (uint8_t)(a * 255.f + 0.5f); f->pg[p] = (uint8_t)(h * 255.f);
            f->pang[p] = (uint8_t)((int)floorf(ang * (256.f / 6.2831853f) + 0.5f) & 255);
        }
    }
}

void fluid_fine_frame(fluid_fine_t *f, const fluid_frame_t *s, const fluid_draw_opts_t *o, int phase, int m) {
    const int64_t t_in = now_us();
    const int W = f->W, H = f->H, S = f->S, N = f->N;
    memset(f->sig, 0, sizeof(uint32_t) * (size_t)N);         /* f->sig0: as each cell was last drawn (fluid_draw_cells) */
    memset(f->imm, 0, (size_t)N);
    memset(f->vis, 0, (size_t)N);
    const float now = o->time;
    float dt = now - f->last_t;
    f->last_t = now;
    dt = dt < 0.f ? 0.f : dt > 0.1f ? 0.1f : dt;
    if (m < 1) m = 1;

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
    /* the backdrop: dark round the box, or (in a few steps) fading round the last one while its word breaks up */
    const float *box = NULL;
    float dark = 0.f;
    if (has) { box = f->bbox; dark = 1.f; }
    else if (f->end && f->bbox[1] >= f->bbox[0]) {
        const float u = (now - f->prev_t) / FINE_END_S;
        if (u >= 0.f && u < 1.f) { box = f->bbox; dark = ceilf((1.f - u) * FINE_DARK_STEPS) / FINE_DARK_STEPS; }
        else f->end = false;
    }
    const uint32_t gen = box ? (mix((uint32_t)lrintf(box[0]) | (uint32_t)lrintf(box[1]) << 16, ((uint32_t)lrintf(box[2]) | (uint32_t)lrintf(box[3]) << 16) ^
                                    (uint32_t)lrintf(dark * FINE_DARK_STEPS) << 28) | 2u) : 1u;
    f->gen = gen;
    {   /* the drawing under it darker too: its cells go again when that changes (on their rows' turns) */
        const float dimv = box ? dark * FINE_DIM : 0.f;
        f->dim[4] = dimv;
        if (box) {
            const float R = FLUID_DIM_REACH;
            memcpy(f->dim, box, sizeof(float) * 4);
            const int i0 = f->cellx[(int)fmaxf(0.f, fminf(S - 1.f, box[0] - R))], i1 = f->cellx[(int)fmaxf(0.f, fminf(S - 1.f, box[1] + R))];
            const int j0 = f->celly[(int)fmaxf(0.f, fminf(S - 1.f, box[2] - R))], j1 = f->celly[(int)fmaxf(0.f, fminf(S - 1.f, box[3] + R))];
            const uint32_t dk = mix((uint32_t)lrintf(dimv * 64.f) + 1u, gen);
            for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) f->sig[j * W + i] += dk;
        }
    }
    if (has) {   /* the word forming: its cells go again at each step of it, at once */
        const uint32_t step = (uint32_t)lrintf(fminf(1.f, fmaxf(0.f, o->cap_unformed)) * 64.f) + 1u;
        const bool changed = step != f->step_key || word_new;
        const int i0 = f->cellx[(int)fmaxf(0.f, fminf(S - 1.f, f->cbox[0]))], i1 = f->cellx[(int)fmaxf(0.f, fminf(S - 1.f, f->cbox[1]))];
        const int j0 = f->celly[(int)fmaxf(0.f, fminf(S - 1.f, f->cbox[2]))], j1 = f->celly[(int)fmaxf(0.f, fminf(S - 1.f, f->cbox[3]))];
        for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) { f->sig[j * W + i] += mix(step, 0x51u); if (changed) f->imm[j * W + i] = 1; }
        f->step_key = step;
    } else f->step_key = 0;

    /* each cell's shift: the wave's, in whole sub-dots (as refracted dots) but at most one across and two down, in
     * stage px: its dust rides inside the cell (what leaves one side comes in at the other: fluid_fine_rect), so a
     * shift costs no speck any work. Only within FINE_RIDE px of a tapped spot's drawing or the caption (where the eye
     * is), and on the rows whose turn it is. */
    const float subx = (float)S / (2 * W), suby = (float)S / (4 * H), rk = DOT_REFR / FQ_H, cwf = (float)S / W, chf = (float)S / H;
    const bool capbox = box != NULL || (f->np && f->pbox[1] >= f->pbox[0] && now - f->prev_t < FINE_SWAP_S);
    const float *cb = box ? box : f->pbox;
    {   /* where it rides: worked out again only when the tapped drawing or the caption's box changes */
        uint32_t rkey = capbox ? mix((uint32_t)lrintf(cb[0]) | (uint32_t)lrintf(cb[1]) << 16, (uint32_t)lrintf(cb[2]) | (uint32_t)lrintf(cb[3]) << 16) : 7u;
        for (int k = 0; o->still && k < N; k++) if (o->still[k] == 2) rkey = rkey * 31u + (uint32_t)k;
        rkey = rkey * 31u + (uint32_t)f->rim;
        if (rkey != f->ride_key) {
            f->ride_key = rkey;
            memset(f->ride, 0, (size_t)N);
            for (int j = 0; f->rim && j < H; j++) for (int i = 0; i < W; i++) {
                const float ex = (i + 0.5f) * cwf - S * 0.5f, ey = (j + 0.5f) * chf - S * 0.5f, lim = FINE_RIM * S * 0.5f;
                if (ex * ex + ey * ey >= lim * lim) f->ride[j * W + i] = 1;
            }
            const int ri = (int)ceilf(FINE_RIDE / cwf), rj = (int)ceilf(FINE_RIDE / chf);
            for (int k = 0; o->still && k < N; k++) {
                if (o->still[k] != 2) continue;
                const int ci = k % W, cj = k / W;
                for (int j = cj - rj; j <= cj + rj; j++) for (int i = ci - ri; i <= ci + ri; i++) {
                    if (i < 0 || j < 0 || i >= W || j >= H) continue;
                    const float ex = (i - ci) * cwf, ey = (j - cj) * chf;
                    if (ex * ex + ey * ey <= FINE_RIDE * FINE_RIDE) f->ride[j * W + i] = 1;
                }
            }
            for (int j = 0; capbox && j < H; j++) for (int i = 0; i < W; i++) {
                const float px = (i + 0.5f) * cwf, py = (j + 0.5f) * chf;
                const float ex = px < cb[0] ? cb[0] - px : px > cb[1] ? px - cb[1] : 0.f, ey = py < cb[2] ? cb[2] - py : py > cb[3] ? py - cb[3] : 0.f;
                if (ex * ex + ey * ey <= FINE_RIDE * FINE_RIDE) f->ride[j * W + i] = 1;
            }
        }
    }
    for (int j = 1; j < H - 1; j++) {
        if ((j + phase) % m) continue;
        for (int i = 1; i < W - 1; i++) {
            const int k = j * W + i;
            f->sx[k] = f->sy[k] = 0;
            if (!f->ride[k]) continue;
            const float fx = (float)(s->h[k + 1] - s->h[k - 1]) * rk * 2.f, fy = (float)(s->h[k + W] - s->h[k - W]) * rk * 4.f * H / W;
            int ox = (int)lrintf(fx), oy = (int)lrintf(fy);
            ox = ox < -1 ? -1 : ox > 1 ? 1 : ox; oy = oy < -2 ? -2 : oy > 2 ? 2 : oy;
            f->sx[k] = (int8_t)-lrintf(ox * subx); f->sy[k] = (int8_t)-lrintf(oy * suby);
        }
    }

    /* the specks. A cell's own are worked out on its row's turn (a quarter of the rows a frame): those the light never
     * hides only when the backdrop by them changed, the others against the light. Until then the cell shows what it
     * showed. A cell never worked out for this set (a set just swapped in) is, off its turn too, while FINE_CATCHUP lasts. */
    const fine_set_t *st = f->drawn;
    if (st) {
        const int LG = f->lg;
        const float T = now * FINE_LIGHT_SPEED;
        if (!f->pause || !f->lgrid_ok) {
            f->lrow = (uint8_t)((f->lrow + 1) % FINE_PHASES);
            light_grid(f, f->lgrid, T, f->lgrid_ok ? f->lrow : -1);   /* a quarter of its rows a frame, in turn */
            f->lgrid_ok = true;
        }
        if (f->reveal < 1.f) f->reveal = fminf(1.f, f->reveal + dt / FINE_REVEAL_SET_S);
        const float reach = FINE_PATCH1 + FINE_PATCH_RAG * 0.5f;
        int budget = FINE_CATCHUP, worked = 0;
        for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) {
            const int k = j * W + i;
            const uint32_t a = st->start[k], e = st->start[k + 1], sm = a + st->nstat[k];
            if (a == e) continue;
            if ((o->kdots && o->kdots[k]) ||                    /* a ball's or an image's own dots */
                (f->reveal < 1.f && 1.f - f->reveal >= fluid_fine_turn(i, j, H))) continue;   /* a first set: as it rises */
            bool near_box = false;
            if (box) {
                const float x0 = f->cx[i], x1 = f->cx[i + 1], y0 = f->cy[j], y1 = f->cy[j + 1];
                const float ex = x1 < box[0] ? box[0] - x1 : x0 > box[1] ? x0 - box[1] : 0.f, ey = y1 < box[2] ? box[2] - y1 : y0 > box[3] ? y0 - box[3] : 0.f;
                near_box = ex < reach + 1.f && ey < reach + 1.f;
            }
            const uint32_t key = near_box ? gen : 1u;
            bool turn = !f->pause && j % FINE_PHASES == f->lrow;
            if (!turn && !f->eff[k]) {                          /* never worked out: now, while the budget lasts */
                if (budget <= 0) continue;
                budget -= (int)(e - a);
                turn = true;
            }
            if (turn) {
                if (f->eff[k] != key) {                         /* those the light never hides */
                    uint32_t acc = 0;
                    if (!near_box) { memset(f->lv + a, 255, sm - a); acc = st->ssig[k]; }
                    else for (uint32_t n = a; n < sm; n++) {
                        const uint32_t p = st->pos[n];
                        const int x = (int)(p & 511), y = (int)(p >> 9 & 511);
                        const float px = x + 0.5f, py = y + 0.5f;
                        const float ex = px < box[0] ? box[0] - px : px > box[1] ? px - box[1] : 0.f, ey = py < box[2] ? box[2] - py : py > box[3] ? py - box[3] : 0.f;
                        const float keep = 1.f - dark * (1.f - sstep(FINE_PATCH0, FINE_PATCH1, sqrtf(ex * ex + ey * ey) + (hash_u(x, y, 5) - 0.5f) * FINE_PATCH_RAG));
                        const uint8_t lv = keep <= 0.02f ? 0 : (uint8_t)(keep * 255.f + 0.5f);
                        f->lv[n] = lv;
                        if (lv) acc += mix(p & 0x3FFFF, (uint32_t)lv << 16);
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
                    if (lv && near_box) {
                        const float px = x + 0.5f, py = y + 0.5f;
                        const float ex = px < box[0] ? box[0] - px : px > box[1] ? px - box[1] : 0.f, ey = py < box[2] ? box[2] - py : py > box[3] ? py - box[3] : 0.f;
                        const float keep = 1.f - dark * (1.f - sstep(FINE_PATCH0, FINE_PATCH1, sqrtf(ex * ex + ey * ey) + (hash_u(x, y, 5) - 0.5f) * FINE_PATCH_RAG));
                        lv = keep <= 0.02f ? 0 : (uint8_t)(keep * 255.f + 0.5f);
                    }
                    f->lv[n] = lv;
                    if (lv) acc += mix(p & 0x3FFFF, (uint32_t)lv << 16);
                }
                f->tot[k] = acc;
                worked += (int)(e - sm);
            }
            f->vis[k] = 1;
            if (f->tot[k]) f->sig[k] += f->tot[k] + ((f->sx[k] | f->sy[k]) ? mix(0x5a17u, (uint32_t)(uint8_t)f->sx[k] << 8 | (uint8_t)f->sy[k]) : 0u);
        }
        f->prof_eval += (uint32_t)worked;
    }

    /* the word before, breaking up: its specks scatter from its centre, ride the water and fade into it */
    int live_p = 0;
    f->pbb[0] = f->pbb[2] = 32767; f->pbb[1] = f->pbb[3] = -1;
    if (f->np) {
        const float D = f->end ? FINE_END_S : FINE_SWAP_S, spread = f->end ? FINE_END_SPREAD : FINE_SWAP_SPREAD, u = (now - f->prev_t) / D;
        if (u >= 0.f && u < 1.f) {
            const float fade = (1.f - u) * sqrtf(1.f - u), ease = 1.f - (1.f - u) * (1.f - u), mw = ease * 0.7f;
            for (int q = 0; q < f->np; q++) {
                f->ppx[q] = -1;
                const int x0 = f->px0[q], y0 = f->py0[q], k0 = f->celly[y0] * W + f->cellx[x0];
                const float ang = f->pang[q] * (1.f / 256.f), d = ease * spread * (0.4f + f->pg[q] * (1.f / 255.f));
                const float x = x0 + 0.5f + sin_t(ang + 0.25f) * d + f->sx[k0] * ease, y = y0 + 0.5f + sin_t(ang) * d * 0.7f + f->sy[k0] * ease;
                if (x < 1.f || y < 1.f || x >= S - 2 || y >= S - 2) continue;
                const float v = f->pa[q] * (1.f / 255.f) * fade;
                const uint16_t c = f->pc[q];
                const float r = (c >> 11) * (255.f / 31.f), g = (c >> 5 & 63) * (255.f / 63.f), bb = (c & 31) * (255.f / 31.f);
                const uint16_t col = pack565((r + (FINE_WATER_R - r) * mw) * v, (g + (FINE_WATER_G - g) * mw) * v, (bb + (FINE_WATER_B - bb) * mw) * v);
                if (!col) continue;
                const int X = (int)x, Y = (int)y;
                f->ppx[q] = (int16_t)X; f->ppy[q] = (int16_t)Y; f->ppc[q] = col;
                f->sig[f->celly[Y] * W + f->cellx[X]] += mix((uint32_t)(X | Y << 9 | 1u << 20), col);
                if (X < f->pbb[0]) f->pbb[0] = (int16_t)X;
                if (X > f->pbb[1]) f->pbb[1] = (int16_t)X;
                if (Y < f->pbb[2]) f->pbb[2] = (int16_t)Y;
                if (Y > f->pbb[3]) f->pbb[3] = (int16_t)Y;
                live_p = q + 1;
            }
        } else if (u >= 1.f) f->np = 0;
    }
    f->npd = live_p;
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
