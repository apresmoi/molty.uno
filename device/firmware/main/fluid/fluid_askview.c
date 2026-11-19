/*
 * SPDX-License-Identifier: MIT
 *
 * fluid_askview.c — a question drawn over the screen (fluid_askview.h), as device/creatures/js/ask-options.js draw():
 * the same lattice of dots (2 x 4 a cell), the same forming from the real crest, the same dust carried by the water.
 * What the page does per dot each frame this does over precomputed lists: each line's dots (only those that can ever
 * show), each letter's pixels. The twinkles move a cell row at a time, a few times a second (the panel's cost is by
 * the cell that changes); the letters, once formed, are the same masks frame after frame.
 */
#include "fluid_askview.h"
#include "glyph_font.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define AV_CALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define AV_FREE(p)   heap_caps_free(p)
#else
#define AV_CALLOC(n) calloc(1, (n))
#define AV_FREE(p)   free(p)
#endif

/* ask-options.js */
#define TOUCH_S   1.6f       /* the start: a finger's touch at the centre                        */
#define TOUCH_R   0.09f
#define REACH     0.012f     /* the crest has reached a cell where the water is up this much (h)   */
#define LINE_W    0.011f     /* a line's half width, of the radius                                 */
#define DUST_D    0.085f     /* its dust, inward of it                                             */
#define LET_R     0.86f      /* the rim lets go here (decline, timeout): a ring-shaped wave         */
#define LET_S     0.3f
#define LET_W     0.05f
#define TWINKLE_HZ 5.f       /* a cell row's twinkle moves this often                              */
#define MASK_BYTES ((FLUID_ASK_STR * 6 * 7 + 7) / 8)
/* the scene dims under the question (to DIM of its brightness, its world's dust thinned to KEEP) where the crest has
 * passed, and comes back a cell row at a time as the question leaves, in DIM_STEPS steps: a cell's colour changes a
 * few times, never every frame (each change sends it to the panel again) */
#define DIM       0.28f
#define KEEP      0.45f
#define DIM_STEPS 3
static const uint32_t ICE = 0x96A0BE;

typedef struct { uint16_t q, k; int8_t d; uint8_t b0, thr, h1, h2, h3, tg, row; } av_dot_t;   /* d x 1000; b0: line fade or dust u */
typedef struct { float x, y; uint16_t k, bit; uint8_t h1, h2, h3, it; } av_px_t;

struct fluid_askview {
    int W, H, N, stage, nd, np;
    int by, cap;                                    /* the dots' build: the next row of them, room for this many */
    float cw, ch, sc;
    fluid_ask_layout_t L;
    av_dot_t *dot;
    av_px_t *px;
    float *reach;                                   /* per cell: when the crest first lifted it (-1: not yet) */
    uint8_t *hb;                                    /* per dot: its fixed chance of staying in the dimmed scene */
    uint8_t *qd, *ql;                               /* per cell: the question's dots, their level (eighths)   */
    uint8_t *cp;                                    /* per cell: how far the scene has dimmed under it, 0..DIM_STEPS */
    uint32_t *qc;                                   /* ... and colour                                         */
    int mode, pick; float T;
    bool settled;                                   /* every letter formed: the masks are whole              */
    uint32_t fno; int im;                           /* frames drawn; how many the cell rows take turns over  */
    uint8_t rowon[64];                              /* this frame's turn: the rows its dots are made again in */
    struct { float x, y, w, h; int mw; bool on; } box[FLUID_ASK_ITEMS];
    uint8_t mask[FLUID_ASK_ITEMS][MASK_BYTES], dmask[FLUID_ASK_ITEMS][MASK_BYTES];
};
enum { M_OFF, M_IN, M_ON, M_PICK, M_OUT };

