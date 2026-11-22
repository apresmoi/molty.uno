/*
 * SPDX-License-Identifier: MIT
 *
 * web_field.c: the page's water, for the website. A Molty's screen is one round panel; the page round it is wider than
 * any panel, so it gets a field of its own: the firmware's water (fluid_sim.c, the wave alone: p.still) on a grid as
 * wide as the browser's window, and the firmware's drawer (fluid_draw.c, crisp dots: what it draws a rigged creature's
 * world with) showing the page's dust through it. One water with the screens: a tap anywhere is a bump in this field,
 * a screen takes this field's height at its rim (web_molty.c wm_couple), a heading's window all over.
 *
 * What is NOT the firmware's, and so is the website's own (the "wide dust field"): where the page's dust is.
 *   dust    the motes a rigged creature's world has (creature_dots.c: a dot in fifty, more toward the horizon, DIM or
 *           MOTE, its level by its own hash), with "toward the horizon" read as "toward a screen or a heading": each
 *           focus the page names (wf_focus) is a radial falloff, and a cell's dust is the loose motes plus, by that
 *           falloff, the denser ones. The numbers are creature_dots.c's (the website's tests pin them).
 *   crest   a wave focuses the dust too: where the water is steep (the slope the drawer moves the dots by) the
 *           denser motes show, by how steep, and ride it; they go as it passes. Loose dust alone is a dot in fifty,
 *           and a ring moving that few would not be seen.
 *   tiles   the same dust with no water is what the page shows before any script and wherever the water is flat: two
 *           tiles (wf_picture: loose, and in focus), repeated by CSS on this lattice. The canvas this field draws
 *           into shows only the cells the water moves, opaque, over them; a cell at rest is clear.
 *   cells   8 x 13 px (a 466 px panel's are 7.9 x 12.9): whole pixels, so the lattice repeats everywhere and the field
 *           can stand at any cell of the page. The page shows a pixel of it at the size its screen shows one.
 *
 * A browser instantiates the module once more for this (a screen's instance never calls it). Nothing here draws a
 * creature or a scene. The same file builds natively (the host tests of the website measure the water with it).
 */
#include "web_grid.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define WF_API EMSCRIPTEN_KEEPALIVE
#else
#define WF_API
#endif

#define WF_CW      8                    /* a cell, px */
#define WF_CH      13
#define WF_M_MAX   (FLUID_GRID_MAX_W / 13)   /* the field is 13 m x 8 m cells: a square stage of 104 m px (fluid_sim.h) */
#define WF_TILE_M  10                   /* the dust repeats every 130 x 80 cells: the tiles' side, 1040 px */
#define WF_EDGE    12                   /* cells: the field's rim swallows a wave (the sim's own sponge) */
#define WF_STEP    (1.f / 60.f)         /* mole_fluid.c FLUID_STEP */
#define WF_FOCI    48
#define WF_STOPS   4
/* creature_dots.c: the dust's motes */
#define WF_MOTE_FAR   0.02              /* a dot in fifty is a mote ...                     */
#define WF_MOTE_NEAR  0.09              /* ... and this many more at the horizon (here: in focus) */
#define WF_MOTE_CUT   0.12f             /* one fainter than this does not show              */
/* fluid_draw.c DOT_REFR: the dots shift this many cells per unit of the water's slope */
#define WF_REFR       3.3f
/* the crest: the denser motes come where the slope shifts a dot at all (half a sub-dot: fluid_draw.c rounds it), all of
 * them at WF_CREST sub-dots */
#define WF_CREST_MIN  0.5f
#define WF_CREST      1.f
#define WF_LEVELS     15
#define WF_CALM       90                /* steps with no cell of the window moved: the water is laid flat */
/* A front crosses the page on water that keeps its waves (wf_open's leak and damp); once nothing in the window moves
 * faster than WF_FAST (Q10 a step: water that stands, however it is left standing) for WF_SETTLE steps, the water is
 * the device's again (fluid_params_default) and settles as a screen's does. */