static inline float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static inline float smooth(float a, float b, float x) { const float t = clampf((x - a) / (b - a), 0.f, 1.f); return t * t * (3.f - 2.f * t); }
static uint32_t hash2(uint32_t a, uint32_t b) { uint32_t h = a * 374761393u + b * 668265263u; h = (h ^ (h >> 13)) * 1274126177u; return h ^ (h >> 16); }
static inline uint8_t hb8(uint32_t a, uint32_t b) { return (uint8_t)(hash2(a, b) >> 24); }

/* the lattice: dot (sx, sy)'s centre in stage px, and back */
static inline float slot_x(const fluid_askview_t *v, int sx) { return ((float)(sx >> 1) + ((sx & 1) ? 0.7f : 0.3f)) * v->cw; }
static inline float slot_y(const fluid_askview_t *v, int sy) { return ((float)(sy >> 2) + 0.14f + 0.24f * (float)(sy & 3)) * v->ch; }

/* a target's signed distance at (X, Y) (0 on its line, > 0 toward its dust) and fade at its ends; false: not there */
static bool shape(const fluid_ask_target_t *t, float X, float Y, float *d, float *fade) {
    if (t->kind == FLUID_ASK_HLINE) {
        if (X < t->b || X > t->c) return false;
        *d = Y - t->a; *fade = smooth(0.f, 0.04f, fminf(X - t->b, t->c - X));
        return true;
    }
    const float r = sqrtf(X * X + Y * Y);
    *d = t->a - r; *fade = 1.f;
    if (t->kind == FLUID_ASK_RING || *d < -LINE_W || *d > LINE_W + DUST_D) return true;   /* off its band: no angle needed */
    float a = atan2f(X, -Y); if (a < 0.f) a += 6.2831853f;
    if (a < t->b || a > t->c) return false;
    *fade = smooth(0.f, 0.05f, fminf(a - t->b, t->c - a));
    return true;
}

/* whether target t's line or its dust can reach a dot r2 (squared) from the middle: its band and a hair more, with no
 * root (the dots in it are then measured as before: shape) */
static inline bool in_band(const fluid_ask_target_t *t, float r2) {
    if (t->kind == FLUID_ASK_HLINE) return true;
    const float lo = t->a - LINE_W - DUST_D - 1e-3f, hi = t->a + LINE_W * 0.6f + 1e-3f;
    return r2 <= hi * hi && (lo <= 0.f || r2 >= lo * lo);
}

bool fluid_askview_build(fluid_askview_t *v, const fluid_sim_t *s, int rows) {
    const int W = v->W, DW = 2 * W, DH = 4 * v->H;
    const fluid_ask_layout_t *L = &v->L;
    const float half = v->stage * 0.5f;
    for (const int end = v->by + rows < DH ? v->by + rows : DH; v->by < end; v->by++) {
        const int sy = v->by;
        for (int sx = 0; sx < DW && v->nd < v->cap; sx++) {
            const float X = slot_x(v, sx) / half - 1.f, Y = slot_y(v, sy) / half - 1.f, r2 = X * X + Y * Y;
            if (r2 > 0.988f * 0.988f) continue;
            const int k = (sy >> 2) * W + (sx >> 1);
            if (!s->mask[k]) continue;
            for (int t = 0; t < L->ntargets; t++) {
                float d, fade;
                if (!in_band(&L->tg[t], r2) || !shape(&L->tg[t], X, Y, &d, &fade) || fade <= 0.f || d < -LINE_W * 0.6f || d > LINE_W + DUST_D) continue;
                const uint8_t h1 = hb8((uint32_t)sx, (uint32_t)sy), h2 = hb8((uint32_t)sx + 911u, (uint32_t)sy - 377u), h3 = hb8((uint32_t)sx - 53u, (uint32_t)sy + 1201u);
                float b0 = fade, thr = 1.f;
                if (d > LINE_W) {                             /* dust: shows while its hash is under u^2.6 x 0.6 x fade (twinkling) */
                    const float u = 1.f - (d - LINE_W) / DUST_D;
                    thr = powf(u, 2.6f) * 0.6f * fade;
                    if (u <= 0.f || h1 / 256.f > thr) continue;
                    b0 = u;
                }
                v->dot[v->nd++] = (av_dot_t){ (uint16_t)(sy * DW + sx), (uint16_t)k, (int8_t)lrintf(d * 1000.f), (uint8_t)(b0 * 255.f + 0.5f),
                                              (uint8_t)fminf(255.f, thr * 256.f), h1, h2, h3, (uint8_t)t, (uint8_t)(sy >> 2) };
                break;                                        /* one target a dot */
            }
        }
    }
    return v->by >= DH;
}

fluid_askview_t *fluid_askview_create(const fluid_sim_t *s, const fluid_ask_q_t *q) {
    fluid_askview_t *v = fluid_askview_begin(s, q);
    if (v) fluid_askview_build(v, s, 4 * v->H);
    return v;
}

fluid_askview_t *fluid_askview_begin(const fluid_sim_t *s, const fluid_ask_q_t *q) {
    fluid_askview_t *v = AV_CALLOC(sizeof(fluid_askview_t));
    if (!v) return NULL;
    const int W = s->W, H = s->H, N = W * H, DW = 2 * W, DH = 4 * H;
    const fluid_ask_layout_t *L = &v->L;
    fluid_ask_lay(q, &v->L);
    v->W = W; v->H = H; v->N = N; v->stage = s->stage;
    v->cw = (float)s->stage / W; v->ch = (float)s->stage / H; v->sc = (float)s->stage / (float)FLUID_STAGE_REF;
    v->reach = AV_CALLOC(sizeof(float) * (size_t)N); v->hb = AV_CALLOC((size_t)N * 8);
    v->qd = AV_CALLOC((size_t)N); v->ql = AV_CALLOC((size_t)N); v->qc = AV_CALLOC(sizeof(uint32_t) * (size_t)N); v->cp = AV_CALLOC((size_t)N);
    if (!v->reach || !v->hb || !v->qd || !v->ql || !v->qc || !v->cp) { fluid_askview_destroy(v); return NULL; }
    for (int k = 0; k < N * 8; k++) v->hb[k] = hb8((uint32_t)k, 77u);
    const float half = s->stage * 0.5f;
    /* a row's underline in the cell row under its label's: the drawer clears the cells a word covers, the page only its
     * dots (moved at most a cell row down) */
    for (int t = 0; t < L->ntargets; t++) {
        fluid_ask_target_t *g = &v->L.tg[t];
        for (int it = 0; g->kind == FLUID_ASK_HLINE && it < L->nitems; it++) {
            const fluid_ask_item_t *I = &L->item[it];
            if (I->role != FLUID_ASK_LABEL || I->opt != g->opt) continue;
            const float bottom = (I->y + 1.f) * half + 3.5f * I->px * v->sc, first = ceilf(bottom / v->ch - 1e-3f) * v->ch;
            const float y = (g->a + 1.f) * half;
            if (y < first + 0.3f * v->ch) g->a = (first + 0.3f * v->ch) / half - 1.f;
            g->cy = g->a;
        }
    }
    {   /* the targets' dots that can ever show: room for those in a target's band (few are not kept), worked out by
         * fluid_askview_build */
        int cap = 0;
        for (int sy = 0; sy < DH; sy++) for (int sx = 0; sx < DW; sx++) {
            const float X = slot_x(v, sx) / half - 1.f, Y = slot_y(v, sy) / half - 1.f, r2 = X * X + Y * Y;
            if (r2 > 0.988f * 0.988f || !s->mask[(sy >> 2) * W + (sx >> 1)]) continue;
            for (int t = 0; t < L->ntargets; t++) {
                const fluid_ask_target_t *g = &L->tg[t];
                if (g->kind == FLUID_ASK_HLINE ? X >= g->b && X <= g->c && Y - g->a >= -LINE_W * 0.6f && Y - g->a <= LINE_W + DUST_D : in_band(g, r2)) { cap++; break; }
            }
        }
        if (!(v->dot = AV_CALLOC(sizeof(av_dot_t) * (size_t)(cap + 1)))) { fluid_askview_destroy(v); return NULL; }
        v->cap = cap; v->nd = 0; v->by = 0;
    }
    for (int pass = 0; pass < 2; pass++) {        /* each letter's lit pixels */
        int np = 0;
        for (int it = 0; it < L->nitems; it++) {
            const fluid_ask_item_t *I = &L->item[it];
            const float p = I->px * v->sc, w = (float)(I->len * 6 - 1) * p;
            const float x0 = (I->x + 1.f) * half - w * 0.5f, y0 = (I->y + 1.f) * half - 3.5f * p;
            if (pass) { v->box[it].x = x0; v->box[it].y = y0; v->box[it].w = w; v->box[it].h = 7.f * p; v->box[it].mw = I->len * 6 - 1; }
            for (int n = 0; n < I->len; n++) {
                const uint8_t *g = gc_font5x7(I->str[n]);
                for (int r = 0; r < 7 && g; r++) for (int c = 0; c < 5; c++) {
                    if (!(g[r] & (16 >> c))) continue;
                    if (pass) {
                        const float x = x0 + ((float)(n * 6 + c) + 0.5f) * p, y = y0 + ((float)r + 0.5f) * p;
                        const int i = clampf(x / v->cw, 0.f, W - 1.f), j = clampf(y / v->ch, 0.f, H - 1.f);
                        v->px[np] = (av_px_t){ x, y, (uint16_t)(j * W + i), (uint16_t)(r * (I->len * 6 - 1) + n * 6 + c),
                                               hb8((uint32_t)(n * 7 + c), (uint32_t)(r * 13 + I->len + it * 101)), hb8((uint32_t)(r + 97), (uint32_t)(n * 5 + c + it * 31)),
                                               hb8((uint32_t)(n * 31 + c), (uint32_t)(r * 7 + it)), (uint8_t)it };
                    }
                    np++;
                }
            }
        }
        if (!pass && !(v->px = AV_CALLOC(sizeof(av_px_t) * (size_t)(np + 1)))) { fluid_askview_destroy(v); return NULL; }
        v->np = np;
    }
    for (int k = 0; k < N; k++) v->reach[k] = -1.f;
    v->pick = -1;
    return v;
}