#define WF_FAST       6
#define WF_SETTLE     30
static const float WF_DIM[3] = { 0x5c, 0x56, 0x88 }, WF_BRIGHT[3] = { 0xb4, 0xac, 0xec };

typedef struct { float x, y, rx, ry, st[2 * WF_STOPS]; } wf_focus_t;

static struct {
    fluid_sim_t *sim; fluid_frame_t *fr; fluid_draw_t *dr;
    int W, H, stage;
    int wi, wj, ww, wh;                 /* the window: its first cell in the field, its size, cells */
    int pi, pj;                         /* the page's cell at the window's first */
    uint8_t *dots;                      /* W x H: each cell's dust */
    uint8_t *shown;                     /* W x H: the canvas has the cell opaque */
    uint8_t *still;                     /* W x H: how much in focus the cell is by where it is, 0..255 */
    uint8_t *crest;                     /* W x H: ... and by the wave on it now, 0..WF_LEVELS */
    uint16_t *win;                      /* the window as the drawer draws it, rgb565 (panel order) */
    uint32_t *rgba;                     /* ... and as a canvas takes it */
    int16_t *rects; int nrects;         /* what the last wf_draw changed: x, y, w, h (window px) each */
    wf_focus_t focus[WF_FOCI]; int nfocus;
    float ambient;                      /* every cell this much in focus (the tiles: 0 and 1) */
    uint32_t bg; bool paper;            /* the page under the dust: its colour (0xRRGGBB), and whether it is paper (ink on it) */
    float acc; uint32_t frame;
    bool stale;                         /* the dust is to be laid again (the window moved, the foci changed) */
    bool all;                           /* ... and every cell drawn again */
    int quiet;                          /* steps since the water last moved a dot of the window (1 << 20: it is flat) */
    float leak, damp;                   /* what a step keeps while a front is out */
    int slow;                           /* steps since anything in the window moved fast */
} F;