void fluid_askview_destroy(fluid_askview_t *v) {
    if (!v) return;
    AV_FREE(v->dot); AV_FREE(v->px); AV_FREE(v->reach); AV_FREE(v->hb); AV_FREE(v->qd); AV_FREE(v->ql); AV_FREE(v->qc); AV_FREE(v->cp);
    AV_FREE(v);
}

void fluid_askview_start(fluid_askview_t *v, fluid_sim_t *s) {
    for (int k = 0; k < v->N; k++) v->reach[k] = -1.f;
    v->settled = false;
    fluid_sim_impulse(s, 0.f, 0.f, TOUCH_S, TOUCH_R);
}

void fluid_askview_answer(fluid_askview_t *v, fluid_sim_t *s, int pick) {
    v->pick = pick;
    for (int t = 0; pick >= 0 && t < v->L.ntargets; t++)
        if (v->L.tg[t].opt == pick) { fluid_sim_impulse(s, v->L.tg[t].cx, v->L.tg[t].cy, TOUCH_S * 1.2f, TOUCH_R); return; }
    for (int j = 0; j < s->H; j++) for (int i = 0; i < s->W; i++) {   /* the rim lets go: a ring-shaped wave */
        const int k = j * s->W + i;
        if (!s->mask[k]) continue;
        const float e = (sqrtf(s->xs[i] * s->xs[i] + s->ys[j] * s->ys[j]) - LET_R) / LET_W;
        if (e * e >= 9.f) continue;
        const float h = (float)s->h[k] + LET_S * expf(-2.f * e * e) * FQ_H;
        s->h[k] = (int16_t)clampf(h, -32768.f, 32767.f);
    }
}