/* creature_dots.c hash3u, hash3f */
static uint32_t hash3u(int i, int j, int k) {
    uint32_t h = (uint32_t)i * 374761393u + (uint32_t)j * 668265263u + (uint32_t)k * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
static inline float hash3f(int i, int j, int k) { return (float)hash3u(i, j, k) * 2.3283064e-10f; }
static inline int wrap(int a, int n) { const int r = a % n; return r < 0 ? r + n : r; }

/* a mote of `base` at `level` on the page: light on the dark page, ink on paper (darker the brighter it would be) */
static void mote_rgb(const float *base, float level, float *out) {
    const float k = F.paper ? fminf(1.f, level * 1.3f) : level;
    for (int c = 0; c < 3; c++) out[c] = F.paper ? 255.f - (255.f - base[c] * 0.5f) * k : base[c] * k;
}

/* how much in focus the page is at (x, y), page px: 0 loose .. 1 */
static float focus_at(float x, float y) {
    float clear = 1.f - F.ambient;
    for (int q = 0; q < F.nfocus && clear > 0.f; q++) {
        const wf_focus_t *f = &F.focus[q];
        const float dx = (x - f->x) / f->rx, dy = (y - f->y) / f->ry, t2 = dx * dx + dy * dy;
        if (t2 >= 1.f) continue;
        const float t = sqrtf(t2);
        float a = f->st[1];
        for (int s = 0; s < WF_STOPS; s++) {
            const float t1 = f->st[2 * s], a1 = f->st[2 * s + 1];
            if (t <= t1) { const float t0 = s ? f->st[2 * s - 2] : 0.f, a0 = s ? f->st[2 * s - 1] : a1; a = t1 > t0 ? a0 + (a1 - a0) * (t - t0) / (t1 - t0) : a1; break; }
            a = a1;
        }
        clear *= 1.f - a;
    }
    return 1.f - clear;
}

/* The dust of the page's cell (pi, pj), `a` in focus: its dots (a braille byte) and its one colour. The loose motes are
 * always there, at the level they have away from the horizon; the denser ones come with the focus. The colour is the
 * loose tile's cell colour going to the focused tile's by `a`, as the page lays the second tile over the first. */
static uint8_t dust_cell(int pi, int pj, float a, uint32_t *rgba8) {
    static const uint8_t BIT[8] = { 0, 1, 2, 6, 3, 4, 5, 7 };   /* dot (col << 2 | row) -> braille bit */
    uint8_t bits = 0;
    float lf = 0.f, ln = 0.f; const float *bf = NULL, *bn = NULL;
    for (int q = 0; q < 8; q++) {
        const int X = wrap(2 * pi + (q >> 2), 26 * WF_TILE_M), Y = wrap(4 * pj + (q & 3), 32 * WF_TILE_M);
        const double seed = (double)hash3u(X, Y, 11) / 4294967296.0;
        if (seed >= WF_MOTE_FAR + WF_MOTE_NEAR) continue;
        const float tw = hash3f(X, Y, 0), al = 0.3f + 0.55f * tw;
        const float *base = tw > 0.8f ? WF_BRIGHT : WF_DIM;
        const bool far = seed < WF_MOTE_FAR;
        if (far ? al * 0.55f > lf : false) { lf = al * 0.55f; bf = base; }
        if (al > ln) { ln = al; bn = base; }
        if (far || al * a > WF_MOTE_CUT) bits |= (uint8_t)(1 << BIT[q]);
    }
    if (!bits) { *rgba8 = 0; return 0; }
    float cf[3] = { (float)(F.bg >> 16 & 255), (float)(F.bg >> 8 & 255), (float)(F.bg & 255) }, cn[3];
    if (bf) mote_rgb(bf, lf, cf);
    mote_rgb(bn, ln, cn);
    *rgba8 = fluid_pack_rgba8((cf[0] + (cn[0] - cf[0]) * a) / 255.f, (cf[1] + (cn[1] - cf[1]) * a) / 255.f, (cf[2] + (cn[2] - cf[2]) * a) / 255.f, 0.f);
    return bits;
}

/* the dust of field cell k as it is in focus now: by where it is, and by the wave on it */
static void lay_cell(int k) {
    const float a = 1.f - (1.f - F.still[k] * (1.f / 255.f)) * (1.f - F.crest[k] * (1.f / WF_LEVELS));
    F.dots[k] = dust_cell(F.pi - F.wi + k % F.W, F.pj - F.wj + k / F.W, a, &F.sim->tcol[k]);
}

static void lay_dust(void) {
    for (int j = 0; j < F.H; j++) for (int i = 0; i < F.W; i++) {
        const int k = j * F.W + i;
        F.still[k] = (uint8_t)(focus_at((F.pi - F.wi + i + 0.5f) * WF_CW, (F.pj - F.wj + j + 0.5f) * WF_CH) * 255.f + 0.5f);
        F.crest[k] = 0;
        lay_cell(k);
    }
    F.stale = false; F.all = true;
}

/* the wave focuses the dust: each cell of the window by the slope of the water on it (as fluid_draw.c wave_shift
 * measures it, in sub-dots of shift). True: some cell's dust changed. */
static bool lay_crest(void) {
    const fluid_sim_t *s = F.sim;
    const int W = F.W;
    const int16_t *h = s->h;
    const float rx = WF_REFR * (1.f / FQ_H) * 2.f, ry = WF_REFR * (1.f / FQ_H) * 4.f * F.H / W;
    bool changed = false;
    for (int j = F.wj; j < F.wj + F.wh; j++) for (int i = F.wi; i < F.wi + F.ww; i++) {
        const int k = j * W + i;
        const float fx = (float)(h[k + 1] - h[k - 1]) * rx, fy = (float)(h[k + W] - h[k - W]) * ry, m = sqrtf(fx * fx + fy * fy);
        const int lv = m < WF_CREST_MIN ? 0 : m >= WF_CREST ? WF_LEVELS : 1 + (int)((WF_LEVELS - 1) * (m - WF_CREST_MIN) / (WF_CREST - WF_CREST_MIN));
        if (lv == F.crest[k]) continue;
        F.crest[k] = (uint8_t)lv;
        lay_cell(k);
        changed = true;
    }
    return changed;
}

WF_API void wf_close(void) {
    fluid_frame_destroy(F.fr); fluid_draw_destroy(F.dr); fluid_sim_destroy(F.sim);
    free(F.dots); free(F.shown); free(F.still); free(F.crest); free(F.win); free(F.rgba); free(F.rects);
    memset(&F, 0, sizeof(F));
}

/** The field for a window of ww x wh cells (8 x 13 px each: the canvas is that many px), on a page whose colour under
 *  the dust is bg (0xRRGGBB; paper: its dust is ink). The water's own numbers: c2 (cells a step, squared), leak, damp
 *  (fluid_params_t). 0, or -1 (no memory, or a window wider than the largest field: ask for a smaller one). */
WF_API int wf_open(int ww, int wh, uint32_t bg, int paper, float c2, float leak, float damp) {
    wf_close();
    int m = (ww + 2 * WF_EDGE + 12) / 13, mh = (wh + 2 * WF_EDGE + 7) / 8;
    if (mh > m) m = mh;
    if (ww < 1 || wh < 1 || m > WF_M_MAX) return -1;
    if (m < 2) m = 2;
    F.W = 13 * m; F.H = 8 * m; F.stage = 104 * m;
    F.ww = ww; F.wh = wh; F.wi = (F.W - ww) / 2; F.wj = (F.H - wh) / 2;
    F.bg = bg & 0xffffffu; F.paper = paper != 0;
    F.sim = fluid_sim_create(F.W, F.H, F.stage);
    F.fr = fluid_frame_create(F.W, F.H);
    F.dr = fluid_draw_create(F.W, F.H, F.stage, F.stage);
    const size_t N = (size_t)F.W * F.H, px = (size_t)ww * WF_CW * wh * WF_CH;
    F.dots = calloc(N, 1); F.shown = calloc(N, 1); F.still = calloc(N, 1); F.crest = calloc(N, 1); F.win = calloc(px, sizeof(uint16_t)); F.rgba = calloc(px, sizeof(uint32_t));
    F.rects = malloc(sizeof(int16_t) * 4 * (size_t)(wh * (ww / 2 + 1) + 1));
    if (!F.sim || !F.fr || !F.dr || !F.dots || !F.shown || !F.still || !F.crest || !F.win || !F.rgba || !F.rects) { wf_close(); return -1; }
    fluid_sim_t *s = F.sim;
    s->p.still = true; s->p.c2 = c2;
    F.leak = leak; F.damp = damp; F.slow = WF_SETTLE + 1;
    /* a rectangle of water, not a disc: every cell but the border's, and the sim's own sponge along the field's rim */
    for (int j = 0; j < F.H; j++) for (int i = 0; i < F.W; i++) {
        const int k = j * F.W + i, in = i < F.W - 1 - i ? i : F.W - 1 - i, jn = j < F.H - 1 - j ? j : F.H - 1 - j, edge = in < jn ? in : jn;
        s->mask[k] = edge > 0;
        s->sponge[k] = (uint8_t)(edge >= WF_EDGE ? 0 : 255 - 255 * edge / WF_EDGE);
    }
    F.stale = true; F.quiet = 1 << 20;
    return 0;
}

/** The page's cell under the window's first one. Moving the window carries the water with the page. */
WF_API void wf_place(int pi, int pj) {
    if (!F.sim || (pi == F.pi && pj == F.pj)) return;
    const int di = pi - F.pi, dj = pj - F.pj, W = F.W, H = F.H;
    F.pi = pi; F.pj = pj; F.stale = true;
    fluid_sim_t *s = F.sim;
    int16_t *f[2] = { s->h, s->hp };
    for (int q = 0; q < 2; q++) {
        if (abs(di) >= W || abs(dj) >= H) { memset(f[q], 0, sizeof(int16_t) * (size_t)W * H); continue; }
        for (int n = 0; n < H; n++) {
            const int j = dj >= 0 ? n : H - 1 - n, js = j + dj;      /* rows in the order that reads before it writes */
            int16_t *row = f[q] + j * W;
            if (js < 0 || js >= H) { memset(row, 0, sizeof(int16_t) * (size_t)W); continue; }
            const int16_t *src = f[q] + js * W;
            if (di >= 0) { memmove(row, src + di, sizeof(int16_t) * (size_t)(W - di)); memset(row + W - di, 0, sizeof(int16_t) * (size_t)di); }
            else { memmove(row - di, src, sizeof(int16_t) * (size_t)(W + di)); memset(row, 0, sizeof(int16_t) * (size_t)-di); }
        }
    }
}

/** What focuses the page's dust: n foci of 4 + 8 numbers each: the centre (page px), the radii, and WF_STOPS stops
 *  (how far out, 0..1 of the radius; how much in focus, 0..1) from the centre out; beyond the radius: none. */
WF_API void wf_focus(const float *f, int n) {
    F.nfocus = n < 0 ? 0 : n > WF_FOCI ? WF_FOCI : n;
    memcpy(F.focus, f, sizeof(wf_focus_t) * (size_t)F.nfocus);
    F.stale = true;
}

/** A tap at (x, y), page px: a bump `strength` high and r px wide, as a finger's on a screen (mole_fluid.c ripple). */
WF_API void wf_tap(float x, float y, float strength, float r) {
    if (!F.sim) return;
    const float half = F.stage * 0.5f;
    fluid_sim_impulse(F.sim, (x - (F.pi - F.wi) * WF_CW) / half - 1.f, (y - (F.pj - F.wj) * WF_CH) / half - 1.f, strength, r / half);
    F.quiet = F.slow = 0;
}

/** dt s of the water (fixed steps, at most 4, as mole_fluid.c takes them). Returns 2 while a front is out (the water
 *  keeps its waves), 1 while what is left settles (the water is the device's), 0 once it is flat: no cell of the window
 *  off its rest for a while, and it is laid flat (what is left of a wave is under any dot's sight). */
WF_API int wf_step(float dt) {
    if (!F.sim || F.quiet >= 1 << 20) return 0;
    fluid_sim_t *s = F.sim;
    fluid_params_t own;
    fluid_params_default(&own);
    const bool front = F.slow <= WF_SETTLE;
    s->p.leak = front ? F.leak : own.leak; s->p.damp = front ? F.damp : own.damp;
    F.acc += dt;
    int n = 0;
    for (; F.acc >= WF_STEP && n < 4; n++, F.acc -= WF_STEP) fluid_sim_step(s, WF_STEP);
    if (n == 4) F.acc = 0.f;
    /* does it move a dot anywhere in the window (the slope the drawer shifts a dot by: lay_crest), and how fast */
    const int W = F.W;
    const int16_t *h = s->h, *hp = s->hp;
    const float lim = WF_CREST_MIN / (WF_REFR * (1.f / FQ_H) * 2.f), ay = 2.f * F.H / W;
    bool moves = false, fast = false;
    for (int j = F.wj; j < F.wj + F.wh && !(moves && fast); j++) for (int i = F.wi; i < F.wi + F.ww; i++) {
        const int k = j * W + i, v = h[k] - hp[k];
        const float fx = (float)(h[k + 1] - h[k - 1]), fy = (float)(h[k + W] - h[k - W]) * ay;
        moves |= fx * fx + fy * fy >= lim * lim;
        fast |= v >= WF_FAST || v <= -WF_FAST;
    }
    F.quiet = moves ? 0 : F.quiet + n;
    F.slow = fast ? 0 : F.slow + n;
    if (F.quiet > WF_CALM) {
        memset(s->h, 0, sizeof(int16_t) * (size_t)s->N); memset(s->hp, 0, sizeof(int16_t) * (size_t)s->N);
        F.quiet = 1 << 20;
    }
    return F.slow <= WF_SETTLE ? 2 : 1;
}

static inline uint32_t rgba_of(uint16_t be) {
    const uint16_t v = (uint16_t)(be >> 8 | be << 8);
    const uint32_t r5 = v >> 11, g6 = v >> 5 & 63, b5 = v & 31;
    return 0xff000000u | (b5 << 3 | b5 >> 2) << 16 | (g6 << 2 | g6 >> 4) << 8 | (r5 << 3 | r5 >> 2);
}

/* the drawer's frame for the water as it is; whole: every cell is looked at and drawn */
static fluid_draw_opts_t draw_cells(void) {
    const bool laid = F.stale;
    if (laid) lay_dust();
    if (lay_crest() || laid) fluid_sim_color(F.sim, 1.f);   /* still water: each cell its target's colour */
    fluid_sim_snapshot(F.sim, F.fr, false);
    const fluid_draw_opts_t o = { .palette = FLUID_PAL_PASTEL, .edge_mode = FLUID_EDGE_CONTOURS, .scene = true, .dots = F.dots,
                                  .frame = FLUID_FRAME(F.frame++, 2) };
    if (F.all) F.dr->force = true;
    fluid_draw_cells(F.dr, F.fr, &o);
    return o;
}

/** Draw what changed. The canvas's pixels are wf_pixels (ww * 8 x wh * 13, RGBA): a cell the water moves is opaque (the
 *  page's colour and its dust where the water put it), a cell at rest clear (the page's own tile shows). Returns how
 *  many boxes changed: wf_rects, 4 numbers each (x, y, w, h). */
WF_API int wf_draw(void) {
    if (!F.sim) return 0;
    const fluid_draw_opts_t o = draw_cells();
    const fluid_draw_t *d = F.dr;
    const int W = F.W, pw = F.ww * WF_CW, x0 = F.wi * WF_CW, y0 = F.wj * WF_CH;
    const fluid_rect_t t = { F.win, pw, x0, y0, x0 + pw, y0 + F.wh * WF_CH };
    fluid_draw_rect(d, &o, &t, F.all);
    const uint32_t bg = 0xff000000u | (F.bg & 255) << 16 | (F.bg & 0xff00) | F.bg >> 16;
    F.nrects = 0;
    for (int j = F.wj; j < F.wj + F.wh; j++) {
        /* (every cell of the row: one back at rest that looks as it did moved is not the drawer's to draw again, but
         * is the canvas's to clear) */
        const int a = F.wi, b = F.wi + F.ww;
        for (int i = a, run = -1, last = -1; i <= b; i++) {
            /* at rest: the drawer left the cell its own dots in its own colour (fluid_draw.c crisp_flat), and no wave
             * has brought it more */
            const bool rest = i < b && (d->cd0[j * W + i] & 0x100) && !F.crest[j * W + i];
            const bool dirty = i < b && (F.all || d->dirty[j * W + i] || (rest && F.shown[j * W + i]));
            if (dirty) {
                F.shown[j * W + i] = !rest;
                for (int y = (j - F.wj) * WF_CH; y < (j - F.wj + 1) * WF_CH; y++) {
                    const uint16_t *in = F.win + (size_t)y * pw + (i - F.wi) * WF_CW;
                    uint32_t *out = F.rgba + (size_t)y * pw + (i - F.wi) * WF_CW;
                    for (int x = 0; x < WF_CW; x++) out[x] = rest ? 0 : in[x] ? rgba_of(in[x]) : bg;
                }
                if (run < 0) run = i;
                last = i;
            }
            /* a run of changed cells is a box; a gap of a few cells does not end it (a box costs the caller a call) */
            if (run >= 0 && (i >= b || i - last > 6)) {
                int16_t *r = F.rects + 4 * F.nrects++;
                r[0] = (int16_t)((run - F.wi) * WF_CW); r[1] = (int16_t)((j - F.wj) * WF_CH); r[2] = (int16_t)((last + 1 - run) * WF_CW); r[3] = WF_CH;
                run = -1;
            }
        }
    }
    if (F.all) { F.nrects = 1; F.rects[0] = F.rects[1] = 0; F.rects[2] = (int16_t)pw; F.rects[3] = (int16_t)(F.wh * WF_CH); }
    F.all = false;
    return F.nrects;
}
WF_API const int16_t *wf_rects(void) { return F.rects; }
WF_API const uint8_t *wf_pixels(void) { return (const uint8_t *)F.rgba; }

/** The dust alone, all of it `ambient` in focus (0: the loose tile, 1: the tile in focus), on flat water: the window
 *  drawn whole into wf_pixels, clear between the dots. A field opened 130 x 80 cells gives one tile (1040 px a side). */
WF_API const uint8_t *wf_picture(float ambient) {
    if (!F.sim) return NULL;
    F.ambient = ambient; F.stale = true;
    const fluid_draw_opts_t o = draw_cells();
    const int pw = F.ww * WF_CW, ph = F.wh * WF_CH, x0 = F.wi * WF_CW, y0 = F.wj * WF_CH;
    const fluid_rect_t t = { F.win, pw, x0, y0, x0 + pw, y0 + ph };
    fluid_draw_rect(F.dr, &o, &t, true);
    for (size_t q = 0; q < (size_t)pw * ph; q++) F.rgba[q] = F.win[q] ? rgba_of(F.win[q]) : 0;
    F.ambient = 0.f; F.stale = F.all = true;
    return (const uint8_t *)F.rgba;
}

/** The water under n points: xy (x, y each, any px) placed on the page at (ox + x * scale, oy + y * scale) page px;
 *  out gets its height now and a step ago (h, hp each), as a screen's rim takes it (wm_couple). */
WF_API void wf_sample(const float *xy, int n, float ox, float oy, float scale, float *out) {
    if (!F.sim) return;
    const fluid_sim_t *s = F.sim;
    const int W = F.W, H = F.H;
    const float fx0 = (float)((F.pi - F.wi) * WF_CW), fy0 = (float)((F.pj - F.wj) * WF_CH);
    for (int q = 0; q < n; q++) {
        const float gx = (ox + xy[2 * q] * scale - fx0) / WF_CW - 0.5f, gy = (oy + xy[2 * q + 1] * scale - fy0) / WF_CH - 0.5f;
        const int i = (int)floorf(gx), j = (int)floorf(gy);
        if (i < 0 || j < 0 || i >= W - 1 || j >= H - 1) { out[2 * q] = out[2 * q + 1] = 0.f; continue; }
        const float u = gx - i, v = gy - j;
        const int k = j * W + i;
        for (int c = 0; c < 2; c++) {
            const int16_t *a = c ? s->hp : s->h;
            const float top = a[k] + (a[k + 1] - a[k]) * u, bot = a[k + W] + (a[k + W + 1] - a[k + W]) * u;
            out[2 * q + c] = (top + (bot - top) * v) * (1.f / FQ_H);
        }
    }
}

/** The field's grid and its water's height (W x H, Q10), for the tests that measure a wave. */
WF_API int wf_grid_w(void) { return F.W; }
WF_API int wf_grid_h(void) { return F.H; }
WF_API int wf_window_i(void) { return F.wi; }
WF_API int wf_window_j(void) { return F.wj; }
WF_API const int16_t *wf_h(void) { return F.sim ? F.sim->h : NULL; }