/* a dot at lattice (sx, sy) at brightness b (> 1: toward white) in colour c; the brightest a cell's colour */
static inline void put(fluid_askview_t *v, const fluid_sim_t *s, int sx, int sy, float b, uint32_t c) {
    if (sx < 0 || sy < 0 || sx >= 2 * v->W || sy >= 4 * v->H || b <= 0.03f || !v->rowon[sy >> 2]) return;
    const int k = (sy >> 2) * v->W + (sx >> 1);
    if (!s->mask[k]) return;
    v->qd[k] |= (uint8_t)(1 << fluid_braille_bit(sx & 1, sy & 3));
    int e = (int)(b * 8.f + 0.5f); e = e < 1 ? 1 : e > 24 ? 24 : e;
    if (e > v->ql[k]) { v->ql[k] = (uint8_t)e; v->qc[k] = c; }
}

/* where the water has carried a grain that broke loose `age` s ago, stage px: its eddies, its slope's push, the grain's
 * own spread, a slight rise (ask-options.js carry) */
static void carry(const fluid_askview_t *v, const fluid_sim_t *s, int k, float age, uint8_t h1, uint8_t h2, float *ox, float *oy) {
    const int W = s->W, i = k % W, j = k / W;
    const float gx = i > 0 && i < W - 1 ? (s->h[k + 1] - s->h[k - 1]) / FQ_H : 0.f, gy = j > 0 && j < s->H - 1 ? (s->h[k + W] - s->h[k - W]) / FQ_H : 0.f;
    const float vx = s->u[k] / FQ_U * v->cw * 0.5f + (-gx * 60.f + (h1 / 256.f - 0.5f) * 34.f) * v->sc;
    const float vy = s->v[k] / FQ_U * v->ch * 0.3f + (-gy * 60.f + (h2 / 256.f - 0.5f) * 34.f - 6.f) * v->sc;
    *ox = vx * age; *oy = vy * age;
}
static inline void put_px(fluid_askview_t *v, const fluid_sim_t *s, float x, float y, float b, uint32_t c) {
    put(v, s, (int)lrintf(x / v->cw * 2.f - 0.6f), (int)lrintf(y / v->ch * 4.f - 0.56f), b, c);
}
/* how formed a thing at cell k is at T: from when the real crest first lifted it (a slow fallback never leaves a gap) */
static inline float formed(const fluid_askview_t *v, int k, float T, float delay, float span) {
    const float r = v->reach[k], a = r < 0.f ? 0.f : smooth(delay, delay + span, T - r);
    return fmaxf(a, smooth(2.6f + delay, 3.f + delay, T));
}
/* T on a coarser clock for things that change as they form (a line, a word): each row or word steps STEP_HZ times a
 * second at its own moment (phase 0..1), so a cell changes every few frames, not every one */
#define STEP_HZ 10.f
static inline float stepped(float T, float phase) { return floorf(T * STEP_HZ + phase) / STEP_HZ; }
static inline uint32_t toward_white(uint32_t c, float hot) {
    if (hot <= 0.f) return c;
    const float r = (c >> 16 & 255), g = (c >> 8 & 255), b = (c & 255);
    return (uint32_t)(r + (255.f - r) * hot) << 16 | (uint32_t)(g + (255.f - g) * hot) << 8 | (uint32_t)(b + (255.f - b) * hot);
}

void fluid_askview_frame(fluid_askview_t *v, const fluid_sim_t *s, const fluid_ask_screen_t *scr, float t) {
    const int N = v->N, DW = 2 * v->W;
    v->mode = scr->phase == FLUID_ASK_SHOW ? (scr->t < FLUID_ASK_FORM_S ? M_IN : M_ON) : scr->phase == FLUID_ASK_PICK ? M_PICK : scr->phase == FLUID_ASK_OUT ? M_OUT : M_OFF;
    v->T = v->mode == M_IN || v->mode == M_ON ? scr->t : scr->out_t;
    if (v->mode == M_OUT) v->pick = -1;
    const bool forming = v->mode == M_IN || v->mode == M_ON, leaving = v->mode == M_PICK || v->mode == M_OUT;
    const float T = v->T;
    /* the cell rows take turns (as fluid_ilace_row: the drawer sends the water's own changes in the same rows): the
     * question's dots in this frame's rows are made again, the others kept from their last turn */
    v->im = v->mode == M_ON ? 2 : 3;
    v->fno++;
    for (int j = 0; j < v->H; j++) {
        v->rowon[j] = v->mode == M_OFF || T < 0.05f || j % v->im == (int)((v->fno & 0x0FFFFFFFu) % (uint32_t)v->im);
        if (v->rowon[j]) { memset(v->qd + j * v->W, 0, (size_t)v->W); memset(v->ql + j * v->W, 0, (size_t)v->W); }
    }
    if (v->mode == M_OFF) { memset(v->cp, 0, (size_t)N); for (int it = 0; it < v->L.nitems; it++) v->box[it].on = false; return; }
    if (forming) for (int k = 0; k < N; k++) if (v->reach[k] < 0.f && s->h[k] > (int16_t)(REACH * FQ_H)) v->reach[k] = T;
    for (int k = 0; k < N; k++) {                     /* the scene under it: dimmed where the crest passed, back row by row */
        const int j = k / v->W;
        const float p = forming ? formed(v, k, T, 0.f, 0.35f) : leaving ? 1.f - smooth(0.1f, 0.9f, T - 0.25f * (float)(j * 7 % v->H) / (float)v->H) : 0.f;
        v->cp[k] = (uint8_t)(p * DIM_STEPS + 0.5f);
    }

    /* the crest while it forms the question: pale dust where the real wave is up (the ring's crest, in the asking colour) */
    if (v->mode == M_IN && T < 2.2f) {
        const float fade = 1.f - smooth(1.4f, 2.2f, T);
        for (int k = 0; k < N; k++) {
            if (!s->mask[k] || s->h[k] <= (int16_t)(0.015f * FQ_H)) continue;
            const int i = k % v->W, j = k / v->W;
            if (s->xs[i] * s->xs[i] + s->ys[j] * s->ys[j] > 0.95f) continue;
            const float lift = smooth(0.015f, 0.07f, s->h[k] / FQ_H) * fade, chance = lift * 0.4f * 256.f;
            for (int q = 0; q < 8; q++) {
                const int sx = 2 * i + (q >> 2), sy = 4 * j + (q & 3);
                if (chance > hb8((uint32_t)sx - 53u, (uint32_t)sy + 1201u)) put(v, s, sx, sy, 0.4f + 0.5f * lift, ICE);
            }
        }
    }
    /* the lines, then their dust inward, each as the crest reaches it; leaving, they break into dust in place and the
     * water carries it off (nothing moves as a shape); the chosen one lingers a moment, brighter */
    const fluid_ask_target_t *chosen = NULL;
    for (int q = 0; v->mode == M_PICK && q < v->L.ntargets; q++) if (v->L.tg[q].opt == v->pick) chosen = &v->L.tg[q];
    for (int n = 0; n < v->nd; n++) {
        const av_dot_t *D = &v->dot[n];
        const float d = D->d * 0.001f, tr = floorf(t * TWINKLE_HZ + (float)(D->row * 7 % 11) / 11.f) / TWINKLE_HZ;   /* its row's twinkle */
        const float h2 = D->h2 / 256.f, h3 = D->h3 / 256.f;
        float base;
        if (d <= LINE_W) base = (0.9f + 0.1f * fluid_sin_turns(tr * (1.3f / 6.2831853f) + h2)) * (D->b0 / 255.f);
        else {
            if (D->h1 > D->thr * (0.82f + 0.18f * fluid_sin_turns(tr * (0.7f / 6.2831853f) + h3))) continue;
            const float u = D->b0 / 255.f;
            base = (0.28f + 0.52f * u) * (0.62f + 0.38f * fluid_sin_turns(tr * (1.6f / 6.2831853f) + h2));
        }
        const fluid_ask_target_t *tg = &v->L.tg[D->tg];
        const int sx = D->q % DW, sy = D->q / DW;
        if (forming) {
            const float o = d <= LINE_W ? 0.f : 0.1f + 1.6f * d, f = formed(v, D->k, stepped(T, (float)(D->row * 5 % 7) / 7.f), o * 0.4f, 0.3f);
            if (f <= 0.f) continue;
            put(v, s, sx, sy, base * f + 3.f * fmaxf(0.f, s->h[D->k] / FQ_H), tg->rgb);
        } else {
            const bool ch = tg == chosen;
            const float age = T - D->h1 / 256.f * 0.3f - (ch ? 0.18f : 0.f), f = 1.f - smooth(0.f, 0.55f, age);
            if (f <= 0.f || h3 > 0.35f + 0.65f * f) continue;   /* breaking up: fewer dots as it goes */
            const float b = base * (0.35f + 0.65f * f) * (ch ? 1.f + 0.5f * f : 1.f);
            if (age <= 0.f) { put(v, s, sx, sy, b, tg->rgb); continue; }
            float ox, oy;
            carry(v, s, D->k, age, D->h1, D->h2, &ox, &oy);
            put_px(v, s, slot_x(v, sx) + ox, slot_y(v, sy) + oy, b, tg->rgb);
        }
    }
    /* the letters: crisp pixels once formed (the masks); before, dust condensing toward them; leaving, each breaks loose
     * and the water carries it off as it fades */
    if (forming && v->settled) return;              /* whole, as they were */
    memset(v->mask, 0, sizeof(v->mask));
    bool all = forming;
    for (int n = 0; n < v->np; n++) {
        const av_px_t *P = &v->px[n];
        const fluid_ask_item_t *I = &v->L.item[P->it];
        const bool lbl = I->role == FLUID_ASK_LABEL, picked = v->mode == M_PICK && I->opt == v->pick && v->pick >= 0;
        float f;
        if (forming) {
            f = formed(v, P->k, stepped(T, (float)(P->it % 3) / 3.f), lbl ? 0.15f : 0.05f, 0.45f);
            if (f >= 0.999f) { v->mask[P->it][P->bit >> 3] |= (uint8_t)(0x80 >> (P->bit & 7)); continue; }
            all = false;
            if (f <= 0.02f) continue;
            const float sc = (1.f - f) * 14.f * v->sc;
            put_px(v, s, P->x + (P->h1 / 256.f - 0.5f) * sc * 2.f, P->y + (P->h2 / 256.f - 0.5f) * sc * 2.f, 0.35f + 0.65f * f, I->rgb);
            continue;
        }
        const float age = T - P->h1 / 256.f * 0.3f - (picked ? 0.25f : 0.f);
        f = 1.f - smooth(0.f, 0.6f, age);
        if (f <= 0.02f || P->h3 / 256.f > 0.35f + 0.65f * f) continue;
        if (age <= 0.f) { v->mask[P->it][P->bit >> 3] |= (uint8_t)(0x80 >> (P->bit & 7)); continue; }
        float ox, oy;
        carry(v, s, P->k, age, P->h1, P->h2, &ox, &oy);
        put_px(v, s, P->x + ox, P->y + oy, 0.35f + 0.65f * f, toward_white(I->rgb, picked ? 0.4f * (1.f - smooth(0.f, 0.4f, T)) : 0.f));
    }
    if (all && v->mode == M_ON) v->settled = true;
    for (int it = 0; it < v->L.nitems; it++) {
        bool on = false;
        for (int b = 0; b < (v->box[it].mw * 7 + 7) / 8 && !on; b++) on = v->mask[it][b] != 0;
        v->box[it].on = on;
    }
}

uint32_t fluid_askview_draw_frame(const fluid_askview_t *v) { return FLUID_FRAME(v->fno, v->im < 2 ? 2 : v->im); }
const uint8_t *fluid_askview_dots(const fluid_askview_t *v) { return v->qd; }
const fluid_ask_layout_t *fluid_askview_layout(const fluid_askview_t *v) { return &v->L; }

void fluid_askview_opts(const fluid_askview_t *v, fluid_draw_opts_t *o, const uint8_t *dots, const fluid_letter_t *letters, int nletters) {
    o->dots = dots; o->scene = true; o->dust = 0.f; o->melt = 0.f;
    o->shapes = NULL; o->nshapes = 0; o->kdots = NULL; o->fine = NULL; o->still = NULL;
    o->frame = fluid_askview_draw_frame(v);
    o->letters = letters; o->nletters = nletters;
}

void fluid_askview_compose(const fluid_askview_t *v, uint8_t *dots, const uint8_t *primary, fluid_rgba16_t *col) {
    int keep[DIM_STEPS + 1], dim[DIM_STEPS + 1], pdim[DIM_STEPS + 1];
    for (int l = 0; l <= DIM_STEPS; l++) {
        const float p = (float)l / DIM_STEPS;
        keep[l] = (int)((1.f - (1.f - KEEP) * p) * 256.f); dim[l] = (int)((1.f - (1.f - DIM) * p) * 256.f); pdim[l] = l ? (int)(dim[l] * 0.85f) : 256;
    }
    for (int k = 0; k < v->N; k++) {
        if (v->qd[k]) {                               /* the question's dots own the cell */
            const int e = v->ql[k];
            const float kk = fminf(1.f, e / 8.f), hot = fmaxf(0.f, e / 8.f - 1.f) * 0.5f;
            const uint32_t c = v->qc[k];
            const float r = (c >> 16 & 255) * kk, g = (c >> 8 & 255) * kk, b = (c & 255) * kk;
            dots[k] = v->qd[k];
            col[k].r = (uint16_t)((r + (255.f - r) * hot) * 257.f); col[k].g = (uint16_t)((g + (255.f - g) * hot) * 257.f);
            col[k].b = (uint16_t)((b + (255.f - b) * hot) * 257.f); col[k].a = 0;
            continue;
        }
        const int l = v->cp[k];
        if (!dots[k] || !l) continue;
        const bool own = primary && primary[k];
        if (!own) {                                   /* the world's dust thins */
            const uint8_t *h = v->hb + (size_t)k * 8;
            uint8_t m = 0;
            for (int q = 0; q < 8; q++) if (h[q] < keep[l]) m |= (uint8_t)(1 << q);
            dots[k] &= m;
        }
        const int f = own ? pdim[l] : dim[l];
        col[k].r = (uint16_t)(col[k].r * f >> 8); col[k].g = (uint16_t)(col[k].g * f >> 8); col[k].b = (uint16_t)(col[k].b * f >> 8);
    }
}

int fluid_askview_letters(fluid_askview_t *v, fluid_letter_t *out, int max) {
    int n = 0;
    memcpy(v->dmask, v->mask, sizeof(v->mask));
    for (int it = 0; it < v->L.nitems && n < max; it++) {
        if (!v->box[it].on) continue;
        const fluid_ask_item_t *I = &v->L.item[it];
        const bool picked = v->mode == M_PICK && I->opt == v->pick && v->pick >= 0;
        out[n++] = (fluid_letter_t){ .ch = 'A', .x = v->box[it].x, .y = v->box[it].y, .w = v->box[it].w, .h = v->box[it].h, .fs = v->box[it].h,
                                     .rgb = FLUID_RGB | toward_white(I->rgb, picked ? 0.4f * (1.f - smooth(0.f, 0.4f, v->T)) : 0.f),
                                     .mask = v->dmask[it], .mw = (uint16_t)v->box[it].mw, .mh = 7, .whole = true };
    }
    return n;
}
