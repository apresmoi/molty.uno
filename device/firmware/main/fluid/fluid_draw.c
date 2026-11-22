/*
 * fluid_draw.c — draw the fluid as braille dots / ASCII glyphs into the framebuffer.
 *
 * Port of device/creatures/js/fluid/render.js. Braille is drawn as our own
 * 2×4 dot rectangles (no font): sub-samples are bilinear reads of the view field at
 * the 8 dot centres, then either
 *   contour — the cell straddles an edge: dots on the dense side of its own mid-level
 *   tonal   — ordered dither on the GLOBAL 4×4 Bayer lattice of sub-dots (+ a per-dot
 *             jitter = the living shimmer of the dim field) → 0…8 dots
 * edge_mode "off" falls back to glyph-ramp ASCII via the 5×7 device font, with
 * streamline strokes (- \ | /) where the ink moves fast.
 *
 * Dirty cells only: each cell remembers (kind|dots, rgb565) and is cleared + redrawn
 * only when that changes; the caller flushes the returned row band, or draws just the
 * changed windows (fluid_draw_rect) into a small buffer for the panel. Cells under a
 * condensed speed-reader letter are the word's dark box: cleared every frame, then
 * the letters are drawn on top.
 */
#include "fluid_sim.h"
#include "fluid_fine.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_timer.h"
#define FLUID_CALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM)
#define FLUID_MALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM)
#define FLUID_FREE(p)   heap_caps_free(p)
#define FLUID_REALLOC(p, n) heap_caps_realloc((p), (n), MALLOC_CAP_SPIRAM)
static inline int64_t now_us(void) { return esp_timer_get_time(); }
#else
#include <time.h>
#ifdef FLUID_TEST_ALLOC                                  /* (host tests: every size checked against a ceiling, scene_test.c) */
void *fluid_test_alloc(void *p, size_t n, bool zero);
#define FLUID_CALLOC(n) fluid_test_alloc(NULL, (n), true)
#define FLUID_MALLOC(n) fluid_test_alloc(NULL, (n), false)
#define FLUID_REALLOC(p, n) fluid_test_alloc((p), (n), false)
#else
#define FLUID_CALLOC(n) calloc(1, (n))
#define FLUID_MALLOC(n) malloc(n)
#define FLUID_REALLOC(p, n) realloc((p), (n))
#endif
#define FLUID_FREE(p)   free(p)
static inline int64_t now_us(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000; }
#endif

/* ---- tunables (render.js R) ---- */
#define EDGE_RANGE    0.16f   /* sub-sample spread that makes a cell a contour cell       */
#define EDGE_MIN      0.30f   /* …only where something solid is there                     */
#define DITHER_GAMMA_IS_ONE 1   /* render.js DITHER_GAMMA = 1.0 → skip the LUT */
#define DITHER_GAMMA  1.0f    /* value^γ before dithering: long, soft fades               */
#define DITHER_GAIN   0.97f
#define DIM_FLOOR     0.16f   /* brightness of a 1-dot cell                               */
#define SHIMMER_HZ    24.f    /* (used by the caller to derive `frame`)                   */
#define DOT_W         0.26f   /* dot size as a fraction of the cell                       */
#define WORD_INK      0.3f    /* a word's dot shows once the fluid under it is this dense   */
#define DOT_H         0.15f
#define DOT_REFR      3.3f    /* a scene's dots and symbols shift this many cells per unit of
                               * wave slope (fluid_sim_view's refr 1.1 x 3: they move with the water) */
/* a scene in high definition (fluid_draw_opts_t.scene) breaks into dust: */
#define DUST_CALM     2.f     /* the wave only shifts its dots (DOT_REFR) up to this many sub-dots;
                               * steeper (near a touch) they scatter                        */
#define DUST_GAIN     1.f     /* scatter radius, sub-dots, per sub-dot of shift beyond DUST_CALM */
#define DUST_MAX      8.f     /* the most a touch scatters a dot, sub-dots                  */
#define DUST_REACH    8.f     /* a transition's scatter at dust 1, sub-dots (and it thins them out) */
#define SCENE_LEVEL   0.85f   /* a scene dot's brightness (scene_player's SCENE_INK)        */
/* ...and with its shapes (fluid_draw_opts_t.shapes) it is drawn at the panel's pixels while calm: */
#define HD_CALM       1.f     /* the wave shifts the dots up to this many sub-dots: still smooth */
#define HD_FADE       1.f     /* this many more and the cell is all dust (DUST_CALM: they scatter) */
#define HD_DUST       3.f     /* a transition's dust fades the shapes this much faster      */
#define HD_LEVELS     8       /* a cell's smooth weight, in steps (its key: a step redraws it) */
#define HD_EDGE       0.42f   /* a mask's or a letter's edge, on its bilinear coverage      */
#define HAZE_AMBIENT  0.12f   /* around it, a faint dust: its dither of this much ink       */
#define HAZE_LEVEL    0.3f    /* ...at this brightness, well under the drawing's            */
#define HAZE_JITTER   0.5f    /* ...its dither thresholds this noisy (a dust, not a lattice)  */
#define HAZE_HZ       1.f     /* ...each cell row reseeded this often, rows at their own moments:
                               * a slow shimmer that redraws a row or so a frame             */
#define CAP_FADE      3.f     /* the subtitle sits on dark: the dust thins to none this many dots
                               * from its letters                                        */
#define HALO_REACH    3       /* ...and denser and brighter this many dots from the drawing: the
                               * dust taking its shape                                    */
static const float HALO_INK[HALO_REACH + 1]   = { 0.f, 0.34f, 0.16f, 0.07f };  /* a dot's ink, by distance */
static const float HALO_LEVEL[HALO_REACH + 1] = { 0.55f, 0.55f, 0.45f, 0.36f }; /* a cell's brightness, by its nearest */
#define RUN_GAP       3       /* a scene's changed cells this many smooth ones apart are sent as two
                               * windows (the shapes between are not redrawn; anything else
                               * between is cheaper to send than another window)          */

#define KEY_EMPTY 0x0000u
#define KEY_GLYPH 0x0100u     /* | char                                                   */
#define KEY_SKIP  0xFFFFu
#define KEY_STALE 0xFFFEu     /* never drawn: the cell is redrawn next frame (overlays)   */
#define KEY_HD    0x8000u     /* | weight << 8 | dots: a scene's shapes at weight / HD_LEVELS,
                               * its dots over them                                       */
static inline bool key_hd(uint16_t key) { return (key & 0xF000u) == KEY_HD; }

const char *const FLUID_PAL_NAMES[FLUID_PAL_COUNT]   = { "green", "amber", "pastel" };
const char *const FLUID_EDGE_NAMES[FLUID_EDGE_COUNT] = { "off", "contours", "all" };

static const float PAL[FLUID_PAL_COUNT][3][3] = {     /* lo, hi, acc (0..255) */
    { { 10, 60, 28 },  { 150, 255, 175 }, { 255, 214, 90 } },
    { { 70, 30, 0 },   { 255, 190, 90 },  { 255, 255, 245 } },
    { { 0, 0, 0 },     { 255, 255, 255 }, { 255, 140, 60 } },
};

uint32_t fluid_palette_accent(int palette) {
    const int pal = (palette >= 0 && palette < FLUID_PAL_COUNT) ? palette : FLUID_PAL_PASTEL;
    const float *ac = PAL[pal][2];
    return (uint32_t)ac[0] << 16 | (uint32_t)ac[1] << 8 | (uint32_t)ac[2];
}

/* 4×4 Bayer over the global sub-dot lattice (2W × 4H): (v + 0.5) / 16 */
static const float BAYER4[16] = {
    0.5f / 16, 8.5f / 16, 2.5f / 16, 10.5f / 16, 12.5f / 16, 4.5f / 16, 14.5f / 16, 6.5f / 16,
    3.5f / 16, 11.5f / 16, 1.5f / 16, 9.5f / 16, 15.5f / 16, 7.5f / 16, 13.5f / 16, 5.5f / 16,
};

/* DITHER_GAIN · v^DITHER_GAMMA, 64 segments, filled once */
static float s_glut[65];
static bool  s_glut_ok = false;

#if !DITHER_GAMMA_IS_ONE
static inline float gamma_lut(float v) {
    if (v <= 0.f) return 0.f;
    if (v >= 1.f) return s_glut[64];
    float f = v * 64.f; int i = (int)f;
    return s_glut[i] + (s_glut[i + 1] - s_glut[i]) * (f - i);
}
#endif

static inline float hash_u(int a, int b, uint32_t c) {
    uint32_t h = (uint32_t)a * 0x27d4eb2du ^ (uint32_t)b * 0x165667b1u ^ c * 0x9e3779b1u;
    h ^= h >> 15; h *= 0x85ebca6bu; h ^= h >> 13;
    return (float)(h & 0xFFFFu) * (1.f / 65536.f);
}

/* sqrt for the shapes' per-pixel coverage: no libm call (it is one on the device), two Newton steps (~1e-5) */
static inline float fsqrt(float x) {
    if (x <= 0.f) return 0.f;
    union { float f; uint32_t i; } c = { x };
    c.i = 0x5f3759dfu - (c.i >> 1);
    float y = c.f;
    y *= 1.5f - 0.5f * x * y * y;
    y *= 1.5f - 0.5f * x * y * y;
    return x * y;
}
/* floor and ceiling of a coordinate on or near the panel (no libm call) */
static inline int ifloor(float v) { return (int)(v + 8192.f) - 8192; }
static inline int iceil(float v) { return 8192 - (int)(8192.f - v); }   /* (both for -8192 < v < 8192) */

static inline uint16_t rgb565_be(int r, int g, int b) {
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    uint16_t le = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return (uint16_t)((le >> 8) | (le << 8));
}

uint8_t fluid_braille_dither(const float *sub, int si, int sj, float shimmer, uint32_t frame) {
    /* one hash per cell, a 4-bit jitter per dot: the same uniform threshold wobble as a
     * per-dot hash at 1/8 of the cost */
    uint32_t hh = shimmer > 0.f ? (uint32_t)(hash_u(si, sj, frame) * 65536.f) * 0x9e3779b1u : 0;
    const float js = shimmer * (1.f / 16.f), j0 = shimmer * (0.5f / 16.f - 0.5f);
    uint8_t dots = 0;
    for (int q = 0; q < 8; q++) {
        int col = q >> 2, row = q & 3;
        int sx = si * 2 + col, sy = sj * 4 + row;
        float thr = BAYER4[((sy & 3) << 2) | (sx & 3)];
        if (shimmer > 0.f) thr += j0 + js * (float)((hh >> (q * 4)) & 15);
#if DITHER_GAMMA_IS_ONE
        if (DITHER_GAIN * sub[q] > thr) dots |= (uint8_t)(1u << fluid_braille_bit(col, row));
#else
        if (gamma_lut(sub[q]) > thr) dots |= (uint8_t)(1u << fluid_braille_bit(col, row));
#endif
    }
    return dots;
}

uint8_t fluid_braille_contour(const float *sub) {
    float mn = sub[0], mx = sub[0];
    for (int q = 1; q < 8; q++) { if (sub[q] < mn) mn = sub[q]; if (sub[q] > mx) mx = sub[q]; }
    float mid = (mn + mx) * 0.5f;
    uint8_t dots = 0;
    for (int q = 0; q < 8; q++) if (sub[q] > mid) dots |= (uint8_t)(1u << fluid_braille_bit(q >> 2, q & 3));
    return dots;
}

/* ── create / destroy ────────────────────────────────────────────────────── */

static void mm_free(fluid_morph_mem_t *mm);

fluid_draw_t *fluid_draw_create(int W, int H, int fb_w, int fb_h) {
    if (W <= 0 || H <= 0 || fb_w < 32 || fb_h < 32 || fb_w > 4096 || fb_h > 4096) return NULL;
    if (!s_glut_ok) {
        for (int i = 0; i <= 64; i++) s_glut[i] = DITHER_GAIN * powf(i / 64.f, DITHER_GAMMA);
        s_glut_ok = true;
    }
    fluid_draw_t *d = FLUID_CALLOC(sizeof(fluid_draw_t));
    if (!d) return NULL;
    size_t N = (size_t)W * H;
    size_t bytes = (size_t)(W + 1 + H + 1 + 2 * W + 6 * H) * sizeof(int16_t) + 2 * N * sizeof(uint16_t) + 49 * N + 1 + (size_t)fb_h + (size_t)fb_w;
    d->block = FLUID_CALLOC(bytes);
    if (!d->block) { FLUID_FREE(d); return NULL; }
    d->cd0 = FLUID_CALLOC(N * sizeof(uint16_t)); d->csig = FLUID_CALLOC(N * sizeof(uint32_t)); d->why = FLUID_CALLOC(N);
    if (!d->cd0 || !d->csig || !d->why) { fluid_draw_destroy(d); return NULL; }
    int16_t *p16 = (int16_t *)d->block;
    d->cx = p16; d->cy = d->cx + W + 1; d->dx = d->cy + H + 1; d->dy = d->dx + 2 * W; d->span = d->dy + 4 * H;
    d->key = (uint16_t *)(d->span + 2 * H); d->col = d->key + N;
    d->skip = (uint8_t *)(d->col + N); d->dirty = d->skip + N;
    d->yrow = (int8_t *)(d->dirty + N);
    d->hds = (uint8_t *)d->yrow + fb_h; d->hdw = d->hds + N; d->halo = d->hdw + fb_w;
    d->halo1 = d->halo + 8 * N; d->hde = d->halo1 + 8 * N; d->hch = d->hde + N; d->cdist = d->hch + N; d->scat = (int8_t *)(d->cdist + N); d->pdots = (uint8_t *)d->scat + 24 * N;
    d->psrc = (int16_t *)(((uintptr_t)(d->pdots + N) + 1) & ~(uintptr_t)1);
    for (int y = 0; y < 4 * H; y++) for (int x = 0; x < 2 * W; x++) {
        int8_t *sc = d->scat + 3 * (y * 2 * W + x);
        sc[0] = (int8_t)lrintf((hash_u(x, y, 1) * 2.f - 1.f) * 127.f);
        sc[1] = (int8_t)lrintf((hash_u(x, y, 2) * 2.f - 1.f) * 127.f);
        sc[2] = (int8_t)((int)(hash_u(x, y, 3) * 255.f) - 128);
    }
    d->cap0[0] = 1; d->cap0[1] = 0;                        /* no subtitle */
    d->W = W; d->H = H;
    d->fb_w = fb_w; d->fb_h = fb_h;
    d->stage = fb_w < fb_h ? fb_w : fb_h;
    const int ox = d->ox = (fb_w - d->stage) / 2, oy = d->oy = (fb_h - d->stage) / 2;
    const float cw = (float)d->stage / W, ch = (float)d->stage / H;
    float dw = cw * DOT_W, dh = ch * DOT_H;
    d->dot_w = (int)lrintf(dw < 1.5f ? 1.5f : dw); if (d->dot_w < 1) d->dot_w = 1;
    d->dot_h = (int)lrintf(dh < 1.5f ? 1.5f : dh); if (d->dot_h < 1) d->dot_h = 1;
    for (int i = 0; i <= W; i++) d->cx[i] = (int16_t)(ox + lrintf(i * cw));
    for (int j = 0; j <= H; j++) d->cy[j] = (int16_t)(oy + lrintf(j * ch));
    for (int i = 0; i < W; i++) for (int c = 0; c < 2; c++)
        d->dx[i * 2 + c] = (int16_t)(ox + lrintf(i * cw + (c ? 0.7f : 0.3f) * cw - d->dot_w * 0.5f));
    for (int j = 0; j < H; j++) for (int r = 0; r < 4; r++)
        d->dy[j * 4 + r] = (int16_t)(oy + lrintf(j * ch + (0.14f + 0.24f * r) * ch - d->dot_h * 0.5f));
    memset(d->yrow, -1, (size_t)fb_h);
    for (int j = 0; j < H; j++) for (int r = 0; r < 4; r++)
        for (int y = d->dy[j * 4 + r]; y < d->dy[j * 4 + r] + d->dot_h; y++)
            if (y >= d->cy[j] && y < d->cy[j + 1]) d->yrow[y] = (int8_t)r;
    d->force = true;
    return d;
}

void fluid_draw_destroy(fluid_draw_t *d) {
    if (!d) return;
    FLUID_FREE(d->block); FLUID_FREE(d->sd_dist); FLUID_FREE(d->cd0); FLUID_FREE(d->csig); FLUID_FREE(d->shp0); FLUID_FREE(d->sh1); FLUID_FREE(d->near); FLUID_FREE(d->hdc); FLUID_FREE(d->hdcv);
    mm_free(d->m.mm); mm_free(d->m.jfree); FLUID_FREE(d->m.colx); FLUID_FREE(d->why);
    FLUID_FREE(d);
}

/* ── primitives ──────────────────────────────────────────────────────────── */

/* fb pixel (x, y) in the window t */
static inline uint16_t *rect_px(const fluid_rect_t *t, int x, int y) {
    return t->px + (size_t)(y - t->y0) * t->stride + (x - t->x0);
}

/* 5×7 glyph scaled (nearest) into the box (x0,y0,w,h), clipped to [cx0,cx1)×[cy0,cy1)
 * and to the window t */
static void draw_glyph(const fluid_rect_t *t, const uint8_t *gl, int x0, int y0, int w, int h, uint16_t c,
                       int cx0, int cy0, int cx1, int cy1) {
    if (!gl || w <= 0 || h <= 0) return;
    if (cx0 < t->x0) cx0 = t->x0;
    if (cy0 < t->y0) cy0 = t->y0;
    if (cx1 > t->x1) cx1 = t->x1;
    if (cy1 > t->y1) cy1 = t->y1;
    for (int py = 0; py < h; py++) {
        int y = y0 + py; if (y < cy0 || y >= cy1) continue;
        uint8_t bits = gl[py * 7 / h];
        if (!bits) continue;
        uint16_t *row = rect_px(t, t->x0, y);
        for (int px = 0; px < w; px++) {
            int x = x0 + px; if (x < cx0 || x >= cx1) continue;
            if ((bits >> (4 - px * 5 / w)) & 1) row[x - t->x0] = c;
        }
    }
}

/* a symbol (fluid_letter_t.mask): its 1-bit image scaled into w x h px, nearest pixel */
static void draw_mask(const fluid_rect_t *t, const uint8_t *m, int mw, int mh, int x0, int y0, int w, int h, uint16_t c,
                      int cx0, int cy0, int cx1, int cy1) {
    if (!m || w <= 0 || h <= 0) return;
    if (cx0 < t->x0) cx0 = t->x0;
    if (cy0 < t->y0) cy0 = t->y0;
    if (cx1 > t->x1) cx1 = t->x1;
    if (cy1 > t->y1) cy1 = t->y1;
    for (int py = 0; py < h; py++) {
        const int y = y0 + py;
        if (y < cy0 || y >= cy1) continue;
        const int my = py * mh / h;
        uint16_t *row = rect_px(t, t->x0, y);
        for (int px = 0; px < w; px++) {
            const int x = x0 + px;
            if (x < cx0 || x >= cx1) continue;
            const int p = my * mw + px * mw / w;
            if (m[p >> 3] >> (7 - (p & 7)) & 1) row[x - t->x0] = c;
        }
    }
}

/* no contour ring around the condensed letters: cells next to a letter only dither (and
 * none at all next to a word's dots) */
static inline bool near_skip(const uint8_t *sk, int W, int H, int i, int j) {
    for (int dj = -1; dj <= 1; dj++) for (int di = -1; di <= 1; di++) {
        int x = i + di, y = j + dj;
        if (x >= 0 && y >= 0 && x < W && y < H && sk[y * W + x]) return true;
    }
    return false;
}


/* ── draw ────────────────────────────────────────────────────────────────── */

#define IU (1.f / FQ_UNIT)

/* the 8 dot sub-samples of interior cell (i,j): bilinear reads at x = i ∓ 0.25,
 * y = j + (row − 1.5)/4 — always inside the 3×3 neighbourhood with fixed weights,
 * so this is exactly the JS bilinear sampler without per-sample index math. */
static inline void subsample(const int16_t *v, int W, int k, float *sub, float *smin, float *smax, float *smean) {
    float L[3], R[3];
    for (int r = 0; r < 3; r++) {
        const int16_t *p = v + k + (r - 1) * W;
        float c = p[0];
        L[r] = 0.25f * p[-1] + 0.75f * c;
        R[r] = 0.75f * c + 0.25f * p[1];
    }
    sub[0] = (0.375f * L[0] + 0.625f * L[1]) * IU;
    sub[1] = (0.125f * L[0] + 0.875f * L[1]) * IU;
    sub[2] = (0.875f * L[1] + 0.125f * L[2]) * IU;
    sub[3] = (0.625f * L[1] + 0.375f * L[2]) * IU;
    sub[4] = (0.375f * R[0] + 0.625f * R[1]) * IU;
    sub[5] = (0.125f * R[0] + 0.875f * R[1]) * IU;
    sub[6] = (0.875f * R[1] + 0.125f * R[2]) * IU;
    sub[7] = (0.625f * R[1] + 0.375f * R[2]) * IU;
    float mn = sub[0], mx = sub[0], sm = sub[0];
    for (int q = 1; q < 8; q++) { float x = sub[q]; sm += x; if (x < mn) mn = x; if (x > mx) mx = x; }
    *smin = mn; *smax = mx; *smean = sm * 0.125f;
}

/* the wave's shift of the dots at interior cell k (DOT_REFR), in sub-dots */
static inline void wave_shift(const fluid_frame_t *s, int k, float *fx, float *fy) {
    static int w0, h0;                                  /* (4 H / W, the cell's aspect, kept: a float division is a call on the
                                                         * device, and this runs for every cell every frame) */
    static float ay;
    const int W = s->W, H = s->H;
    if (W != w0 || H != h0) { ay = 4.f * H / W; w0 = W; h0 = H; }
    const int16_t *h = s->h;
    const float rk = DOT_REFR * (1.f / FQ_H);
    *fx = (float)(h[k + 1] - h[k - 1]) * rk * 2.f;
    *fy = (float)(h[k + W] - h[k - W]) * rk * ay;
}

/* The braille lattice dots cell (i,j) shows through the water: those under it displaced by
 * the wave slope, as fluid_sim_view refracts the ink, in whole sub-dots and at most a cell
 * (so they only land in the cleared ring near_skip keeps around the strokes). A flat
 * surface leaves every dot exactly on its lattice cell. */
static inline int refracted_dots(const fluid_frame_t *s, const uint8_t *dots, int i, int j, int k) {
    const int W = s->W, H = s->H;
    if (i < 1 || j < 1 || i >= W - 1 || j >= H - 1) return dots[k];
    float fx, fy;
    wave_shift(s, k, &fx, &fy);
    int ox = (int)lrintf(fx), oy = (int)lrintf(fy);
    if (!ox && !oy) return dots[k];
    ox = ox < -2 ? -2 : ox > 2 ? 2 : ox;
    oy = oy < -4 ? -4 : oy > 4 ? 4 : oy;
    int out = 0;
    for (int q = 0; q < 8; q++) {
        const int c = q >> 2, r = q & 3, x = 2 * i + c + ox, y = 4 * j + r + oy;
        if (x < 0 || y < 0 || x >= 2 * W || y >= 4 * H) continue;
        if (dots[(y >> 2) * W + (x >> 1)] >> fluid_braille_bit(x & 1, y & 3) & 1) out |= 1 << fluid_braille_bit(c, r);
    }
    return out;
}

/* how far a scene's dots scatter at cell (i,j), sub-dots: the wave's steepness beyond
 * DUST_CALM, plus a transition's dust; *fx, *fy the wave's shift */
static inline float dust_radius(const fluid_frame_t *s, int i, int j, float dust, float *fx, float *fy) {
    *fx = *fy = 0.f;
    if (i >= 1 && j >= 1 && i < s->W - 1 && j < s->H - 1) wave_shift(s, j * s->W + i, fx, fy);
    const float m2 = *fx * *fx + *fy * *fy;            /* (calm, most cells: no root, a call on the device) */
    const float steep = m2 <= DUST_CALM * DUST_CALM ? 0.f : DUST_GAIN * (sqrtf(m2) - DUST_CALM);
    return (steep < 0.f ? 0.f : steep > DUST_MAX ? DUST_MAX : steep) + DUST_REACH * dust;
}

/* A scene's dots at cell (i,j) as dust: shifted by the wave as refracted_dots, and each dot
 * takes the one a hashed offset away inside the scatter radius (round on the panel); a
 * transition's dust also thins them out (all gone at 1). Below half a sub-dot of scatter and no
 * dust this is refracted_dots: calm water gives exactly the lattice. *kc: the cell the first
 * dot came from (its colour). */
static inline int scene_dots(const fluid_draw_t *d, const fluid_frame_t *s, const uint8_t *dots, int i, int j, int k, float dust, int *kc, bool cull) {
    const int W = s->W, H = s->H;
    float fx, fy;
    const float rad = dust_radius(s, i, j, dust, &fx, &fy), ay = 2.f * H / W;   /* sub-dots are cw/2 x ch/4 */
    *kc = k;
    if (rad < 0.5f && dust <= 0.f) return refracted_dots(s, dots, i, j, k);
    const int ox = fx < -2.f ? -2 : fx > 2.f ? 2 : (int)lrintf(fx), oy = fy < -4.f ? -4 : fy > 4.f ? 4 : (int)lrintf(fy);
    int out = 0;
    if (cull && 2.f * (d->cdist[k] - 1) > rad * (ay > 1.f ? ay : 1.f) + 4.f) return 0;   /* no dot of the drawing reaches it */
    const int thin = (int)(dust * 255.f) - 128;
    const float rx = rad * (1.f / 127.f), ry = rx * ay;
    for (int q = 0; q < 8; q++) {
        const int c = q >> 2, r = q & 3, x0 = 2 * i + c, y0 = 4 * j + r;
        const int8_t *sc = d->scat + 3 * (y0 * 2 * W + x0);    /* its fixed scatter (fluid_draw_create) */
        if (dust > 0.f && sc[2] < thin) continue;
        const float fx = sc[0] * rx, fy = sc[1] * ry;
        const int x = x0 + ox + (int)(fx + (fx < 0.f ? -0.5f : 0.5f)), y = y0 + oy + (int)(fy + (fy < 0.f ? -0.5f : 0.5f));
        if (x < 0 || y < 0 || x >= 2 * W || y >= 4 * H) continue;
        const int ks = (y >> 2) * W + (x >> 1);
        if (dots[ks] >> fluid_braille_bit(x & 1, y & 3) & 1) {
            if (!out) *kc = ks;
            out |= 1 << fluid_braille_bit(c, r);
        }
    }
    return out;
}

/* how much of a scene in high definition is dust at cell (i,j), 0 (calm: its shapes alone) ..
 * 1 (all dust): the wave's shift of its dots beyond HD_CALM, or a transition's dust */
static inline float hd_dust(const fluid_frame_t *s, int i, int j, float dust) {
    float fx = 0.f, fy = 0.f;
    if (i >= 1 && j >= 1 && i < s->W - 1 && j < s->H - 1) wave_shift(s, j * s->W + i, &fx, &fy);
    const float m = fmaxf((sqrtf(fx * fx + fy * fy) - HD_CALM) / HD_FADE, dust * HD_DUST);
    return m < 0.f ? 0.f : m > 1.f ? 1.f : m;
}

/* on fine dust, the water breaks the drawing at cell (i,j) into its dots: where it shifts them FINE_BREAK sub-dots or
 * more (fluid_sim.h) */
static inline bool fine_breaks(const fluid_frame_t *s, int i, int j) {
    if (i < 1 || j < 1 || i >= s->W - 1 || j >= s->H - 1) return false;
    float fx, fy;
    wave_shift(s, j * s->W + i, &fx, &fy);
    return fx * fx + fy * fy >= FINE_BREAK * FINE_BREAK;
}

/* A transition's dust, pushed: each dot of the drawing (o->dots) to where it scatters, as
 * scene_dots would pull it (the wave's shift, a fixed offset inside the scatter radius, thinned
 * by the dust), into d->pdots / d->psrc: the work follows the drawing's dots, not the lattice */
static void dust_push(fluid_draw_t *d, const fluid_frame_t *s, const fluid_draw_opts_t *o) {
    const int W = s->W, H = s->H, N = W * H, thin = (int)(o->dust * 255.f) - 128;
    const float ay = 2.f * H / W;
    memset(d->pdots, 0, (size_t)N);
    for (int k = 0; k < N; k++) {
        if (!o->dots[k] || !s->mask[k]) continue;
        const int i = k % W, j = k / W;
        float fx, fy;
        const float rad = dust_radius(s, i, j, o->dust, &fx, &fy), rx = rad * (1.f / 127.f), ry = rx * ay;
        const int ox = fx < -2.f ? -2 : fx > 2.f ? 2 : (int)lrintf(fx), oy = fy < -4.f ? -4 : fy > 4.f ? 4 : (int)lrintf(fy);
        for (int q = 0; q < 8; q++) {
            const int c = q >> 2, r = q & 3;
            if (!(o->dots[k] >> fluid_braille_bit(c, r) & 1)) continue;
            const int x0 = 2 * i + c, y0 = 4 * j + r;
            const int8_t *sc = d->scat + 3 * (y0 * 2 * W + x0);
            if (sc[2] < thin) continue;
            const float gx = sc[0] * rx, gy = sc[1] * ry;
            const int x = x0 - ox - (int)(gx + (gx < 0.f ? -0.5f : 0.5f)), y = y0 - oy - (int)(gy + (gy < 0.f ? -0.5f : 0.5f));
            if (x < 0 || y < 0 || x >= 2 * W || y >= 4 * H) continue;
            const int kd = (y >> 2) * W + (x >> 1);
            if (!d->pdots[kd]) d->psrc[kd] = (int16_t)k;
            d->pdots[kd] |= (uint8_t)(1 << fluid_braille_bit(x & 1, y & 3));
        }
    }
}

/* ── EXPERIMENT scene dust (fluid_fine_dust_kind FINE_DUST_DOTS; delete this block, sd_* in fluid_draw_t and its
 * calls once Juan picks). A scene on fine dust shows the creature screen's kind of dust instead of single-pixel specks:
 * braille dots on the water's own cells (fixed, made once per drawing), seen through the water exactly as the
 * creature's (scene_dots: shifted by the wave's slope, scattered where it is steep), so a tap anywhere sends the same
 * wave through them. They gather to the drawing: denser and on an ordered lattice near it, sparse and loose far away;
 * brighter near it. Never on the drawing (drawn beside its shapes only). ── */
#define SD_AMBIENT    0.05f   /* a dot's chance far from the drawing ..                                    */
#define SD_PEAK       0.6f    /* .. plus this much at the drawing, falling off over                       */
#define SD_FALL       9.f     /* .. this many px                                                          */
#define SD_ON         1.5f    /* none closer than this to a drawn dot, px                                 */
#define SD_LEVEL_FAR  0.42f   /* brightness far .. 1 near                                                 */
#define SD_REACH      240     /* the distance's cap, 1/4 px (60 px: ambient beyond)                       */
#define SD3_CLEAR     4.f     /* kind 3: no hard dot closer than this to a drawn dot, px                   */
#define SD3_LEVEL     0.9f    /* kind 3: the hard dots' brightness (far dust: SD_LEVEL_FAR; the creature's motes are bright) */

/* the dust's colour at nearness `near` (0..1) to the drawing, brightness times k */
static uint16_t sd_rgb(int pal, float near, float k) {
    const float lv = (SD_LEVEL_FAR + (1.f - SD_LEVEL_FAR) * near) * k;
    if (pal == FLUID_PAL_PASTEL)
        return rgb565_be((int)((FINE_WATER_R + (235 - FINE_WATER_R) * 0.45f * near) * lv), (int)((FINE_WATER_G + (230 - FINE_WATER_G) * 0.45f * near) * lv),
                         (int)((FINE_WATER_B + (255 - FINE_WATER_B) * 0.45f * near) * lv));
    const float *lo = PAL[pal][0], *hi = PAL[pal][1];
    return rgb565_be((int)(lo[0] + (hi[0] - lo[0]) * lv * 0.8f), (int)(lo[1] + (hi[1] - lo[1]) * lv * 0.8f), (int)(lo[2] + (hi[2] - lo[2]) * lv * 0.8f));
}

/* each dot's distance to the drawing's dots and the dust's dots and colour by it: again only when the drawing
 * changes; the cells whose dust changed are marked (d->hch). False: no memory. */
static bool sd_field(fluid_draw_t *d, const uint8_t *dots, int palette, int kind) {
    const int W = d->W, H = d->H, DW = 2 * W, DH = 4 * H, N = W * H;
    if (!d->sd_dist) {
        d->sd_dist = FLUID_CALLOC(sizeof(uint16_t) * 11 * (size_t)N + 6 * (size_t)N);
        if (!d->sd_dist) return false;
        d->sd_col = d->sd_dist + 8 * N; d->sd_lcol = d->sd_col + N; d->sd_hcol = d->sd_lcol + N; d->sd_dots = (uint8_t *)(d->sd_hcol + N);
        d->sd_live = d->sd_dots + N; d->sd_near = d->sd_live + N; d->sd_ch = d->sd_near + N; d->sd_pm = d->sd_ch + N; d->sd_hard = d->sd_pm + N;
    }
    /* (kind 3: a sparse even layer, its density the setter's, none within SD3_CLEAR px of the drawing) */
    const bool hard = kind == FINE_DUST_FINE_DOTS;
    const float pct = hard ? fluid_fine_hard_density_get() * 0.01f : 0.f;
    uint32_t sig = (2166136261u ^ (uint32_t)palette) * 16777619u ^ (uint32_t)(hard ? 1000 + fluid_fine_hard_density_get() : 0);
    for (int k = 0; k < N; k++) sig = (sig ^ dots[k]) * 16777619u;
    if (sig == d->sd_sig) return true;
    d->sd_sig = sig;
    uint16_t *f = d->sd_dist;                           /* chamfer, 1/4 px, the dots' own pitch either way */
    const int ax = (int)lrintf(4.f * d->stage / DW), ay = (int)lrintf(4.f * d->stage / DH), ad = (int)lrintf(sqrtf((float)(ax * ax + ay * ay)));
    for (int y = 0; y < DH; y++) for (int x = 0; x < DW; x++)
        f[y * DW + x] = dots[(y >> 2) * W + (x >> 1)] >> fluid_braille_bit(x & 1, y & 3) & 1 ? 0 : SD_REACH;
#define SD_MIN(q, w) do { const int v_ = (q) + (w); if (v_ < *p) *p = (uint16_t)v_; } while (0)
    for (int y = 0; y < DH; y++) for (int x = 0; x < DW; x++) {
        uint16_t *p = &f[y * DW + x];
        if (x > 0) SD_MIN(p[-1], ax);
        if (y > 0) { SD_MIN(p[-DW], ay); if (x > 0) SD_MIN(p[-DW - 1], ad); if (x < DW - 1) SD_MIN(p[-DW + 1], ad); }
    }
    for (int y = DH - 1; y >= 0; y--) for (int x = DW - 1; x >= 0; x--) {
        uint16_t *p = &f[y * DW + x];
        if (x < DW - 1) SD_MIN(p[1], ax);
        if (y < DH - 1) { SD_MIN(p[DW], ay); if (x > 0) SD_MIN(p[DW - 1], ad); if (x < DW - 1) SD_MIN(p[DW + 1], ad); }
    }
#undef SD_MIN
    static float near_lut[SD_REACH + 1];
    if (near_lut[0] != 1.f) for (int q = 0; q <= SD_REACH; q++) near_lut[q] = q >= SD_REACH ? 0.f : expf(-q * 0.25f / SD_FALL);
    const int pal = (palette >= 0 && palette < FLUID_PAL_COUNT) ? palette : FLUID_PAL_PASTEL;
    for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) {
        const int k = j * W + i;
        int on = 0; float most = -1.f;
        for (int q = 0; q < 8; q++) {
            const int x = 2 * i + (q >> 2), y = 4 * j + (q & 3), dq = f[y * DW + x];
            if (dq < (int)((hard ? SD3_CLEAR : SD_ON) * 4.f)) continue;
            const float near = near_lut[dq > SD_REACH ? SD_REACH : dq];
            if (hard) {                                 /* the creature screen's motes: picked once per dot by a hash */
                if (hash_u(x, y, 0x3D) >= pct) continue;
                on |= 1 << fluid_braille_bit(q >> 2, q & 3);
                if (near > most) most = near;
                continue;
            }
            /* ordered near the drawing (the lattice's Bayer), loose far from it (a hash) */
            const float thr = near * BAYER4[(y & 3) * 4 + (x & 3)] + (1.f - near) * hash_u(x, y, 0x5D);
            if (thr >= SD_AMBIENT + SD_PEAK * near) continue;
            on |= 1 << fluid_braille_bit(q >> 2, q & 3);
            if (near > most) most = near;
        }
        const uint16_t col = on ? sd_rgb(pal, most, hard ? SD3_LEVEL / SD_LEVEL_FAR : 1.f) : 0;
        if (on != d->sd_dots[k] || col != d->sd_col[k]) { d->sd_dots[k] = (uint8_t)on; d->sd_col[k] = col; d->hch[k] = 1; }
        d->sd_near[k] = (uint8_t)(most > 0.f ? most * 255.f + 0.5f : 0.f);
    }
    return true;
}

/* The dust alive at rest. The creature screen's own dust only twinkles at rest (creature_dots.c: each mote's
 * brightness drawn again every TWINKLE frames, its row's moment scattered, so a few rows a frame); its water is flat.
 * Scenes take that twinkle on the cells' colour (the far dust more, the dust by the drawing hardly) and the chart's
 * calm look Juan approved for the specks, the light on the water (fluid_fine.h: thin bright ridges where two slowly
 * warping wave fronts cross) on the dots' density: the far dust thins where the light is dark, so slow soft fronts of
 * dots come and go through it; by the drawing they always show. Both hold still while the picture changes. Each frame
 * the cells whose dots or colour changed are marked (d->hch). */
#define SD_WIN_PX     170     /* a window's cost in pixels (the device: ~0.15 ms against ~0.9 us a pixel): the dots' cells go in
                               * runs split at gaps that cost more                                            */
#define SD_FRAME_CELLS 110    /* changed cells a frame sends at most (about 18 ms on the panel); what the drawing and the words
                               * do not take, the water, the light and the twinkle may (at least SD_WATER_MIN)  */
#define SD_WATER_MIN  40
#define SD_ILACE_MAX  6      /* ... the water's rows taking turns over this many frames at most            */
#define SD_TWINKLE    24      /* frames between a cell's twinkles (creature_dots.c TWINKLE)                 */
#define SD_TWINKLE_A  0.45f   /* how much the far dust's brightness twinkles                               */
static void sd_alive(fluid_draw_t *d, const fluid_draw_opts_t *o) {
    const int W = d->W, H = d->H;
    const fluid_fine_t *f = o->fine;
    if (!f->pause) { const float dt = o->time - d->sd_t0; d->sd_t += dt < 0.f ? 0.f : dt > 0.1f ? 0.1f : dt; }
    d->sd_t0 = o->time;
    const uint32_t fr = (uint32_t)(d->sd_t * 30.f);
    /* the cells a word's specks or its forming are in, this frame and the last: the backdrop's own changes go there and
     * by the drawing (its dim); elsewhere it only thins the dust, which is the dust's (sd_cell) */
    for (int k = 0; k < W * H; k++) d->sd_pm[k] = (uint8_t)((d->sd_pm[k] & 1) << 1 | (f->imm[k] != 0));
    for (int q = 0; q < f->npd; q++) if (f->ppx[q] >= 0) d->sd_pm[f->celly[f->ppy[q]] * W + f->cellx[f->ppx[q]]] |= 1;
    const int pal = (o->palette >= 0 && o->palette < FLUID_PAL_COUNT) ? o->palette : FLUID_PAL_PASTEL;
    for (int j = 0; j < H; j++) {
        const uint32_t ph = (fr + (uint32_t)(((j * 11) % H) * SD_TWINKLE / H)) / SD_TWINKLE;
        for (int i = 0; i < W; i++) {
            const int k = j * W + i, on = d->sd_dots[k];
            int live = 0; uint16_t col = 0;
            if (on) {
                for (int q = 0; q < 8; q++) {
                    const int c = q >> 2, r = q & 3, bit = 1 << fluid_braille_bit(c, r);
                    if (!(on & bit)) continue;
                    const int x = 2 * i + c, y = 4 * j + r, dq = d->sd_dist[y * 2 * W + x];
                    const float near = dq >= SD_REACH ? 0.f : expf(-dq * 0.25f / SD_FALL);
                    const float l = f->lgrid_ok && d->sd_kind != FINE_DUST_FINE_DOTS ? fluid_fine_light(f, d->dx[x] + d->dot_w * 0.5f - d->ox, d->dy[y] + d->dot_h * 0.5f - d->oy) : 1.f;
                    if (hash_u(x, y, 0x71) < FINE_LIGHT_FLOOR + (1.f - FINE_LIGHT_FLOOR) * (l + (1.f - l) * near)) live |= bit;
                }
                const float near = d->sd_near[k] * (1.f / 255.f), tw = hash_u(i, j, ph);
                if (live) col = sd_rgb(pal, near, 1.f - SD_TWINKLE_A * (1.f - near) * (1.f - tw));
            }
            d->sd_ch[k] = 0;
            if (live != d->sd_live[k] || col != d->sd_lcol[k]) { d->sd_live[k] = (uint8_t)live; d->sd_lcol[k] = col; d->sd_ch[k] = !d->hch[k]; d->hch[k] = 1; }
        }
    }
}

/* the dust's dots cell (i, j) shows: through the water as the creature's (scene_dots), thinned round the caption's
 * backdrop as the specks are (fluid_fine_frame's f->dim: its box, from and to how far); *kc the cell they came from */
static int sd_cell(const fluid_draw_t *d, const fluid_frame_t *s, const fluid_draw_opts_t *o, int i, int j, int k, int *kc) {
    int dots = scene_dots(d, s, d->sd_live, i, j, k, 0.f, kc, false);
    const float *dm = o->fine->dim, reach = FINE_PATCH1 + FINE_PATCH_RAG * 0.5f;
    if (!dots || dm[4] <= 0.f) return dots;
    for (int q = 0; q < 8; q++) {
        const int c = q >> 2, r = q & 3, bit = 1 << fluid_braille_bit(c, r);
        if (!(dots & bit)) continue;
        const float x = d->dx[2 * i + c] + d->dot_w * 0.5f - d->ox, y = d->dy[4 * j + r] + d->dot_h * 0.5f - d->oy;
        const float ex = x < dm[0] ? dm[0] - x : x > dm[1] ? x - dm[1] : 0.f, ey = y < dm[2] ? dm[2] - y : y > dm[3] ? y - dm[3] : 0.f;
        if (ex > reach || ey > reach) continue;
        const float dd = fsqrt(ex * ex + ey * ey);
        if (dd < dm[5] || dd >= dm[6]) continue;
        if (fluid_smooth(FINE_PATCH0, FINE_PATCH1, dd + (hash_u(2 * i + c, 4 * j + r, 5) - 0.5f) * FINE_PATCH_RAG) < 0.5f) dots &= ~bit;
    }
    return dots;
}

/* the dust's reseed for cell row j: each row at its own moment, HAZE_HZ times a second (never 255) */
static inline uint32_t haze_seed(const fluid_draw_opts_t *o, int j) {
    return (uint32_t)floorf(o->time * HAZE_HZ + hash_u(j, 0, 7)) % 255u;
}

/* (key, colour) of one cell — what render.js would draw there */
static inline void cell_look(const fluid_draw_t *d, const fluid_frame_t *s, const fluid_draw_opts_t *o,
                             int i, int j, int k, int rmax, uint32_t frame, uint16_t *key_out, uint16_t *col_out, uint8_t *hard_out, uint16_t *hcol_out) {
    const int W = s->W;
    const int mode = o->edge_mode;
    float v = s->view[k] * IU;
    bool feat = s->feat[k] != 0;
    int dots = -1, kc = k;                               /* kc: the cell whose colour it takes */
    float smax = 0.f, cell_mean = -1.f;
    uint16_t hd = 0;                                     /* KEY_HD | weight << 8: its shapes */
    if (o->scene && o->dots) {
        /* a scene in high definition: its dots alone on black, at one brightness; with its
         * shapes, those while calm (and only the dots no shape draws), fading into the dust */
        const uint8_t *src = o->dots;
        float dust = o->dust;                           /* a transition's scatter */
        if (o->shapes) {
            if (o->fine) dust = 0.f;                    /* (on fine dust a change of scene is the drawer's: fluid_morph_t) */
            /* smooth or dust, no fade: a cell changes once. Where the water moves, the drawing breaks into its dots and
             * they ride it ("lines wobble with water"), whole again on the first calm frame (a broken cell is looked at
             * every frame: never skipped while not smooth). Never: a tapped spot's drawing (o->still), the caption and
             * its backdrop (d->hold), under a ring on the rim (a listen) and while the picture changes. */
            const bool held = (o->still && o->still[k]) || (o->fine && o->fine->rim) || d->m.on ||
                              (i >= d->hold[0] && i <= d->hold[1] && j >= d->hold[2] && j <= d->hold[3]);
            const int wt = (held && dust <= 0.f) || (o->fine ? !fine_breaks(s, i, j) : hd_dust(s, i, j, dust) < 0.5f) ? HD_LEVELS : 0;
            if (wt) hd = (uint16_t)(KEY_HD | wt << 8);
            if (wt == HD_LEVELS) src = o->kdots;
        }
        if (o->shapes && src == o->dots && dust > 0.f) { dots = d->pdots[k]; kc = dots ? d->psrc[k] : k; }   /* dust_push */
        else dots = src ? scene_dots(d, s, src, i, j, k, dust, &kc, true) : 0;
        if (o->fine && o->shapes && !hd && !dots && !o->dots[k]) {   /* (broken, but nothing of the drawing is in it or rides
                                                                      * into it: as it was, not sent) */
            hd = (uint16_t)(KEY_HD | HD_LEVELS << 8);
            dots = o->kdots ? scene_dots(d, s, o->kdots, i, j, k, 0.f, &kc, true) : 0;
        }
        if (o->fine && o->shapes && d->sd_kind == FINE_DUST_FINE_DOTS) {   /* EXPERIMENT kind 3: the hard dots apart, drawn on
                                                                             * black after the specks (the caller adds them to the key) */
            int kd = k;
            const int sd = sd_cell(d, s, o, i, j, k, &kd) & ~(dots > 0 ? dots : 0);
            if (sd) { *hard_out = (uint8_t)sd; *hcol_out = d->sd_lcol[d->sd_lcol[k] ? k : kd]; }
        } else if (o->fine && o->shapes && d->sd_kind) {   /* EXPERIMENT: the creature's kind of dust (sd_cell) */
            int kd = k;
            const int sd = sd_cell(d, s, o, i, j, k, &kd);
            if (sd && !dots) { *key_out = (uint16_t)(hd | sd); *col_out = d->sd_lcol[d->sd_lcol[k] ? k : kd]; return; }   /* (its own colour: a dot riding in takes it) */
            dots |= sd;                                 /* (beside a ball's or an image's dots: their colour) */
        }
        if (!dots && o->shapes && !o->fine && o->dust <= 0.f && i >= 1 && j >= 1 && i < W - 1 && j < s->H - 1) {
            /* around the drawing (not while it breaks into dust or forms: the effect stays light), a faint dust, denser and brighter close to it (d->halo), never on
             * it, thinning to dark round the subtitle (d->cap), reseeded slowly (haze_seed) */
            float sub[8];
            int near = HALO_REACH + 1, on = 0;
            for (int q = 0; q < 8; q++) {
                const int x = 2 * i + (q >> 2), y = 4 * j + (q & 3), dd = d->halo[y * 2 * W + x];
                sub[q] = HAZE_AMBIENT + (dd <= HALO_REACH ? HALO_INK[dd] : 0.f);
                if (d->cap[0] <= d->cap[1]) {
                    const int e = x < d->cap[0] ? d->cap[0] - x : x > d->cap[1] ? x - d->cap[1] : 0, f = y < d->cap[2] ? d->cap[2] - y : y > d->cap[3] ? y - d->cap[3] : 0;
                    sub[q] *= fluid_clamp01(((float)(e > f ? e : f) - 1.f) / CAP_FADE);
                }
                if (dd) on |= 1 << fluid_braille_bit(q >> 2, q & 3);
                if (dd < near) near = dd;
            }
            const int hz = fluid_braille_dither(sub, i, j, HAZE_JITTER, haze_seed(o, j)) & on;
            if (hz) {
                const int pal = (o->palette >= 0 && o->palette < FLUID_PAL_COUNT) ? o->palette : FLUID_PAL_PASTEL;
                const float lv = near <= HALO_REACH ? HALO_LEVEL[near] : HAZE_LEVEL;
                const float *lo = PAL[pal][0], *hi = PAL[pal][1], f = lv * (255.f / 65535.f);
                int kc2 = k, most = 0;                       /* near it: the drawing's colour */
                for (int q = 0; near <= HALO_REACH && q < 9; q++) {
                    const int kn = k + (q / 3 - 1) * W + q % 3 - 1, n = __builtin_popcount(o->dots[kn]);
                    if (n > most) { most = n; kc2 = kn; }
                }
                const fluid_rgba16_t *c = &s->col[kc2];
                *col_out = pal == FLUID_PAL_PASTEL ? rgb565_be((int)(c->r * f), (int)(c->g * f), (int)(c->b * f))
                         : rgb565_be((int)(lo[0] + (hi[0] - lo[0]) * lv), (int)(lo[1] + (hi[1] - lo[1]) * lv), (int)(lo[2] + (hi[2] - lo[2]) * lv));
                *key_out = (uint16_t)(hd | hz);
                return;
            }
        }
        if (!dots) { *key_out = hd; *col_out = 0; return; }
        v = SCENE_LEVEL; feat = false;
    } else if (o->dots) {
        /* a word's dots (fluid_text_set_glyphs): each one shows once the ink has reached it,
         * seen through the water (refracted_dots) */
        const int own = o->dots[k], src = refracted_dots(s, o->dots, i, j, k);
        if (src) {
            float sub[8], smin, smean;
            subsample(s->view, W, k, sub, &smin, &smax, &smean);
            int wd = 0;
            for (int q = 0; q < 8; q++)
                if (sub[q] > WORD_INK && (src >> fluid_braille_bit(q >> 2, q & 3)) & 1) wd |= 1 << fluid_braille_bit(q >> 2, q & 3);
            if (wd) dots = wd;
        }
        if (!own && dots < 0 && near_skip(o->dots, W, s->H, i, j)) {
            *key_out = KEY_EMPTY; *col_out = 0; return;  /* clear around the word's strokes */
        }
    }
    if (dots >= 0) {
        /* the word's dots */
    } else if (feat) {
        /* eyes / mouth: dark in the creature but must read on black — a solid block */
        if (mode != FLUID_EDGE_OFF) dots = 0xFF;
    } else if (mode != FLUID_EDGE_OFF) {
        float sub[8], smin, smean;
        subsample(s->view, W, k, sub, &smin, &smax, &smean);
        if (smax - smin > EDGE_RANGE && smax >= EDGE_MIN && !near_skip(d->skip, W, s->H, i, j)) {
            dots = fluid_braille_contour(sub);
        } else if (mode == FLUID_EDGE_ALL || smax < 0.3f) {
            dots = fluid_braille_dither(sub, i, j, o->shimmer, frame);
            cell_mean = smean;
        }
    }
    int glyph = -1;
    if (dots < 0 && feat && o->font) {
        bool l = i > 0 && s->feat[k - 1], r = i < W - 1 && s->feat[k + 1];
        glyph = (l && r) ? '@' : 'o';
    } else if (dots < 0) {
        int idx = (int)(v * rmax + 0.5f);
        if (idx > 0 && o->font) {
            glyph = (unsigned char)o->ramp[idx > rmax ? rmax : idx];
            if (mode == FLUID_EDGE_OFF && s->stroke[k]) glyph = 0x80 | (s->stroke[k] - 1);
        }
    }
    if (dots <= 0 && glyph < 0) { *key_out = KEY_EMPTY; *col_out = 0; return; }
    const int pal = (o->palette >= 0 && o->palette < FLUID_PAL_COUNT) ? o->palette : FLUID_PAL_PASTEL;
    const float *lo = PAL[pal][0], *hi = PAL[pal][1], *ac = PAL[pal][2];
    float vb = cell_mean >= 0.f ? DIM_FLOOR + (1.f - DIM_FLOOR) * fminf(1.f, cell_mean * 1.6f)
             : dots >= 0 ? fmaxf(v, smax) : v;
    float br = cell_mean >= 0.f ? vb : 0.25f + 0.75f * vb;
    float r, g, b;
    const fluid_rgba16_t *c = &s->col[kc];
    if (pal != FLUID_PAL_PASTEL) { r = lo[0] + (hi[0] - lo[0]) * br; g = lo[1] + (hi[1] - lo[1]) * br; b = lo[2] + (hi[2] - lo[2]) * br; }
    else {
        float bt = (cell_mean >= 0.f ? vb * 1.1f : 0.3f + 0.85f * vb) * (255.f / 65535.f);
        r = c->r * bt; g = c->g * bt; b = c->b * bt;
    }
    if (feat) { if (pal != FLUID_PAL_PASTEL) { r = hi[0]; g = hi[1]; b = hi[2]; } else { r = 250; g = 246; b = 255; } }
    float a = c->a * (1.f / 65535.f);
    if (a > 0.02f) { r += (ac[0] - r) * a; g += (ac[1] - g) * a; b += (ac[2] - b) * a; }
    *col_out = rgb565_be((int)r, (int)g, (int)b);
    *key_out = dots > 0 ? (uint16_t)(hd | dots) : (uint16_t)(KEY_GLYPH | glyph);
}

/* one pixel row of a glyph / stroke cell (x0..x1, y0..y1); row[0] is fb x = bx, written
 * only inside [rx0, rx1) */
static inline void raster_glyph_row(uint16_t *row, int bx, int rx0, int rx1, int y, int x0, int x1, int y0, int y1,
                                    uint16_t key, uint16_t col, const fluid_draw_opts_t *o) {
    int cwp = x1 - x0, chp = y1 - y0;
    int gw = cwp - 2 < 5 ? (cwp < 5 ? cwp : 5) : cwp - 2, gh = chp - 3 < 7 ? chp : chp - 3;
    int gx = x0 + (cwp - gw) / 2, gy = y0 + (chp - gh) / 2;
    int py = y - gy;
    if (py < 0 || py >= gh || gw <= 0) return;
    int gch = key & 0xFF;
    if (gch & 0x80) {                     /* streamline: 0 '-', 1 '\', 2 '|', 3 '/' */
        int x;
        switch (gch & 3) {
        case 0:  if (py != gh / 2) return; for (x = gx; x < gx + gw; x++) if (x >= rx0 && x < rx1) row[x - bx] = col; return;
        case 1:  x = gx + py * gw / gh; break;
        case 2:  x = gx + gw / 2; break;
        default: x = gx + gw - 1 - py * gw / gh; break;
        }
        if (x >= x0 && x < x1 && x >= rx0 && x < rx1) row[x - bx] = col;
        return;
    }
    const uint8_t *gl = o->font((char)gch);
    uint8_t bits = gl ? gl[py * 7 / gh] : 0;
    if (!bits) return;
    for (int px = 0; px < gw; px++)
        if ((bits >> (4 - px * 5 / gw)) & 1 && gx + px >= rx0 && gx + px < rx1) row[gx + px - bx] = col;
}

/* a cell's shapes, as far as its dots and its neighbours' tell (a stroke's soft edge may
 * reach into the next cell) */
static inline uint8_t hd_sig(const uint8_t *dots, int W, int H, int i, int j) {
    const int k = j * W + i;
    return (uint8_t)(1 + dots[k] + 3 * (i > 0 ? dots[k - 1] : 0) + 5 * (i < W - 1 ? dots[k + 1] : 0) +
                     7 * (j > 0 ? dots[k - W] : 0) + 11 * (j < H - 1 ? dots[k + W] : 0));
}

/* each dot's distance to the drawing's dots (chessboard, in dots, capped past HALO_REACH):
 * measured again only when the drawing changes, and the cells where it changed marked (d->hch) */
static void halo_field(fluid_draw_t *d, const uint8_t *dots, bool cells_only) {
    const int W = d->W, H = d->H, DW = 2 * W, DH = 4 * H, FAR = HALO_REACH + 1;
    uint32_t sig = 2166136261u ^ (uint32_t)cells_only;
    for (int k = 0; k < W * H; k++) sig = (sig ^ dots[k]) * 16777619u;
    if (sig == d->halo_sig) return;
    const bool all = !d->halo_sig;
    d->halo_sig = sig;
    uint8_t *f = d->halo1;
    if (!cells_only) {                                  /* the haze's field (a scene on fine dust has none) */
    for (int y = 0; y < DH; y++) for (int x = 0; x < DW; x++)
        f[y * DW + x] = dots[(y >> 2) * W + (x >> 1)] >> fluid_braille_bit(x & 1, y & 3) & 1 ? 0 : FAR;
    for (int y = 0; y < DH; y++) for (int x = 0; x < DW; x++) {          /* down, then up */
        uint8_t *p = &f[y * DW + x];
        if (x > 0 && p[-1] + 1 < *p) *p = (uint8_t)(p[-1] + 1);
        if (y > 0) for (int dx = -1; dx <= 1; dx++) if (x + dx >= 0 && x + dx < DW && p[dx - DW] + 1 < *p) *p = (uint8_t)(p[dx - DW] + 1);
    }
    for (int y = DH - 1; y >= 0; y--) for (int x = DW - 1; x >= 0; x--) {
        uint8_t *p = &f[y * DW + x];
        if (x < DW - 1 && p[1] + 1 < *p) *p = (uint8_t)(p[1] + 1);
        if (y < DH - 1) for (int dx = -1; dx <= 1; dx++) if (x + dx >= 0 && x + dx < DW && p[dx + DW] + 1 < *p) *p = (uint8_t)(p[dx + DW] + 1);
    }
    for (int y = 0; y < DH; y++) for (int x = 0; x < DW; x++)
        if (all || f[y * DW + x] != d->halo[y * DW + x]) d->hch[(y >> 2) * W + (x >> 1)] = 1;
    d->halo1 = d->halo; d->halo = f;
    }
    /* and in cells, chessboard, capped at 255 */
    uint8_t *c = d->cdist;
    for (int k = 0; k < W * H; k++) c[k] = dots[k] ? 0 : 255;
    for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) {
        uint8_t *p = &c[j * W + i];
        if (i > 0 && p[-1] < 255 && p[-1] + 1 < *p) *p = (uint8_t)(p[-1] + 1);
        for (int di = -1; j > 0 && di <= 1; di++) if (i + di >= 0 && i + di < W && p[di - W] < 255 && p[di - W] + 1 < *p) *p = (uint8_t)(p[di - W] + 1);
    }
    for (int j = H - 1; j >= 0; j--) for (int i = W - 1; i >= 0; i--) {
        uint8_t *p = &c[j * W + i];
        if (i < W - 1 && p[1] < 255 && p[1] + 1 < *p) *p = (uint8_t)(p[1] + 1);
        for (int di = -1; j < H - 1 && di <= 1; di++) if (i + di >= 0 && i + di < W && p[di + W] < 255 && p[di + W] + 1 < *p) *p = (uint8_t)(p[di + W] + 1);
    }
}

/* the subtitle's box in dots (its letters: the palette's, not a symbol), empty when none; when it
 * moves, the cells its dark reaches, before and after, marked (d->hch) */
static void caption_box(fluid_draw_t *d, const fluid_draw_opts_t *o) {
    const float px = (float)d->stage / (2 * d->W), py = (float)d->stage / (4 * d->H);
    int16_t b[4] = { 1, 0, 0, 0 };
    for (int n = 0; n < o->nletters; n++) {
        const fluid_letter_t *L = &o->letters[n];
        if (L->mask || (L->rgb & FLUID_RGB) || L->ch == ' ') continue;
        const int16_t x0 = (int16_t)floorf(L->x / px), x1 = (int16_t)ceilf((L->x + L->w) / px) - 1;
        const int16_t y0 = (int16_t)floorf(L->y / py), y1 = (int16_t)ceilf((L->y + L->h) / py) - 1;
        if (b[0] > b[1]) { b[0] = x0; b[1] = x1; b[2] = y0; b[3] = y1; continue; }
        if (x0 < b[0]) b[0] = x0;
        if (x1 > b[1]) b[1] = x1;
        if (y0 < b[2]) b[2] = y0;
        if (y1 > b[3]) b[3] = y1;
    }
    memcpy(d->cap, b, sizeof(b));
    if (!memcmp(d->cap, d->cap0, sizeof(b))) return;
    const int16_t *bs[2] = { d->cap, d->cap0 }, reach = (int16_t)CAP_FADE + 2;
    for (int q = 0; q < 2; q++) {
        if (bs[q][0] > bs[q][1]) continue;
        for (int j = (bs[q][2] - reach) / 4; j <= (bs[q][3] + reach) / 4; j++) for (int i = (bs[q][0] - reach) / 2; i <= (bs[q][1] + reach) / 2; i++)
            if (i >= 0 && j >= 0 && i < d->W && j < d->H) d->hch[j * d->W + i] = 1;
    }
    memcpy(d->cap0, d->cap, sizeof(b));
}

/* the cells the water never breaks (cell_look's held): the caption's letters and, while it is up, its backdrop and the
 * drawing it dims (fluid_shapes_row_dim: FLUID_DIM_REACH px round its box), a cell more each way */
static void hold_box(fluid_draw_t *d, const fluid_draw_opts_t *o) {
    float b[4] = { 1e9f, -1e9f, 1e9f, -1e9f };              /* fb px */
    for (int n = 0; n < o->nshapes && n < FLUID_MAX_SHAPES; n++) {
        if (o->shapes[n].tag != FLUID_TAG_CAPTION) continue;
        b[0] = fminf(b[0], d->sb[n][0]); b[1] = fmaxf(b[1], d->sb[n][1]); b[2] = fminf(b[2], d->sb[n][2]); b[3] = fmaxf(b[3], d->sb[n][3]);
    }
    const float *dm = o->fine ? o->fine->dim : NULL;
    if (dm && dm[4] > 0.f) {
        const float R = FLUID_DIM_REACH;
        b[0] = fminf(b[0], dm[0] - R + d->ox); b[1] = fmaxf(b[1], dm[1] + R + d->ox); b[2] = fminf(b[2], dm[2] - R + d->oy); b[3] = fmaxf(b[3], dm[3] + R + d->oy);
    }
    if (b[0] > b[1]) { d->hold[0] = d->hold[2] = 1; d->hold[1] = d->hold[3] = 0; return; }
    const float kx = (float)d->W / d->stage, ky = (float)d->H / d->stage;
    d->hold[0] = (int16_t)(floorf((b[0] - d->ox) * kx) - 1); d->hold[1] = (int16_t)(floorf((b[1] - d->ox) * kx) + 1);
    d->hold[2] = (int16_t)(floorf((b[2] - d->oy) * ky) - 1); d->hold[3] = (int16_t)(floorf((b[3] - d->oy) * ky) + 1);
}

/* crisp dots alone (a scene's dots, no shapes, no dust: a rigged creature): the wave leaves cell (i, j) showing its own
 * dots (refracted_dots: no whole sub-dot of shift, so no scatter either) */
static inline bool crisp_flat(const fluid_frame_t *s, int i, int j, int k) {
    if (i < 1 || j < 1 || i >= s->W - 1 || j >= s->H - 1) return true;
    float fx, fy;
    wave_shift(s, k, &fx, &fy);
    return !lrintf(fx) && !lrintf(fy);
}

/* a calm cell of a scene in high definition that can show what it showed: the wave keeps it
 * smooth and moves no dot it keeps (none near, or the shift under half a dot) */
static inline bool hd_still(const fluid_frame_t *s, const fluid_draw_opts_t *o, int i, int j, int k) {
    const int W = s->W;
    if (i < 1 || j < 1 || i >= W - 1 || j >= s->H - 1) return false;
    float fx, fy;
    wave_shift(s, k, &fx, &fy);
    const float m2 = fx * fx + fy * fy;
    if (m2 < 0.25f) return true;
    if (m2 >= (HD_CALM + 0.5f * HD_FADE) * (HD_CALM + 0.5f * HD_FADE)) return false;   /* (hd_dust >= 0.5, without its root) */
    for (int q = 0; o->kdots && q < 9; q++) if (o->kdots[k + (q / 3 - 1) * W + q % 3 - 1]) return false;
    return true;
}

/* the cells under fb box b, redrawn next (KEY_STALE), marked as s_stale's (a shape's change: the growth's, a word's) */
static uint8_t s_stale = FLUID_ANIM_GROW;
static void stale_box(fluid_draw_t *d, const int16_t *b) {
    const float kx = (float)d->W / d->stage, ky = (float)d->H / d->stage;
    int i0 = (int)floorf((b[0] - d->ox) * kx), i1 = (int)ceilf((b[1] - d->ox) * kx), j0 = (int)floorf((b[2] - d->oy) * ky), j1 = (int)ceilf((b[3] - d->oy) * ky);
    i0 = i0 < 0 ? 0 : i0; j0 = j0 < 0 ? 0 : j0; i1 = i1 > d->W ? d->W : i1; j1 = j1 > d->H ? d->H : j1;
    for (int j = j0; j < j1; j++) for (int i = i0; i < i1; i++) { d->key[j * d->W + i] = KEY_STALE; d->why[j * d->W + i] |= (uint8_t)(1u << s_stale); if (d->hdcv) d->hdcv[j * d->W + i] = 0; }
}
/* the cells a shape covers redrawn next: a stroke's own cells only (a long diagonal's box is most of the panel), the
 * others' box */
static void stale_shape(fluid_draw_t *d, const fluid_shape_t *sh, const int16_t *b) {
    if (sh->k == FLUID_SHAPE_RING) {                    /* its line's cells, in its (arc's) box */
        const float kx = (float)d->W / d->stage, ky = (float)d->H / d->stage, cw = 1.f / kx, ch = 1.f / ky;
        const float reach = sh->hw + 2.f + 0.5f * sqrtf(cw * cw + ch * ch);
        int i0 = (int)floorf((b[0] - d->ox) * kx), i1 = (int)ceilf((b[1] - d->ox) * kx), j0 = (int)floorf((b[2] - d->oy) * ky), j1 = (int)ceilf((b[3] - d->oy) * ky);
        i0 = i0 < 0 ? 0 : i0; j0 = j0 < 0 ? 0 : j0; i1 = i1 > d->W ? d->W : i1; j1 = j1 > d->H ? d->H : j1;
        for (int j = j0; j < j1; j++) for (int i = i0; i < i1; i++) {
            const float ex = (i + 0.5f) * cw - sh->x, ey = (j + 0.5f) * ch - sh->y, dc = sqrtf(ex * ex + ey * ey) - sh->r;
            if (fabsf(dc) <= reach) { d->key[j * d->W + i] = KEY_STALE; d->why[j * d->W + i] |= (uint8_t)(1u << s_stale); if (d->hdcv) d->hdcv[j * d->W + i] = 0; }
        }
        return;
    }
    if (sh->k != FLUID_SHAPE_SEG) { stale_box(d, b); return; }
    const float kx = (float)d->W / d->stage, ky = (float)d->H / d->stage, cw = 1.f / kx, ch = 1.f / ky;
    const float reach = sh->hw + 2.f + 0.5f * sqrtf(cw * cw + ch * ch), vx = sh->x1 - sh->x, vy = sh->y1 - sh->y, l2 = vx * vx + vy * vy;
    int i0 = (int)floorf((b[0] - d->ox) * kx), i1 = (int)ceilf((b[1] - d->ox) * kx), j0 = (int)floorf((b[2] - d->oy) * ky), j1 = (int)ceilf((b[3] - d->oy) * ky);
    i0 = i0 < 0 ? 0 : i0; j0 = j0 < 0 ? 0 : j0; i1 = i1 > d->W ? d->W : i1; j1 = j1 > d->H ? d->H : j1;
    for (int j = j0; j < j1; j++) for (int i = i0; i < i1; i++) {
        const float px = (i + 0.5f) * cw, py = (j + 0.5f) * ch;
        float u = l2 > 1e-6f ? ((px - sh->x) * vx + (py - sh->y) * vy) / l2 : 0.f;
        u = u < 0.f ? 0.f : u > 1.f ? 1.f : u;
        const float ex = px - sh->x - u * vx, ey = py - sh->y - u * vy;
        if (ex * ex + ey * ey <= reach * reach) { d->key[j * d->W + i] = KEY_STALE; d->why[j * d->W + i] |= (uint8_t)(1u << s_stale); if (d->hdcv) d->hdcv[j * d->W + i] = 0; }
    }
}
static uint32_t shape_hash(const fluid_shape_t *sh) {
    const float f[12] = { sh->x, sh->y, sh->x1, sh->y1, sh->r, sh->hw, sh->a0, sh->span, sh->w, sh->h, sh->co, sh->si };
    uint32_t h = (2166136261u ^ sh->k ^ (uint32_t)sh->acc << 8) * 16777619u;
    h = (h ^ sh->rgb) * 16777619u;
    h = (h ^ (uint32_t)(uintptr_t)sh->mask) * 16777619u;
    for (int q = 0; q < 12; q++) { uint32_t v; memcpy(&v, &f[q], 4); h = (h ^ v) * 16777619u; }
    return h;
}

void fluid_shape_box(const fluid_shape_t *sh, int ox, int oy, int16_t b[4]) {
    float ex, ey, cx = sh->x, cy = sh->y;
    switch (sh->k) {
    case FLUID_SHAPE_SEG:  ex = fabsf(sh->x1 - sh->x) * 0.5f + sh->hw; ey = fabsf(sh->y1 - sh->y) * 0.5f + sh->hw;
                           cx = (sh->x + sh->x1) * 0.5f; cy = (sh->y + sh->y1) * 0.5f; break;
    case FLUID_SHAPE_RING:
        ex = ey = sh->r + sh->hw;
        if (sh->span < 360.f) {                         /* a part of it: the box of its arc (y down), its ends and the
                                                         * axes it crosses */
            const float D2R = 3.1415927f / 180.f, a0 = sh->a0, a1 = sh->a0 + sh->span;
            float lx = cosf(a0 * D2R), hx = lx, ly = -sinf(a0 * D2R), hy = ly;
            const float x1 = cosf(a1 * D2R), y1 = -sinf(a1 * D2R);
            lx = x1 < lx ? x1 : lx; hx = x1 > hx ? x1 : hx; ly = y1 < ly ? y1 : ly; hy = y1 > hy ? y1 : hy;
            for (int q = (int)ceilf(a0 / 90.f); q * 90.f <= a1; q++) {
                const int m = ((q % 4) + 4) % 4;        /* 0 right, 1 up, 2 left, 3 down */
                if (m == 0) hx = 1.f; else if (m == 1) ly = -1.f; else if (m == 2) lx = -1.f; else hy = 1.f;
            }
            cx = sh->x + 0.5f * (lx + hx) * sh->r; cy = sh->y + 0.5f * (ly + hy) * sh->r;
            ex = 0.5f * (hx - lx) * sh->r + sh->hw; ey = 0.5f * (hy - ly) * sh->r + sh->hw;
        }
        break;
    case FLUID_SHAPE_DISC: ex = ey = sh->r; break;
    default:               ex = 0.5f * (fabsf(sh->co) * sh->w + fabsf(sh->si) * sh->h);
                           ey = 0.5f * (fabsf(sh->si) * sh->w + fabsf(sh->co) * sh->h); break;
    }
    /* (no libm: this runs for every shape that changed, and for the dust's build) */
    const float x0 = ox + cx - ex - 1.f, x1 = ox + cx + ex + 1.f, y0 = oy + cy - ey - 1.f, y1 = oy + cy + ey + 1.f;
    b[0] = (int16_t)(x0 < -1.f ? -1 : x0 > 4096.f ? 4096 : ifloor(x0)); b[1] = (int16_t)(x1 > 4096.f ? 4096 : x1 < -1.f ? -1 : iceil(x1));
    b[2] = (int16_t)(y0 < -1.f ? -1 : y0 > 4096.f ? 4096 : ifloor(y0)); b[3] = (int16_t)(y1 > 4096.f ? 4096 : y1 < -1.f ? -1 : iceil(y1));
}

/* each shape's box in fb px, [x0, x1) x [y0, y1), its soft edge included */
static uint32_t shape_hash(const fluid_shape_t *sh);
/* ... and this frame's hashes (d->sh1); a shape that is where it was, as it was, keeps its box */
static void shape_boxes(fluid_draw_t *d, const fluid_draw_opts_t *o) {
    if (!d->sh1) d->sh1 = FLUID_CALLOC(sizeof(uint32_t) * FLUID_MAX_SHAPES);
    for (int n = 0; n < o->nshapes && n < FLUID_MAX_SHAPES; n++) {
        const uint32_t h = shape_hash(&o->shapes[n]);
        if (d->sh1 && n < d->nsh0 && h == d->sh0[n] && h == d->sh1[n]) continue;
        if (d->sh1) d->sh1[n] = h;
        fluid_shape_box(&o->shapes[n], d->ox, d->oy, d->sb[n]);
    }
}

static bool hdc_room(fluid_draw_t *d);
static void morph_begin(fluid_draw_t *d);
static void morph_frame(fluid_draw_t *d, const fluid_frame_t *s, const fluid_draw_opts_t *o);
static inline bool morph_frozen(const fluid_draw_t *d);
static void morph_pass(fluid_draw_t *d, bool hd);
int fluid_draw_cells(fluid_draw_t *d, const fluid_frame_t *s, const fluid_draw_opts_t *o) {
    const int W = s->W, H = s->H;
    const int pal = (o->palette >= 0 && o->palette < FLUID_PAL_COUNT) ? o->palette : FLUID_PAL_PASTEL;
    const bool mono = pal != FLUID_PAL_PASTEL;
    const float *lo = PAL[pal][0], *hi = PAL[pal][1], *ac = PAL[pal][2];
    const char *ramp = o->ramp ? o->ramp : " .:-=+*#";
    fluid_draw_opts_t oo = *o; oo.ramp = ramp;
    const int rmax = (int)strlen(ramp) - 1;
    const float cw = (float)d->stage / W, ch = (float)d->stage / H;   /* letters are in stage px */
    const int ox = d->ox, oy = d->oy;
    const float melt = o->melt;
    const uint32_t frame = (uint32_t)(o->time * SHIMMER_HZ);
    int drawn = 0;

    if (d->m.on) {                                      /* a change: the drawer stands but for its new look's one pass */
        fluid_morph_t *m = &d->m;
        const bool hd_in = o->scene && o->dots && o->shapes;
        m->seen_hd |= hd_in;
        morph_begin(d);
        /* (its pass never on a frame of a scene that has no shapes, once its inputs were a scene's: it waits for them,
         * FLUID_MORPH_PASS_WAIT_N frames at most) */
        const bool wait = m->on && m->mm && !morph_frozen(d) && m->seen_hd && !hd_in && m->pwait < FLUID_MORPH_PASS_WAIT_N;
        m->pwait += wait;
        if (morph_frozen(d) || wait) {
            morph_frame(d, s, o);
            if (d->m.on || d->m.jhold) { d->anim_on = d->m.on ? 1u << FLUID_ANIM_WAVE : 0; d->anim_late = 0; return 0; }
        } else if (d->m.on) morph_pass(d, hd_in);
    }
    if (d->m.lc) memcpy(d->m.lc, d->m.lcn, (size_t)W * H);
    memset(d->why, 0, (size_t)W * H);
    /* a scene in high definition: its letters are among its shapes */
    const bool hd = o->scene && o->dots && o->shapes;
    const bool crisp = o->scene && o->dots && !o->shapes && o->dust <= 0.f;   /* crisp dots alone (crisp_flat) */
    if (hd) {
        memset(d->hch, 0, (size_t)W * H);
        if (o->dust > 0.f && !o->fine) dust_push(d, s, o);
        shape_boxes(d, o);
        halo_field(d, o->dots, o->fine != NULL);
        /* EXPERIMENT scene dust: the kind shown; a change of it redraws the glass */
        int sdk = o->fine ? fluid_fine_dust_kind_get() : FINE_DUST_SPECKS;
        if (sdk != FINE_DUST_SPECKS && !sd_field(d, o->dots, o->palette, sdk)) sdk = FINE_DUST_SPECKS;
        if (sdk != d->sd_kind) { d->sd_kind = (int8_t)sdk; d->force = true; }
        /* (the light holds still while the picture changes, and on the dots while a wave moves more than they send in
         * two frames: the eye is on the wave; it goes on from there after, never all at once) */
        if (o->fine && !o->fine->ahead) {               /* its fine dust instead of the haze (or worked out ahead on the sim task) */
            o->fine->pause = d->m.on || (d->sd_kind && d->sd_m > 2);
            fluid_fine_frame(o->fine, s, o);
        }
        if (d->sd_kind) sd_alive(d, o);
        else caption_box(d, o);
        hold_box(d, o);
        if (d->force && d->hdcv) memset(d->hdcv, 0, (size_t)W * H);   /* the drawing's cache, drawn again */
        /* a shape that changed (a tapped spot's drawing growing), came or went: the cells it covered and covers. The two
         * lists are walked together, so a shape that comes or goes in the middle (an aspect's rings, lit by a tap) leaves
         * those after it as they are: by their place in the list every one of them would count as changed. (The caption's
         * letters are not in the drawing's cache: its cells stay valid under them.) */
        const int n1 = o->nshapes < FLUID_MAX_SHAPES ? o->nshapes : FLUID_MAX_SHAPES;
        if (!d->shp0) d->shp0 = FLUID_CALLOC(sizeof(fluid_shape_t) * FLUID_MAX_SHAPES);
        if (d->shp0 && d->sh1) {
            enum { AHEAD = 24 };
            uint32_t *h1 = d->sh1;                          /* (shape_boxes) */
            uint8_t *const cache = d->hdcv;
#define GONE(q) do { if (d->shp0[q].tag == FLUID_TAG_CAPTION) d->hdcv = NULL; s_stale = d->shp0[q].tag == FLUID_TAG_CAPTION ? FLUID_ANIM_WORD : FLUID_ANIM_GROW; stale_shape(d, &d->shp0[q], d->sb0[q]); d->hdcv = cache; } while (0)
#define CAME(q) do { if (o->shapes[q].tag == FLUID_TAG_CAPTION) d->hdcv = NULL; s_stale = o->shapes[q].tag == FLUID_TAG_CAPTION ? FLUID_ANIM_WORD : FLUID_ANIM_GROW; stale_shape(d, &o->shapes[q], d->sb[q]); d->hdcv = cache; } while (0)
            int i = 0, j = 0;                               /* this frame's, the last one's */
            if (d->m.on) {                                  /* (a change's one pass: the whole look is new, every cell stale) */
                for (int k = 0; k < W * H; k++) d->key[k] = KEY_STALE;
                if (cache) memset(cache, 0, (size_t)W * H);
                i = n1; j = d->nsh0;
            }
            while (i < n1 || j < d->nsh0) {
                if (i < n1 && j < d->nsh0 && h1[i] == d->sh0[j]) { i++; j++; continue; }
                int di = -1, dj = -1;                       /* the next that match again: some came, or some went */
                for (int q = 1; q <= AHEAD && di < 0 && j < d->nsh0 && i + q < n1; q++) if (h1[i + q] == d->sh0[j]) di = q;
                for (int q = 1; q <= AHEAD && dj < 0 && i < n1 && j + q < d->nsh0; q++) if (d->sh0[j + q] == h1[i]) dj = q;
                if (di > 0 && (dj < 0 || di <= dj)) { for (int q = 0; q < di; q++) CAME(i + q); i += di; }
                else if (dj > 0) { for (int q = 0; q < dj; q++) GONE(j + q); j += dj; }
                else { if (j < d->nsh0) { GONE(j); j++; } if (i < n1) { CAME(i); i++; } }   /* changed in place */
            }
#undef GONE
#undef CAME
            memcpy(d->sh0, h1, sizeof(uint32_t) * (size_t)n1);
            memcpy(d->sb0, d->sb, sizeof(d->sb[0]) * (size_t)n1);
            memcpy(d->shp0, o->shapes, sizeof(fluid_shape_t) * (size_t)n1);
        } else d->force = true;                             /* no memory to compare with: all of it */
        d->nsh0 = n1;
    } else d->nsh0 = 0;

    /* condensed letters: which have enough ink, and which cells they cover (the box) */
    struct { int li; float ink, hh, acc, m; } shown[FLUID_MAX_LETTERS];
    int nshown = 0;
    memset(d->skip, 0, (size_t)W * H);
    for (int li = 0; li < o->nletters && li < FLUID_MAX_LETTERS && o->font && !hd; li++) {
        const fluid_letter_t *L = &o->letters[li];
        if (L->ch == ' ') continue;
        float sx = (L->x + L->w / 2) / d->stage * 2.f - 1.f, sy = (L->y + L->h / 2) / d->stage * 2.f - 1.f;
        const float m = melt + L->melt;                  /* the frame's melt + the letter's own */
        float ink = 1.f;
        if (o->scene) {
            /* a scene in high definition: its letters whole, unless they are dust (their shape is
             * on the dots under them): in a transition, or the scene's own (not its subtitle)
             * where the wave scatters (dust_radius) */
            float fx, fy;
            if (o->dust > 0.f || (!L->whole && (L->mask || (L->rgb & FLUID_RGB)) &&
                                  dust_radius(s, (int)((L->x + L->w / 2) / cw), (int)((L->y + L->h / 2) / ch), 0.f, &fx, &fy) >= 0.5f)) continue;
        } else {
            ink = fluid_frame_sample(s, s->ink, 1.f / FQ_UNIT, sx, sy);
            if (ink < 0.14f + m) continue;
        }
        shown[nshown].li = li; shown[nshown].ink = ink; shown[nshown].m = m;
        shown[nshown].hh = fluid_frame_sample(s, s->h, 1.f / FQ_H, sx, sy);
        shown[nshown].acc = fluid_frame_sample_acc(s, sx, sy);
        nshown++;
        int i0 = (int)floorf(L->x / cw), i1 = (int)floorf((L->x + L->w) / cw - 0.001f);
        int j0 = (int)floorf(L->y / ch), j1 = (int)floorf((L->y + L->h) / ch - 0.001f);
        if (i0 < 0) i0 = 0;
        if (j0 < 0) j0 = 0;
        if (i1 > W - 1) i1 = W - 1;
        if (j1 > H - 1) j1 = H - 1;
        for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) d->skip[j * W + i] = 1;
    }

    if (d->sd_kind == FINE_DUST_DOTS_ILACE && !d->force) {
        /* (EXPERIMENT) the creature screen's interlace of the water's own changes: 2:1, and while a wave would change more
         * cells than a frame has room for after the drawing's own changes (SD_FRAME_CELLS), over more frames (the
         * creature's rings go 3:1 so), up to SD_ILACE_MAX: every row still takes the water's change in turn, a steady
         * cadence, the wave's reach and speed as they are. The cells it will change: those the water moves or has left
         * now, by the share of such cells that changed last frame. */
        int c = 0;
        for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) {
            const int k = j * W + i;
            c += s->mask[k] && key_hd(d->key[k]) && (d->sd_ch[k] || (d->key[k] & 0xFFu) != d->sd_live[k] || !crisp_flat(s, i, j, k));
        }
        const int room = SD_FRAME_CELLS - d->sd_o > SD_WATER_MIN ? SD_FRAME_CELLS - d->sd_o : SD_WATER_MIN;   /* (what the drawing's own changes leave) */
        const int m = (int)ceilf(c * d->sd_a * 1.4f / room);
        d->sd_m = (uint8_t)(m < 2 ? 2 : m > SD_ILACE_MAX ? SD_ILACE_MAX : m);
    } else d->sd_m = 2;
    d->sd_w = d->sd_o = d->sd_c = 0;
    /* what each cell looks like; which changed */
    for (int j = 0; j < H; j++) {
        FLUID_PROF_BEGIN(c0);
        int i0 = W, i1 = 0;
        const uint8_t seed = hd ? (uint8_t)haze_seed(o, j) : 0;
        for (int i = 0; i < W; i++) {
            const int k = j * W + i;
            /* a scene in high definition, calm: a cell whose drawing, halo, dust seed and wave are as
             * they were is not looked at again */
            const fluid_fine_t *ff = hd ? o->fine : NULL;
            const bool dust_same = hd && (ff ? ff->sig[k] == ff->sig0[k] : d->hde[k] == seed);
            const bool look_same = hd && !d->force && o->dust <= 0.f && !d->hch[k] && s->mask[k] && !d->skip[k] && key_hd(d->key[k]) &&
                                   hd_still(s, o, i, j, k) && hd_sig(o->dots, W, H, i, j) == d->hds[k];
            if (look_same && dust_same &&
                (!d->sd_kind || (d->sd_kind == FINE_DUST_FINE_DOTS ? crisp_flat(s, i, j, k) && d->sd_hard[k] == (d->sd_live[k] & ~((d->key[k] & 0xFFu) & ~d->sd_hard[k]))
                                 : (d->key[k] & 0xFFu) == d->sd_live[k] && d->col[k] == d->sd_lcol[k] && crisp_flat(s, i, j, k)))) { d->dirty[k] = 0; continue; }   /* (EXPERIMENT: its dust on the lattice, the water leaving it there) */
            if (look_same && ff && !d->sd_kind) {           /* only its fine dust changed (the water or the drift moved it, the
                                                             * light, a word's backdrop): its drawing as it was, the cell sent again */
                d->why[k] |= ff->why[k]; ff->sig0[k] = ff->sig[k]; d->dirty[k] = 3;   /* (3: its dust alone: glass_move) */
                if (i < i0) i0 = i;
                i1 = i + 1;
                drawn += d->key[k] != KEY_EMPTY;
                continue;
            }
            /* (EXPERIMENT FINE_DUST_DOTS_ILACE: what only the water, the light or the twinkle changes, in every other cell row a
             * frame, as crisp dots) */
            const bool sd_turn = d->sd_kind == FINE_DUST_DOTS_ILACE && !d->force && (!d->hch[k] || d->sd_ch[k]) && dust_same && s->mask[k] &&
                                 key_hd(d->key[k]) && hd_sig(o->dots, W, H, i, j) == d->hds[k];
            if (sd_turn && (j + d->ilace) % d->sd_m) { d->dirty[k] = 0; continue; }
            if (crisp && s->mask[k] && !d->skip[k]) {
                /* crisp dots (a rigged creature): a cell the wave leaves in place shows its own dots in its own
                 * colour, so one whose dots and colour are as they were (and were shown so) is not looked at again */
                const fluid_rgba16_t *c = &s->col[k];
                const uint32_t sig = (uint32_t)(c->r >> 8) << 24 | (uint32_t)(c->g >> 8) << 16 | (uint32_t)(c->b >> 8) << 8 | (uint32_t)(c->a >> 8);
                const uint16_t c0 = (uint16_t)(o->dots[k] | (crisp_flat(s, i, j, k) ? 0x100 : 0));
                const bool same = d->crisp0 && !d->force && sig == d->csig[k] && d->key[k] != KEY_SKIP && d->key[k] != KEY_STALE;
                if (same && (c0 & 0x100) && c0 == d->cd0[k]) { d->dirty[k] = 0; continue; }
                /* its own dots and colour as they were, only the water moving them (a ripple, the ring's wave): such
                 * cells in every other cell row a frame, alternating, as video interlaces (the panel's cost is by the
                 * row); the creature's own changes go every frame */
                if (same && !(c0 & 0x100) && (c0 & 0xff) == (d->cd0[k] & 0xff) && (o->frame ? !fluid_ilace_row(o->frame, j) : ((j + d->ilace) & 1))) { d->dirty[k] = 0; continue; }
                d->cd0[k] = c0; d->csig[k] = sig;
            }
            uint16_t key = KEY_EMPTY, col = 0, hcol = 0;
            uint8_t hard = 0;
            if (d->skip[k]) key = KEY_SKIP;
            else if (s->mask[k]) cell_look(d, s, &oo, i, j, k, rmax, frame, &key, &col, &hard, &hcol);
            key |= hard;                                    /* (EXPERIMENT kind 3: the hard dots, in the key, drawn apart) */
            d->hde[k] = hd && o->dust <= 0.f && key_hd(key) && hd_still(s, o, i, j, k) ? seed : 255;
            /* what the shapes draw in a cell changes with the dots around it */
            const uint8_t sig = key_hd(key) ? hd_sig(o->dots, W, H, i, j) : 0;
            /* a letter's cleared cell that was one last frame: sent again only if the letters changed (2: decided below) */
            const bool lt_cell = key == KEY_SKIP && d->key[k] == KEY_SKIP && !d->force;
            /* everything that changed goes this frame (the light's changes come on its cells' turns: fluid_fine_frame) */
            const bool sdk3 = d->sd_kind == FINE_DUST_FINE_DOTS;
            const bool dirty = key == KEY_SKIP || d->force || key != d->key[k] || col != d->col[k] || sig != d->hds[k] ||
                               (sdk3 && (hard != d->sd_hard[k] || hcol != d->sd_hcol[k])) ||
                               (hd && o->fine && !dust_same && (!d->sd_kind || sdk3 || d->cdist[k] <= 1 || d->sd_pm[k]));
            if (dirty && ff) { if (!dust_same) d->why[k] |= ff->why[k]; ff->sig0[k] = ff->sig[k]; }   /* the dust as drawn */
            if (dirty && ff && d->sd_kind && !d->force && !crisp_flat(s, i, j, k)) d->why[k] |= 1u << FLUID_ANIM_WATER;   /* (EXPERIMENT) */
            d->sd_w += dirty && sd_turn; d->sd_o += dirty && !sd_turn && !d->force;
            d->sd_c += sd_turn && (d->sd_ch[k] || !crisp_flat(s, i, j, k) || (key & 0xFFu) != d->sd_live[k] || dirty);
            /* (3: its look as it was, its dust alone changed, where the water moves it too: glass_move) */
            const bool dust_only = ff && !d->sd_kind && !d->force && key != KEY_SKIP && key == d->key[k] && col == d->col[k] && sig == d->hds[k];
            d->dirty[k] = (uint8_t)(lt_cell ? 2 : dirty ? (dust_only ? 3 : 1) : 0);
            if (dirty) {
                if (i < i0) i0 = i;
                i1 = i + 1;
                d->key[k] = key; d->col[k] = col; d->hds[k] = sig;
                if (sdk3) { d->sd_hard[k] = hard; d->sd_hcol[k] = hcol; }
                if (key != KEY_EMPTY && key != KEY_SKIP) drawn++;
            }
        }
        d->span[2 * j] = (int16_t)(i0 < i1 ? i0 : 0); d->span[2 * j + 1] = (int16_t)(i0 < i1 ? i1 : 0);
        FLUID_PROF_END(FP_DSAMPLE, c0);
    }
    d->force = false;
    d->crisp0 = crisp; d->ilace++;
    if (d->sd_c) { const float a = (float)d->sd_w / d->sd_c; d->sd_a = a > 0.9f * d->sd_a ? a : 0.9f * d->sd_a; }   /* (slow to trust fewer) */
    else if (d->sd_a <= 0.f) d->sd_a = 1.f;

    /* condensed letters ride the surface (y follows the wave height) and decode from
     * ramp glyphs into the real letter as ink arrives */
    d->nlt = 0;
    for (int n = 0; n < nshown; n++) {
        const fluid_letter_t *L = &o->letters[shown[n].li];
        float ink = shown[n].ink, m = shown[n].m;
        char c2;
        if (L->mask && ink <= 0.36f + m * 1.5f) continue;   /* a symbol: whole, once the ink is in */
        if (ink > 0.36f + m * 1.5f) c2 = L->ch;
        else { int idx = (int)lrintf(fmaxf(0.f, ink - m) * rmax); c2 = ramp[idx < 1 ? 1 : idx > rmax ? rmax : idx]; }
        float br = fminf(1.f, 0.55f + ink), r, g, b;
        if (mono) { r = lo[0] + (hi[0] - lo[0]) * br; g = lo[1] + (hi[1] - lo[1]) * br; b = lo[2] + (hi[2] - lo[2]) * br; }
        else if (L->rgb & FLUID_RGB) {                  /* its own colour (a question's button) */
            if (L->acc) br = 1.f;                        /* accented = at full strength       */
            r = (float)(L->rgb >> 16 & 255) * br; g = (float)(L->rgb >> 8 & 255) * br; b = (float)(L->rgb & 255) * br;
        } else { r = g = b = 255.f * br; }
        if (L->acc && (mono || !(L->rgb & FLUID_RGB))) {
            float a = fmaxf(shown[n].acc, 0.85f); if (a > 1.f) a = 1.f; r += (ac[0] - r) * a; g += (ac[1] - g) * a; b += (ac[2] - b) * a;
        }
        int gw = (int)(L->w * 0.8f), gh = (int)(L->h * 0.72f);
        int margin = ((int)L->h - gh) / 2 - 1;
        if (margin < 0) margin = 0;
        float yo = -shown[n].hh * 7.f;
        if (yo > 3.f) yo = 3.f;
        if (yo < -3.f) yo = -3.f;
        int yoff = (int)lrintf(yo);
        if (yoff > margin) yoff = margin;
        if (yoff < -margin) yoff = -margin;
        int gx = ox + (int)lrintf(L->x + (L->w - gw) * 0.5f), gy = oy + (int)lrintf(L->y) + margin + yoff;
        int cx0 = ox + (int)floorf(L->x), cy0 = oy + (int)floorf(L->y), cx1 = ox + (int)ceilf(L->x + L->w), cy1 = oy + (int)ceilf(L->y + L->h);
        if (L->mask) {
            gw = (int)lrintf(L->w); gh = (int)lrintf(L->h); gx = ox + (int)lrintf(L->x); gy = oy + (int)lrintf(L->y);
            /* a symbol seen through the water, like the dots (DOT_REFR): it drifts by the wave
             * slope inside its own cleared cells, so nothing outside them needs a redraw */
            const float sx = (L->x + L->w / 2) / d->stage * 2.f - 1.f, sy = (L->y + L->h / 2) / d->stage * 2.f - 1.f;
            const float ex = 2.f / W, ey = 2.f / H, kp = -DOT_REFR * cw;   /* the image moves against the slope */
            int mx = (int)lrintf((fluid_frame_sample(s, s->h, 1.f / FQ_H, sx + ex, sy) - fluid_frame_sample(s, s->h, 1.f / FQ_H, sx - ex, sy)) * kp);
            int my = (int)lrintf((fluid_frame_sample(s, s->h, 1.f / FQ_H, sx, sy + ey) - fluid_frame_sample(s, s->h, 1.f / FQ_H, sx, sy - ey)) * kp);
            if (mx || my) {
                const int i0 = (int)floorf(L->x / cw), i1 = (int)floorf((L->x + L->w) / cw - 0.001f);
                const int j0 = (int)floorf(L->y / ch), j1 = (int)floorf((L->y + L->h) / ch - 0.001f);
                cx0 = d->cx[i0 < 0 ? 0 : i0]; cx1 = d->cx[i1 > W - 1 ? W : i1 + 1];
                cy0 = d->cy[j0 < 0 ? 0 : j0]; cy1 = d->cy[j1 > H - 1 ? H : j1 + 1];
                mx = mx < cx0 - gx ? cx0 - gx : mx > cx1 - gx - gw ? cx1 - gx - gw : mx;
                my = my < cy0 - gy ? cy0 - gy : my > cy1 - gy - gh ? cy1 - gy - gh : my;
                gx += mx; gy += my;
            }
        }
        d->lt[d->nlt].gl = L->mask ? NULL : o->font(c2); d->lt[d->nlt].c = rgb565_be((int)r, (int)g, (int)b);
        d->lt[d->nlt].mask = L->mask; d->lt[d->nlt].mw = L->mw; d->lt[d->nlt].mh = L->mh;
        d->lt[d->nlt].x = (int16_t)gx; d->lt[d->nlt].y = (int16_t)gy; d->lt[d->nlt].w = (int16_t)gw; d->lt[d->nlt].h = (int16_t)gh;
        d->lt[d->nlt].cx0 = (int16_t)cx0; d->lt[d->nlt].cy0 = (int16_t)cy0;
        d->lt[d->nlt].cx1 = (int16_t)cx1; d->lt[d->nlt].cy1 = (int16_t)cy1;
        d->nlt++;
        drawn++;
    }
    {   /* each letter as it was (glyph, colour, place, mask): the cells it covers need not go again; a changed one's do */
        bool gone = false;                               /* a letter went: what it covered is not known here, all go */
        for (int n = 0; n < FLUID_MAX_LETTERS; n++) {
            uint32_t h = 2166136261u;
            #define LT_MIX(v) (h = (h ^ (uint32_t)(v)) * 16777619u)
            if (n < d->nlt) {
                LT_MIX((uintptr_t)d->lt[n].gl); LT_MIX((uintptr_t)d->lt[n].mask); LT_MIX(d->lt[n].c); LT_MIX(d->lt[n].mw << 16 | d->lt[n].mh);
                LT_MIX((uint16_t)d->lt[n].x << 16 | (uint16_t)d->lt[n].y); LT_MIX((uint16_t)d->lt[n].w << 16 | (uint16_t)d->lt[n].h);
                LT_MIX((uint16_t)d->lt[n].cx0 << 16 | (uint16_t)d->lt[n].cy0); LT_MIX((uint16_t)d->lt[n].cx1 << 16 | (uint16_t)d->lt[n].cy1);
                for (int b = 0; d->lt[n].mask && b < (d->lt[n].mw * d->lt[n].mh + 7) / 8; b++) LT_MIX(d->lt[n].mask[b]);
            }
            #undef LT_MIX
            if (h == d->lt_sig[n]) continue;
            d->lt_sig[n] = h;
            if (n >= d->nlt) { gone = true; continue; }
            /* its cells (the skip box, fb px back to cells, a cell wider for rounding) go again */
            const int i0 = (int)floorf((d->lt[n].cx0 - ox) / cw) - 1, i1 = (int)floorf((d->lt[n].cx1 - ox) / cw) + 1;
            const int j0 = (int)floorf((d->lt[n].cy0 - oy) / ch) - 1, j1 = (int)floorf((d->lt[n].cy1 - oy) / ch) + 1;
            for (int j = j0 < 0 ? 0 : j0; j <= j1 && j < H; j++) for (int i = i0 < 0 ? 0 : i0; i <= i1 && i < W; i++)
                if (d->dirty[j * W + i] == 2) d->dirty[j * W + i] = 1;
        }
        for (int k = 0; d->owe && d->nowed && k < W * H; k++) {   /* (a cell the last frame owed: drawn now) */
            if (!d->owe[k]) continue;
            const int i = k % W, j = k / W;
            d->owe[k] = 0; d->dirty[k] = 1;
            if (d->span[2 * j] >= d->span[2 * j + 1]) { d->span[2 * j] = (int16_t)i; d->span[2 * j + 1] = (int16_t)(i + 1); }
            else { if (i < d->span[2 * j]) d->span[2 * j] = (int16_t)i; if (i + 1 > d->span[2 * j + 1]) d->span[2 * j + 1] = (int16_t)(i + 1); }
        }
        d->nowed = 0;
        for (int j = 0; j < H; j++) {
            int i0 = W, i1 = 0;
            for (int i = d->span[2 * j]; i < d->span[2 * j + 1]; i++) {
                uint8_t *dt = &d->dirty[j * W + i];
                if (*dt == 2) *dt = gone;
                if (*dt) { if (i < i0) i0 = i; i1 = i + 1; }
            }
            d->span[2 * j] = (int16_t)(i0 < i1 ? i0 : 0); d->span[2 * j + 1] = (int16_t)(i0 < i1 ? i1 : 0);
        }
    }
    if (d->m.on) morph_frame(d, s, o);                  /* the picture changing: what of it changes this frame */
    /* each animation's cells this frame, and whether any change of them waits (none may: while the picture changes,
     * only its whole cells show the look) */
    d->anim_on = d->m.on ? 1u << FLUID_ANIM_WAVE : 0; d->anim_late = 0;
    for (int k = 0; k < W * H; k++) {
        if (!d->why[k]) continue;
        d->anim_on |= d->why[k];
        if (!d->dirty[k] && !d->m.on) d->anim_late |= d->why[k];
    }
    return drawn;
}

/* ── a scene in high definition: its shapes at the panel's pixels ───────────── */

/* a shape's colour on the palette: its own (pastel; a mono palette's bright end), the palette's
 * letter colour, the accent (an accented letter with no colour of its own), or black (it erases) */
static void shape_rgb(const fluid_shape_t *sh, int pal, float *c) {
    const float *lo = PAL[pal][0], *hi = PAL[pal][1];
    const float k = 0.25f + 0.75f * SCENE_LEVEL;
    if (sh->acc && (pal != FLUID_PAL_PASTEL || !(sh->rgb & FLUID_RGB))) { memcpy(c, PAL[pal][2], 3 * sizeof(float)); return; }
    for (int q = 0; q < 3; q++) {
        const float own = (float)(sh->rgb >> (16 - 8 * q) & 255);
        c[q] = !(sh->rgb & FLUID_RGB) ? (pal == FLUID_PAL_PASTEL ? 255.f : hi[q])
             : !(sh->rgb & 0xffffffu) ? 0.f : pal == FLUID_PAL_PASTEL ? own : lo[q] + (hi[q] - lo[q]) * k;
    }
}

uint16_t fluid_shape_565(const fluid_shape_t *sh, int palette) {
    float c[3];
    shape_rgb(sh, palette >= 0 && palette < FLUID_PAL_COUNT ? palette : FLUID_PAL_PASTEL, c);
    return (uint16_t)(((int)c[0] & 0xF8) << 8 | ((int)c[1] & 0xFC) << 3 | (int)c[2] >> 3);
}

/* one bit of a mask's or a char's image, 0 outside it */
static inline int shape_bit(const fluid_shape_t *sh, int x, int y) {
    if (x < 0 || y < 0 || x >= sh->mw || y >= sh->mh) return 0;
    if (sh->k == FLUID_SHAPE_CHAR) return sh->mask[y] >> (4 - x) & 1;
    const int p = y * sh->mw + x;
    return sh->mask[p >> 3] >> (7 - (p & 7)) & 1;
}

/* how much of a mask or a char covers the pixel at (u, v) in its bits: read bilinear, a smooth
 * edge at HD_EDGE (e: half its width, on that reading) */
static inline float mask_cover(const fluid_shape_t *sh, float u, float v, float e) {
    const int iu = ifloor(u), iv = ifloor(v);
    const float tu = u - (float)iu, tv = v - (float)iv;
    const int b00 = shape_bit(sh, iu, iv), b10 = shape_bit(sh, iu + 1, iv), b01 = shape_bit(sh, iu, iv + 1), b11 = shape_bit(sh, iu + 1, iv + 1);
    if (!(b00 | b10 | b01 | b11)) return 0.f;
    const float top = b00 + (b10 - b00) * tu, bot = b01 + (b11 - b01) * tu;
    float t = (top + (bot - top) * tv - (HD_EDGE - e)) * (0.5f / e);   /* fluid_smooth(HD_EDGE - e, HD_EDGE + e, ...) */
    t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
    return t * t * (3.f - 2.f * t);
}

/* how much of the pixel at (dx, dy) from the shape's centre (stage px) a stroke, a ring or a
 * disc covers, 0..1: by its distance, one pixel of soft edge */
static inline float shape_cover(const fluid_shape_t *sh, float dx, float dy, float il2) {
    float dist;
    switch (sh->k) {
    case FLUID_SHAPE_SEG: {
        const float vx = sh->x1 - sh->x, vy = sh->y1 - sh->y;
        float u = (dx * vx + dy * vy) * il2;
        u = u < 0.f ? 0.f : u > 1.f ? 1.f : u;
        const float ex = dx - u * vx, ey = dy - u * vy;
        const float d2 = ex * ex + ey * ey, lo = sh->hw - 0.5f, hi = sh->hw + 0.5f;
        if (d2 >= hi * hi) return 0.f;                          /* the squared distance first: most pixels need no root */
        if (lo > 0.f && d2 <= lo * lo) return 1.f;
        dist = fsqrt(d2) - sh->hw;
        break;
    }
    case FLUID_SHAPE_RING: {
        dist = fabsf(fsqrt(dx * dx + dy * dy) - sh->r) - sh->hw;
        if (sh->span < 360.f && dist < 0.5f) {
            float a = fmodf(fluid_atan2(-dy, dx) * (180.f / 3.1415927f) - sh->a0, 360.f);
            if (a < 0.f) a += 360.f;
            if (a > sh->span) {                                   /* past an end: round caps */
                const float a0 = sh->a0 * (3.1415927f / 180.f), a1 = (sh->a0 + sh->span) * (3.1415927f / 180.f);
                const float x0 = dx - sh->r * cosf(a0), y0 = dy + sh->r * sinf(a0), x1 = dx - sh->r * cosf(a1), y1 = dy + sh->r * sinf(a1);
                dist = sqrtf(fminf(x0 * x0 + y0 * y0, x1 * x1 + y1 * y1)) - sh->hw;
            }
        }
        break;
    }
    default: {
        const float d2 = dx * dx + dy * dy, lo = sh->r - 0.5f, hi = sh->r + 0.5f;
        if (d2 >= hi * hi) return 0.f;
        if (lo > 0.f && d2 <= lo * lo) return 1.f;
        dist = fsqrt(d2) - sh->r;
        break;
    }
    }
    return dist >= 0.5f ? 0.f : dist <= -0.5f ? 1.f : 0.5f - dist;
}

/* colour c (rgb565, little-endian) at a / 256 over a big-endian rgb565 pixel */
static inline void blend565(uint16_t *px, uint16_t c, int a) {
    if (a >= 256) { *px = (uint16_t)(c >> 8 | c << 8); return; }
    const uint16_t le = (uint16_t)(*px >> 8 | *px << 8);
    const int na = 256 - a;
    const int r = ((le >> 11) * na + (c >> 11) * a + 128) >> 8, g = ((le >> 5 & 63) * na + (c >> 5 & 63) * a + 128) >> 8,
              b = ((le & 31) * na + (c & 31) * a + 128) >> 8;
    const uint16_t o = (uint16_t)(r << 11 | g << 5 | b);
    *px = (uint16_t)(o >> 8 | o << 8);
}

/* The shapes over pixel row y of the window (row[0] is fb x = bx), inside [xa, xb): each pixel
 * at its weight (wt, 0..HD_LEVELS: its cell's; NULL whole), each shape over what is under it. A ring, a
 * disc or a stroke visits only the pixels of the row it can reach. A caption letter shows only the pixels
 * that formed (their hash under 1 - unformed: a dithered order). */
void fluid_shapes_row(const fluid_shape_t *shapes, int nshapes, const int16_t (*box)[4], int ox, int oy, int palette, const uint8_t *wt,
                      float unformed, uint16_t *row, int bx, int y, int xa, int xb) {
    fluid_shapes_row_dim(shapes, nshapes, box, ox, oy, palette, wt, unformed, NULL, row, bx, y, xa, xb);
}
void fluid_shapes_row_dim(const fluid_shape_t *shapes, int nshapes, const int16_t (*box)[4], int ox, int oy, int palette, const uint8_t *wt,
                          float unformed, const float *dim, uint16_t *row, int bx, int y, int xa, int xb) {
    const int pal = (palette >= 0 && palette < FLUID_PAL_COUNT) ? palette : FLUID_PAL_PASTEL;
    const float Y = (float)(y - oy) + 0.5f;
    /* the caption's backdrop over the drawing on this row: how far it reaches (none: dim NULL or off the row) */
    const float dey = !dim ? 1e9f : Y < dim[2] ? dim[2] - Y : Y > dim[3] ? Y - dim[3] : 0.f;
    const bool dimrow = dim && dim[4] > 0.f && dey < FLUID_DIM_REACH;
    for (int n = 0; n < nshapes && n < FLUID_MAX_SHAPES; n++) {
        const int16_t *b = box[n];
        if (y < b[2] || y >= b[3] || b[1] <= xa || b[0] >= xb) continue;
        const fluid_shape_t *sh = &shapes[n];
        const bool forming = sh->tag == FLUID_TAG_CAPTION && unformed > 0.f;
        if (forming && unformed >= 1.f) continue;
        const float cx = ox + sh->x, dy = Y - sh->y;
        float span[2][2] = { { (float)b[0], (float)b[1] }, { 0.f, 0.f } };   /* fb x, at most two */
        float il2 = 0.f, e = 0.f, du = 0.f, dv = 0.f;
        if (sh->k == FLUID_SHAPE_DISC || sh->k == FLUID_SHAPE_RING) {
            const float ro = (sh->k == FLUID_SHAPE_RING ? sh->r + sh->hw : sh->r) + 1.f, ri = sh->k == FLUID_SHAPE_RING ? sh->r - sh->hw - 1.f : 0.f;
            if (ro * ro <= dy * dy) continue;
            const float xo = fsqrt(ro * ro - dy * dy), xi = ri > fabsf(dy) ? fsqrt(ri * ri - dy * dy) : 0.f;
            span[0][0] = cx - xo; span[0][1] = cx + (xi > 0.f ? -xi : xo) + 1.f;
            if (xi > 0.f) { span[1][0] = cx + xi; span[1][1] = cx + xo + 1.f; }
        } else if (sh->k == FLUID_SHAPE_SEG) {
            const float vx = sh->x1 - sh->x, vy = sh->y1 - sh->y, l2 = vx * vx + vy * vy;
            il2 = l2 > 1e-6f ? 1.f / l2 : 0.f;
            if (fabsf(vy) > 1e-3f) {                    /* not level: where the band crosses the row */
                const float il = 1.f / fsqrt(l2), nx = -vy * il, ny = vx * il, reach = sh->hw + 1.f, c = ny * (Y - sh->y), inx = 1.f / nx;
                const float p = (-reach - c) * inx, q = (reach - c) * inx;
                const float lo = ox + sh->x + (p < q ? p : q) - 1.f, hi = ox + sh->x + (p < q ? q : p) + 2.f;
                if (lo > span[0][0]) span[0][0] = lo;
                if (hi < span[0][1]) span[0][1] = hi;
            }
        } else {                                        /* a mask: (u, v) in its bits, a step a pixel */
            const float kx = sh->mw / sh->w, ky = sh->mh / sh->h;
            e = 0.5f * (kx > ky ? kx : ky);
            e = e < 0.02f ? 0.02f : e > 0.45f ? 0.45f : e;
            du = sh->co / sh->w * sh->mw; dv = sh->si / sh->h * sh->mh;
        }
        const bool mask = sh->k == FLUID_SHAPE_MASK || sh->k == FLUID_SHAPE_CHAR;
        float c[3];
        shape_rgb(sh, pal, c);
        const uint16_t le = (uint16_t)(((int)c[0] & 0xF8) << 8 | ((int)c[1] & 0xFC) << 3 | (int)c[2] >> 3);
        for (int q = 0; q < 2; q++) {
            if (span[q][1] <= span[q][0]) continue;
            int l = span[q][0] < -1000.f ? -1000 : ifloor(span[q][0]), r = span[q][1] > 5000.f ? 5000 : iceil(span[q][1]);
            if (l < xa) l = xa;
            if (l < b[0]) l = b[0];
            if (r > xb) r = xb;
            if (r > b[1]) r = b[1];
            const float dx0 = (float)(l - ox) + 0.5f - sh->x;
            float u = ((sh->co * dx0 - sh->si * dy) / sh->w + 0.5f) * sh->mw - 0.5f, v = ((sh->si * dx0 + sh->co * dy) / sh->h + 0.5f) * sh->mh - 0.5f;
            const bool dimmed = dimrow && sh->tag != FLUID_TAG_CAPTION;
            for (int x = l; x < r; x++, u += du, v += dv) {
                const int w = wt ? wt[x] : HD_LEVELS;
                if (!w || (forming && hash_u(x - ox, y - oy, 3) >= 1.f - unformed)) continue;
                float a = mask ? mask_cover(sh, u, v, e) : shape_cover(sh, (float)(x - ox) + 0.5f - sh->x, dy, il2);
                if (a > 0.f && dimmed) {
                    const float X = (float)(x - ox) + 0.5f, dex = X < dim[0] ? dim[0] - X : X > dim[1] ? X - dim[1] : 0.f;
                    const float dd = fsqrt(dex * dex + dey * dey);
                    if (dd < FLUID_DIM_REACH && dd >= dim[5] && dd < dim[6]) { const float t = dd / FLUID_DIM_REACH; a *= 1.f - dim[4] * (1.f - t * t * (3.f - 2.f * t)); }
                }
                if (a > 0.f) blend565(&row[x - bx], le, (int)(a * w * (256.f / HD_LEVELS) + 0.5f));
            }
        }
    }
}

/* A scene on fine dust redraws only the dust most frames, over a drawing that stays: the drawing (every shape but the
 * caption's, whole) is drawn once into d->hdc, a cell at a time when a window first needs it (or a shape there changed),
 * and copied from there; the caption's letters are drawn over it live. */
static bool hdc_cell(fluid_draw_t *d, const fluid_draw_opts_t *o, int k, int ncache) {
    const int i = k % d->W, j = k / d->W, x0 = d->cx[i], x1 = d->cx[i + 1], y0 = d->cy[j], y1 = d->cy[j + 1];
    /* the few shapes that reach the cell, in their order (each row then looks at those alone); the scratch for them in
     * PSRAM (the drawer's stack is small) */
    enum { NEAR = 96 };                                 /* (past it every shape is tried on each row: at the chart's middle, where
                                                         * its lines meet, that drew a cell in ~10 ms on the device) */
    if (!d->near) d->near = FLUID_CALLOC((sizeof(fluid_shape_t) + sizeof(int16_t[4])) * NEAR);
    fluid_shape_t *sh = d->near;
    int16_t (*box)[4] = (int16_t (*)[4])(sh + NEAR);
    int n = d->near ? 0 : NEAR + 1;
    const float ccx = (x0 + x1) * 0.5f - d->ox, ccy = (y0 + y1) * 0.5f - d->oy;   /* its centre (stage px) and half diagonal */
    const float hx = (x1 - x0) * 0.5f, hy = (y1 - y0) * 0.5f, reach = fsqrt(hx * hx + hy * hy) + 1.5f;
    for (int q = 0; q < ncache && n <= NEAR; q++) {
        const int16_t *b = d->sb[q];
        if (b[3] <= y0 || b[2] >= y1 || b[1] <= x0 || b[0] >= x1) continue;
        /* (a ring's or a long stroke's box holds far more cells than it crosses) */
        const fluid_shape_t *sq = &o->shapes[q];
        if (sq->k == FLUID_SHAPE_RING) {
            const float dc = fsqrt((ccx - sq->x) * (ccx - sq->x) + (ccy - sq->y) * (ccy - sq->y)) - sq->r;
            if ((dc < 0.f ? -dc : dc) > sq->hw + reach) continue;
        } else if (sq->k == FLUID_SHAPE_SEG) {
            const float vx = sq->x1 - sq->x, vy = sq->y1 - sq->y, l2 = vx * vx + vy * vy;
            float u = l2 > 1e-6f ? ((ccx - sq->x) * vx + (ccy - sq->y) * vy) / l2 : 0.f;
            u = u < 0.f ? 0.f : u > 1.f ? 1.f : u;
            const float ex = ccx - sq->x - u * vx, ey = ccy - sq->y - u * vy, lim = sq->hw + reach;
            if (ex * ex + ey * ey > lim * lim) continue;
        } else if (sq->k == FLUID_SHAPE_DISC) {
            const float lim = sq->r + reach;
            if ((ccx - sq->x) * (ccx - sq->x) + (ccy - sq->y) * (ccy - sq->y) > lim * lim) continue;
        }
        if (n < NEAR) { sh[n] = o->shapes[q]; memcpy(box[n], b, sizeof(box[n])); }
        n++;
    }
    for (int y = y0; y < y1; y++) {
        uint16_t *row = d->hdc + (size_t)y * d->fb_w;
        memset(row + x0, 0, sizeof(uint16_t) * (size_t)(x1 - x0));
        if (n > NEAR) fluid_shapes_row(o->shapes, ncache, (const int16_t (*)[4])d->sb, d->ox, d->oy, o->palette, NULL, 0.f, row, 0, y, x0, x1);
        else if (n) fluid_shapes_row(sh, n, (const int16_t (*)[4])box, d->ox, d->oy, o->palette, NULL, 0.f, row, 0, y, x0, x1);
    }
    d->hdcv[k] = n > 0 ? 2 : 1;                         /* (1: nothing of the drawing reaches it, black) */
    d->nbuilt++;
    return n > 0;
}
/* the cached drawing's pixels [xa, xb) of fb row y into row (row[0] = fb x bx), darker under the caption's backdrop */
static void hdc_copy(const fluid_draw_t *d, const fluid_draw_opts_t *o, uint16_t *row, int bx, int y, int xa, int xb) {
    const uint16_t *src = d->hdc + (size_t)y * d->fb_w;
    memcpy(row + (xa - bx), src + xa, sizeof(uint16_t) * (size_t)(xb - xa));
    const float *dim = o->fine->dim;
    if (dim[4] <= 0.f) return;
    const float Y = (float)(y - d->oy) + 0.5f, dey = Y < dim[2] ? dim[2] - Y : Y > dim[3] ? Y - dim[3] : 0.f;
    if (dey >= FLUID_DIM_REACH) return;
    const int la = ifloor(dim[0] - FLUID_DIM_REACH) + d->ox, lb = iceil(dim[1] + FLUID_DIM_REACH) + d->ox;
    for (int x = xa > la ? xa : la; x < xb && x < lb; x++) {
        uint16_t *p = &row[x - bx];
        if (!*p) continue;
        const float X = (float)(x - d->ox) + 0.5f, dex = X < dim[0] ? dim[0] - X : X > dim[1] ? X - dim[1] : 0.f, dd = fsqrt(dex * dex + dey * dey);
        if (dd >= FLUID_DIM_REACH || dd < dim[5] || dd >= dim[6]) continue;
        const float tt = dd / FLUID_DIM_REACH, k = 1.f - dim[4] * (1.f - tt * tt * (3.f - 2.f * tt));
        const int a = (int)(k * 256.f);
        const uint16_t le = (uint16_t)(*p >> 8 | *p << 8);
        const uint16_t o16 = (uint16_t)(((le >> 11) * a >> 8) << 11 | ((le >> 5 & 63) * a >> 8) << 5 | ((le & 31) * a >> 8));
        *p = (uint16_t)(o16 >> 8 | o16 << 8);
    }
}

static bool hdc_room(fluid_draw_t *d) {
    if (d->hdc) return true;
    /* (not zeroed: 434 KB of PSRAM is ~25 ms on the device, and a cell of it is drawn before it is read: hdcv) */
    d->hdc = FLUID_MALLOC(sizeof(uint16_t) * (size_t)d->fb_w * d->fb_h);
    d->hdcv = FLUID_CALLOC(2 * (size_t)d->W * d->H);
    if (!d->hdc || !d->hdcv) { FLUID_FREE(d->hdc); FLUID_FREE(d->hdcv); d->hdc = NULL; d->hdcv = NULL; }
    d->owe = d->hdcv ? d->hdcv + d->W * d->H : NULL;
    return d->hdc != NULL;
}


/* ── the whole picture changing, as dust (fluid_morph_t) ──────────────────────── */

/* "Everywhere at once" (the approved prototype, dissolve-proto/index.html: duration 1.2, spread 0.6, turbulence 0.5),
 * seconds of the change's clock. Each pair flies one path without a stop: out of its old spot (fast, easing), through its
 * cloud point (still moving: MP_TURN), into its new spot; the cloud is never still. The still layers (old lines, old
 * grains with no pair, new grains coming up on their spot, new lines) turn at scattered moments from lists made once,
 * by when (mlist_t): a frame touches only what turns in it. The new lines come in over the dust's last landings, full
 * just after the last grain is down. */
#define MP_LEAVE      0.08f    /* a pair's old grain leaves its spot at 0 .. this ..                                */
#define MP_MID        0.54f    /* .. passes its cloud point at this (and up to MP_MID1 later: its new grain is known by
                                *  then, the clock slows before it till it is) ..                                    */
#define MP_MID1       0.08f
#define MP_LAND       0.36f    /* .. and lands this much later (and up to MP_LAND1 more): all down by 1.04 s         */
#define MP_LAND1      0.06f
#define MP_START      2.5f     /* its speed leaving, at its cloud point, as a share of its leg's mean (an ease out    */
#define MP_TURN       0.5f     /*  that never stops: the dust flows from the dissolve straight into the gathering)    */
#define MP_TOWARD     0.3f     /* its cloud point: this far from the old spot to the new ..                         */
#define MP_SPREAD     0.6f     /* .. moved this far toward a point of a disc round the centre ..                   */
#define MP_DISC       222.f    /* .. this wide (px of a 466 px glass) ..                                           */
#define MP_RIM        226.f    /* .. and from past this, back into the band 0.72 .. 1 of it                        */
#define MP_FLOW       9.f      /* px: the turbulence (a shared flow and each grain's own shimmer, by sin(pi x flight)) */
#define MP_OLD_LINES0 0.f      /* the old lines go out over this .. this (a cell's together, the cells scattered)    */
#define MP_OLD_LINES1 0.32f
#define MP_SPARE0     0.03f    /* an old grain with no pair goes out where it is, each at its own moment in this, more
                                *  of them late (and the new ones come up more of them early: the dust stays as full) */
#define MP_SPARE1     0.48f    /* (all out as the new come up: a new grain never lands where an old one still is)     */
#define MP_BORN0      0.48f    /* a new grain with no pair comes up on its spot, each at its own moment in this ..  */
#define MP_BORN1      0.90f
#define MP_GLIDE0     0.46f    /* .. but one in MP_GLIDERS: it starts in this, ..                                   */
#define MP_GLIDE1     0.70f
#define MP_BORN_FADE  0.08f    /* .. comes up within this, MP_NEAR .. 2 MP_NEAR px from its spot, ..                */
#define MP_BORN_GLIDE 0.2f     /* .. and glides in over this                                                     */
#define MP_NEAR       10.f
#define MP_GLIDERS    16
#define MP_NEW_LINES0 0.85f    /* the new lines come in over this .. this (as the old: a cell's together), over the  */
#define MP_NEW_LINES1 1.12f    /*  last landings                                                                      */
#define MP_LEN        1.12f    /* the change's length (the dust down by 1.04, the lines full 2 frames after)         */
#define MP_GONE       0.2f     /* a pair with no new grain fades in the cloud over this, from its cloud point on    */
#define MP_KNOWN      0.2f     /* the pull toward the new spot comes in over this, once the new picture is read    */
#define MP_FPS        30.f     /* the change's frames a second (one step a frame drawn, never two)                 */
#define MP_RATE       4        /* its clock goes in quarter frames: 4 a frame; while the new look's dust is not in at
                                * its start (built late): MP_SLOW a frame till paired, the dissolve a little slower    */
#define MP_SLOW       3
#define MP_NEAR_S     0.1f     /* .. and this long before what it waits for (the pairs' cloud points, the new lines):
                                * 2 a frame, then 1, then it stands (never a jump: the cloud swirls on)               */
#define MP_NQ         192      /* the lists' moments: quarter frames of the clock, 1.6 s                            */
/* the panel model's cost of the change's own work, us (fluid_morph_t.work), fitted to the device (bench1: the old core's
 * parts against its counts): a pixel read from the kept glass and sorted out, a grain or line pixel kept, a key put in
 * order (each of a sort's three passes), a mover that moved (worked out, cleared, drawn: its random PSRAM touches), a
 * still grain or line pixel turned (a list's entry), one looked at and passed, a pixel of the new look drawn whole */
static float s_mwk = 1.f;                                /* (tests: the device's costs, this many times these: fluid_draw_morph_bounds) */
#define MW_READ       (0.25f * s_mwk)
#define MW_ITEM       (0.15f * s_mwk)
#define MW_SORT       (0.5f * s_mwk)
#define MW_FLY        (3.5f * s_mwk)
#define MW_BORN       (0.6f * s_mwk)
#define MW_PX         (1.0f * s_mwk)   /* a pixel written into the kept glass (a random PSRAM line: bench2's fly + compose, by px) */
#define MW_LOOK       (0.05f * s_mwk)
#define MW_SHAPE      (0.5f * s_mwk)
#define MORPH_READ_CELLS 6                       /* (the new picture read a few cells at a time) */

/* a grain: its place in its cell (dy << 4 | dx) and its size (MG_Z2: 2 x 2 px, else 1); MG_NONE: no pair */
#define MG_Z2   0x100u
#define MG_NONE 0xFFFFu
typedef struct {
    int n, cap;
    uint16_t *g, *col, *ref;           /* each grain: its place, its colour (rgb565 BE), the pair it flies in (MG_NONE) */
    uint8_t  *aux;                     /* 2 a grain, if it has no pair: when it starts (the old: to fade, quarter frames;
                                        * the new: to come up, 0..255 of its span) and its steps' own moment          */
    uint32_t *at;                      /* N + 1: each cell's first grain (the grains go by cell)                        */
} mlayer_t;
typedef struct {
    int n, cap;
    uint8_t  *o; uint16_t *col;        /* each line pixel: its place in its cell (dy << 4 | dx), its colour             */
    uint32_t *at;                      /* N + 1: each cell's first                                                    */
} mlines_t;
typedef struct {                       /* a pair: an old grain flying to a new one                                    */
    int16_t  x0, y0, x1, y1;           /* fb px                                                                       */
    int16_t  qx, qy;                   /* its cloud point without the pull to the new spot, 1/8 px                    */
    uint16_t a, b, d;                  /* leaves, passes its cloud point, lands: ms                                   */
    uint16_t c0, ib;                   /* its old colour; its new grain (MG_NONE: not read yet, or none: it fades in the cloud) */
    uint8_t  z0, z1, ph, r;            /* its sizes; its shimmer's phase; where past the rim it goes back to            */
    int16_t  x, y;                     /* as drawn (z 0: not shown)                                                   */
    uint16_t col;
    uint8_t  z, gone;                  /* gone: 1 it has no new grain (fewer than read: it fades in the cloud); 2 left
                                        * its old spot (drawn as a mover since); 4 at rest                            */
    uint16_t un[4];                    /* what it covers where it is drawn (mdraw)                                    */
    float    ib_a, id_b, id_a;         /* 1 / its two legs, its flight (fly_inv)                                       */
} mfly_t;
typedef struct { int16_t x1, y1; uint16_t ib; uint8_t z1; } mtgt_t;   /* a pair's new grain, as the reader chose it */
typedef struct {                       /* a new grain gliding in                                                      */
    int8_t   ox, oy, px, py;           /* where it starts (1/4 px from its spot), where it is drawn (px from its spot)  */
    uint8_t  lv;                       /* 1: shown; 0x20: at rest on its spot; 0x40: its start worked out              */
    uint16_t un[4];                    /* what it covers                                                              */
} mglide_t;
/* A still layer's pixels as they turn (off: col NULL; on: each its colour), each at its own moment: by quarter frame of
 * the clock, at[q] .. at[q + 1]; in a moment by place (its writes go down the glass). Made once by whoever read that
 * picture; the drawer goes through it, a moment at a time (cur, q: the next). */
typedef struct {
    uint32_t *e;                       /* each: its pixel, y << 10 | x; bit 31: a 2 x 2 px grain                       */
    uint16_t *col;
    uint32_t at[MP_NQ + 1];
    int      n, q;
} mlist_t;
typedef struct { int pass, k, g; } mwalk_t;   /* (a list made over the cells in two passes: where it is; g: gliders met) */
typedef struct {                       /* a sort's scratch (each picture's its own: the drawer sorts the old, the reader the new) */
    uint32_t *tmp, cnt[65];            /* (as long as its keys; its counts, where its keys go)                          */
    int      spos, pass;               /* (its keys moved so far in this pass; the passes done, 3: in order)            */
    bool     counted;
} msort_t;
struct fluid_morph_mem {
    mlayer_t A, B;                     /* the old picture's dust, the new one's                                        */
    mlines_t L0, L1;                   /* their lines                                                                 */
    mfly_t   *fly; int nf;
    uint32_t *bkey;                    /* (reading) the new grains' places on the curve                               */
    uint32_t *akey;                    /* the old grains' places on the curve (sorted: the pairs' order)               */
    msort_t  sa, sb;                   /* the old picture's sort (the drawer's), the new one's (the reader's)          */
    int      wpx;                      /* (pixels written into the glass this frame: the panel model's)                */
    bool     fframe;                   /* (the dust's frame worked out for its specks read late)                       */
    fluid_morph_mem_t *prev;           /* (while the old picture is read in the middle of a change: that change)      */
    int      oj, dj, di;               /* the old picture read to this cell row; the new one's specks (and cell in it)   */
    bool     dust, lines_ok, waits, passed, started, again;   /* the new one read with its dust, its lines; the clock
                                        * held back; the drawer's pass for it made; the clock runs; a change in the
                                        * middle of one                                                               */
    int64_t  f0;                       /* this frame's start (its budget: fluid_morph_t.budget)                        */
    float    shx[256], shy[256];       /* this frame's shimmer by phase                                               */
    uint16_t v[256]; uint8_t dm[256];  /* a cell's pixels while a picture is read: here, not on the draw task's stack    */
    bool     chosen, planned, clock, last, paired, slow;   /* the pairs chosen; and the old picture's lists made; its
                                        * clock started (the drawer's pass made); this frame ends it; each pair its new
                                        * grain; the clock slower till then                                           */
    int      rate;                     /* the clock's quarter frames this frame (MP_RATE, less while it waits)         */
    uint8_t  *nlc, *stale, *bd;        /* N: (reading) where lines may be; drawn whole at the end; their dust's dots read */
    uint8_t  *rows;                    /* fb_h: the pixel rows written this frame (sent)                               */
    uint8_t  *dmg;                     /* N: (the drawer's) cells where a mover came to rest over a lit pixel: written
                                        * again at the end                                                            */
    uint16_t *lbuf, *gbuf; int sw;     /* a cell row's lines and dust as read (sw x the tallest cell)                 */
    float    tk;                       /* when the new picture was paired (s; < 0: from the start)                    */
    int      bj, bi;                   /* the new picture read to this cell row, and cell in it                        */
    bool     bset;                     /* .. its reading set up (where its lines may be)                               */
    bool     keep;                     /* (fluid_morph_t.keep: its lines stay)                                         */
    bool     hd;                       /* its new picture is a scene's in high definition: the drawer's pass says (morph_pass) */
    int      lod; bool lfit;           /* (lines_on_dust's next cell; the new lines' arrays fitted)                     */
    uint32_t *lodt;                    /* (lines_on_dust's table of a cell's places: 512)                               */
    uint8_t  *gl;                      /* (a scene's lines read after its dust) each new grain under a line: in the line's
                                        * pixel                                                                       */
    int      rw;                       /* the reader's work since its caller last took it (the panel model's us)       */
    /* Who reads the new picture: the drawer in its frames, or (job) the other task (fluid_draw_morph_work) from the
     * handoff that gave it the change (its new look made) on. The reader alone writes what it reads into (B, L1, gl, the
     * keys and the sort's scratch, nlc, stale, bd, the read's places, dust, lines_ok, the pairs' new grains tg, the
     * lists nb and nl, the gliders, the drawing's cache it makes for the lines) and the job's own fields; the drawer sees
     * it only as `got`, set at a handoff (or, in line, right after its own read), and never frees a change the job has
     * but at a handoff (fluid_morph_t.jfree). The old picture's lists (os, ol) are the drawer's. */
    bool     job, jdone, jfail, jready, jhurry;   /* (the job has the change; it read all; out of room (either reader);
                                        * the job's copies of fluid_morph_t.ready and .jhurry, taken at the handoff)   */
    bool     bready, lready;           /* (the reader's) the pairs' new grains chosen and the new grains' list made; the
                                        * new lines' list made                                                        */
    bool     knf;                      /* (the reader's to read) the pairs are chosen (nf): set in line, or at a handoff  */
    uint8_t  got;                      /* what of the read the drawer may use (MGOT_*)                                  */
    int      jn, jus, jmax, jdus, jlus;   /* (the job's slices, their us, the longest; the reader's us on the dust, the lines) */
    mtgt_t   *tg; int nk;              /* each pair's new grain (ib MG_NONE: none, it fades in the cloud), those kept    */
    uint32_t *bl; int ngl;             /* the new grains that glide in (n | k << 14, in order), their state             */
    mglide_t *gst;
    int      cost_draw, cost_was;      /* (the movers' drawing last frame and the one before, us: the still layers leave
                                        * it room, and its growth again)                                              */
    float    lk;                       /* (the still layers' us against the model's, its peaks held: their moments' room) */
    mlist_t  os, ol, nb, nl, nw;       /* the old grains with no pair going out, the old lines going, the new grains coming
                                        * up on their spot, the new lines coming, the cells drawn whole with them      */
    int      olk, olp;                 /* (the old lists made to this cell, in this pass: a share of a frame at a time)  */
    mwalk_t  wg, wl;                   /* (the reader's) the new grains' list and the new lines' made this far (lists_walk) */
    int      wz, za;                   /* (the reader's) the new grains' sizes summed this far, their px                 */
    int      jpart[FLUID_MJ_N];              /* (the job's) this slice's us by step (MJ_*: fluid_morph_t.jslow keeps the longest's) */
};
enum { MGOT_DUST = 1, MGOT_LINES = 2, MGOT_DONE = 4, MGOT_FAIL = 8 };   /* (the new dust read, in order, the pairs' new
                                        * grains chosen, its lists made; its lines read and listed; the reader done
                                        * with the change; out of room) */

static inline uint32_t mhash(uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}
static inline float mrand(uint32_t a, uint32_t b) { return (float)(mhash(a, b) >> 8) * (1.f / 16777216.f); }
static inline float msin(float x) { return fluid_sin_turns(x * 0.15915494f); }
static inline float mclamp(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }
static inline float msstep(float a, float b, float t) { const float u = mclamp((t - a) / (b - a)); return u * u * (3.f - 2.f * u); }
/* an rgb565 (big-endian) colour a + (b - a) m, each channel rounded */
static inline uint16_t mmix(uint16_t a, uint16_t b, float m) {
    if (m <= 0.f || a == b) return a;
    if (m >= 1.f) return b;
    const int A = (uint16_t)(a >> 8 | a << 8), B = (uint16_t)(b >> 8 | b << 8);
    const int r = (int)((A >> 11) + ((B >> 11) - (A >> 11)) * m + 0.5f), g = (int)((A >> 5 & 63) + ((B >> 5 & 63) - (A >> 5 & 63)) * m + 0.5f);
    const int bl = (int)((A & 31) + ((B & 31) - (A & 31)) * m + 0.5f);
    const uint16_t o = (uint16_t)(r << 11 | g << 5 | bl);
    return (uint16_t)(o >> 8 | o << 8);
}
/* a line pixel's own moment in its fade: the fractional part of its pixel index x 0.618 */
static inline int cell_ph(int k) { return (int)(((uint32_t)k * 2654435769u) >> 24); }   /* (each cell's lines at its own moment) */

/* ── its memory ── */
static bool mlayer_new(mlayer_t *L, int cap, int N, bool aux) {
    L->n = 0; L->cap = cap;
    L->g = FLUID_MALLOC(sizeof(uint16_t) * 3 * (size_t)cap); L->at = FLUID_CALLOC(sizeof(uint32_t) * (size_t)(N + 1));   /* (a cell not read: none) */
    L->aux = aux ? FLUID_MALLOC(2 * (size_t)cap) : NULL;
    if (!L->g || !L->at || (aux && !L->aux)) return false;
    L->col = L->g + cap; L->ref = L->col + cap;
    return true;
}
/* (once read: to its size; its arrays one block, moved together) */
static void mlayer_fit(mlayer_t *L) {
    if (L->n >= L->cap || !L->g) return;
    memmove(L->g + L->n, L->col, sizeof(uint16_t) * (size_t)L->n);
    memmove(L->g + 2 * L->n, L->ref, sizeof(uint16_t) * (size_t)L->n);
    uint16_t *p = FLUID_REALLOC(L->g, sizeof(uint16_t) * 3 * (size_t)(L->n ? L->n : 1));
    if (p) L->g = p;
    L->cap = L->n; L->col = L->g + L->n; L->ref = L->col + L->n;
}
static bool mlines_new(mlines_t *L, int cap, int N) {
    L->n = 0; L->cap = cap;
    L->o = FLUID_MALLOC(3 * (size_t)cap + 2); L->at = FLUID_CALLOC(sizeof(uint32_t) * (size_t)(N + 1));
    L->col = L->o ? (uint16_t *)(void *)(L->o + ((cap + 1) & ~1)) : NULL;   /* (the colours after the places, aligned) */
    return L->o && L->at;
}
static void mlines_fit(mlines_t *L) {
    const int a = (L->n + 1) & ~1;
    if (!L->o || a >= ((L->cap + 1) & ~1)) return;
    memmove(L->o + a, L->col, sizeof(uint16_t) * (size_t)L->n);
    uint8_t *p = FLUID_REALLOC(L->o, (size_t)a + 2 * (size_t)L->n + 2);
    if (p) L->o = p;
    L->cap = L->n; L->col = (uint16_t *)(void *)(L->o + a);
}
static inline bool lines_add(mlines_t *L, int p, uint16_t c) {
    if (L->n >= L->cap) return false;
    L->o[L->n] = (uint8_t)p; L->col[L->n++] = c;
    return true;
}
static void mm_free(fluid_morph_mem_t *mm) {
    if (!mm) return;
    mm_free(mm->prev);
    FLUID_FREE(mm->A.g); FLUID_FREE(mm->A.at); FLUID_FREE(mm->A.aux); FLUID_FREE(mm->B.g); FLUID_FREE(mm->B.at); FLUID_FREE(mm->B.aux);
    FLUID_FREE(mm->akey); FLUID_FREE(mm->sa.tmp); FLUID_FREE(mm->sb.tmp);
    FLUID_FREE(mm->L0.o); FLUID_FREE(mm->L0.at); FLUID_FREE(mm->L1.o); FLUID_FREE(mm->L1.at);
    FLUID_FREE(mm->fly); FLUID_FREE(mm->bkey); FLUID_FREE(mm->nlc); FLUID_FREE(mm->lbuf); FLUID_FREE(mm->gl); FLUID_FREE(mm->lodt); FLUID_FREE(mm->bl); FLUID_FREE(mm->gst);
    FLUID_FREE(mm->tg); FLUID_FREE(mm->os.e); FLUID_FREE(mm->ol.e); FLUID_FREE(mm->nb.e); FLUID_FREE(mm->nl.e); FLUID_FREE(mm->nw.e);
    FLUID_FREE(mm);
}
static fluid_morph_mem_t *mm_new(const fluid_draw_t *d) {
    const int W = d->W, H = d->H, N = W * H;
    int ch = 0;
    for (int j = 0; j < H; j++) if (d->cy[j + 1] - d->cy[j] > ch) ch = d->cy[j + 1] - d->cy[j];
    fluid_morph_mem_t *mm = FLUID_CALLOC(sizeof(*mm));
    if (!mm) return NULL;
    mm->sw = d->fb_w;
    const bool ok = mlayer_new(&mm->A, FLUID_MORPH_GRAINS, N, false) && mlines_new(&mm->L0, FLUID_MORPH_LINES, N);
    mm->fly = FLUID_MALLOC(sizeof(mfly_t) * FLUID_MORPH_FLY);
    mm->nlc = FLUID_CALLOC(4 * (size_t)N + (size_t)d->fb_h);
    mm->lbuf = FLUID_MALLOC(sizeof(uint16_t) * 2 * (size_t)mm->sw * (size_t)ch);
    mm->akey = FLUID_MALLOC(sizeof(uint32_t) * FLUID_MORPH_GRAINS);
    if (!ok || !mm->fly || !mm->nlc || !mm->lbuf || !mm->akey) { mm_free(mm); return NULL; }
    mm->stale = mm->nlc + N; mm->bd = mm->stale + N; mm->dmg = mm->bd + N; mm->rows = mm->dmg + N;
    mm->gbuf = mm->lbuf + (size_t)mm->sw * ch;
    mm->tk = -1.f; mm->rate = MP_RATE;
    return mm;
}
/* (the cells' maps and the creature's line cells: once) */
static bool morph_maps(fluid_draw_t *d) {
    fluid_morph_t *m = &d->m;
    if (m->colx) return true;
    const int N = d->W * d->H;
    uint8_t *p = FLUID_CALLOC((size_t)d->fb_w + d->fb_h + 3 * (size_t)N);
    if (!p) return false;
    m->colx = p; m->rowy = p + d->fb_w; m->lc = m->rowy + d->fb_h; m->lcn = m->lc + N; m->ord = m->lcn + N;
    memset(m->colx, 255, (size_t)d->fb_w); memset(m->rowy, 255, (size_t)d->fb_h); memset(m->ord, 255, (size_t)N);
    for (int i = 0; i < d->W; i++) for (int x = d->cx[i]; x < d->cx[i + 1]; x++) m->colx[x] = (uint8_t)i;
    for (int j = 0; j < d->H; j++) for (int y = d->cy[j]; y < d->cy[j + 1]; y++) m->rowy[y] = (uint8_t)j;
    return true;
}

void fluid_draw_morph_lines(fluid_draw_t *d, const uint8_t *cells) {
    if (!d || !morph_maps(d)) return;
    if (cells) memcpy(d->m.lcn, cells, (size_t)d->W * d->H); else memset(d->m.lcn, 0, (size_t)d->W * d->H);
}
void fluid_draw_morph_order(fluid_draw_t *d, const uint8_t *order) {
    if (!d || !morph_maps(d)) return;
    if (order) memcpy(d->m.ord, order, (size_t)d->W * d->H); else memset(d->m.ord, 255, (size_t)d->W * d->H);
}
bool fluid_draw_morph_carries(const fluid_draw_t *d) { return d && d->m.snap && (d->m.on || d->m.whole) && d->m.colx && d->m.fulls < 2; }

/* the job no longer reads for the change (at the handoff, or in line: that task is between slices); it is the drawer's */
static void job_release(fluid_draw_t *d) {
    fluid_morph_t *m = &d->m;
    if (m->jmm) m->jmm->job = false;
    m->jmm = NULL;
}
static void morph_got(fluid_draw_t *d);
static void morph_end(fluid_draw_t *d, bool cut) {
    fluid_morph_t *m = &d->m;
    fluid_morph_mem_t *mm = m->mm;
    if (m->on) {                                        /* (its stages for the log) */
        if (mm && !mm->job) morph_got(d);
        m->dg[FLUID_MD_FRAMES] = m->frames;
        m->ends++;
    }
    m->on = false;
    m->mm = NULL;
    if (mm && mm->job) {                                /* the job's (the drawer's frame): freed at the handoff, and till then, */
        mm->prev = m->jfree; m->jfree = mm;             /* if it may still read, nothing of the look drawn (it may be in the */
        if (!(mm->got & MGOT_DONE)) { m->jhold = true; cut = true; }   /* drawing's cache, the dust's frame) */
    } else mm_free(mm);
    if (cut) d->force = m->full = true;                 /* (the next frame sends all of the look: the glass is the look's then) */
    m->whole = (m->look & (FLUID_LOOK_KEEP | FLUID_LOOK_GLASS)) && m->snap;
    if (m->forced && !cut) m->look &= ~FLUID_LOOK_DUST; /* (read without its dust: that comes as a change of its own) */
    m->forced = m->keep = false;
}

void fluid_draw_morph_look(fluid_draw_t *d, uint32_t look, uint16_t *snap, bool ready) {
    if (!d) return;
    fluid_morph_t *m = &d->m;
    if (snap != m->snap) { if (m->on) { job_release(d); morph_end(d, true); } m->snap = snap; m->whole = false; }
    if (m->on) { m->frames++; if (ready && !m->ready) m->ready_at = m->frames; m->ready |= ready; }
    if (look == m->look) return;
    /* its dust alone: nothing while a change runs (its picture is read with it, or it comes after); gained after one, it
     * comes as a change of its own, the lines staying */
    if (!((look ^ m->look) & ~FLUID_LOOK_DUST)) {
        const bool gain = (look & FLUID_LOOK_DUST) && !m->on && m->whole && snap && morph_maps(d);
        m->look = look;
        if (gain) { m->ready = true; m->ready_at = 0; m->frames = 0; m->na = -1; m->mm = NULL; m->on = m->keep = true; m->forced = m->jhurry = false; m->held = 0; m->nheld = 0; m->seen_hd = false; m->pwait = 0; memset(m->dg, 0, sizeof(m->dg)); }
        return;
    }
    /* nothing kept to change from, or into a look the panel's mirror keeps (it writes over the kept glass): at once */
    job_release(d);                                     /* (a change ends or another starts: the reading so far is the drawer's) */
    if (!look || (look & FLUID_LOOK_GLASS) || !snap || !(m->on || m->whole) || !morph_maps(d)) {
        if (m->on) morph_end(d, true);
        else d->force = true;
        m->look = look; m->whole = (look & (FLUID_LOOK_KEEP | FLUID_LOOK_GLASS)) && snap;
        return;
    }
    /* the old picture is read from the glass at the next cells pass (fluid_draw_cells: morph_read_old), before the new
     * look's inputs reach the drawer's own state */
    m->look = look; m->ready = ready; m->ready_at = ready ? 0 : -1; m->frames = 0; m->na = -1;
    if (!m->on) m->mm = NULL;
    m->on = true; m->keep = m->forced = m->jhurry = false; m->held = 0; m->nheld = 0;
    memset(m->dg, 0, sizeof(m->dg)); memset(m->top, 0, sizeof(m->top));
    m->seen_hd = false; m->pwait = 0;
}

/* ── the pictures read ── */

/* each grain of a cell's dust (px flagged in dm, colours in v; the cell w x h, rows 16 apart): 2 x 2 where four of the same
 * colour meet, else one pixel. dm is all clear after (the next cell's) */
static void grains_of(mlayer_t *L, const uint16_t *v, uint8_t *dm, int w, int h) {
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        const int p = y << 4 | x;
        if (!dm[p]) continue;
        if (L->n >= L->cap) { memset(dm, 0, 256); return; }
        uint16_t g = (uint16_t)p;
        if (x + 1 < w && y + 1 < h && dm[p + 1] && dm[p + 16] && dm[p + 17] && v[p + 1] == v[p] && v[p + 16] == v[p] && v[p + 17] == v[p]) {
            dm[p + 1] = dm[p + 16] = dm[p + 17] = 0; g |= MG_Z2;
        }
        dm[p] = 0;
        L->g[L->n] = g; L->col[L->n] = v[p]; L->ref[L->n] = MG_NONE; L->n++;
    }
}
/* is (x, y) in one of the dots of cell k's key (key & 255)? */
static inline bool in_dot(const fluid_draw_t *d, int i, int x, int y, unsigned dots) {
    const int r = d->yrow[y];
    if (r < 0) return false;
    for (int c = 0; c < 2; c++)
        if ((dots >> fluid_braille_bit(c, r) & 1u) && x >= d->dx[i * 2 + c] && x < d->dx[i * 2 + c] + d->dot_w) return true;
    return false;
}
static uint32_t hilbert(int x, int y);
static inline uint32_t grain_key(int x, int y, int n);
/* a moment of the clock (s) as a list's quarter frame; a cell's lines' own moment in a .. b (cell_ph: scattered) */
static inline int mq(float s) { const int q = (int)(s * (4.f * MP_FPS)); return q < 0 ? 0 : q >= MP_NQ ? MP_NQ - 1 : q; }
static inline int cell_q(int k, float a, float b) { return mq(a + (b - a) * cell_ph(k) * (1.f / 256.f)); }
/* has a list gone through moment q yet (its entries there turned)? */
static inline bool mlist_past(const mlist_t *L, int q) { return L->e && q < L->q; }
/* The old picture from the glass, with the drawer's state as it drew it: its dust (the dots of dust cells, a scene's specks:
 * what is not its drawing) and its lines (the rest). In the middle of a change, the lines are that change's old lines
 * still lit. A cell row at a time, `budget` us a frame at most (the drawer stands meanwhile: the glass and its state do
 * not change). 1: read whole, 0: not yet, -1: no room. */
static int morph_read_old(fluid_draw_t *d, int budget) {
    fluid_morph_t *m = &d->m;
    fluid_morph_mem_t *mm = m->mm;
    const int W = d->W, H = d->H, fw = d->fb_w;
    if (!mm || mm->oj >= H) {                           /* (a fresh read; the change shown, if any, is its `prev`) */
        fluid_morph_mem_t *n = mm_new(d);
        if (!n) return -1;
        n->prev = mm; m->mm = mm = n;
    }
    const fluid_morph_mem_t *prev = mm->prev;
    uint16_t *const v = mm->v;
    uint8_t *const dm = mm->dm;
    const int64_t t_in = now_us();
    int64_t row = 0;                                    /* (the last row's time and work: the next is read only when it fits) */
    int wrow = 0;
    for (const int j0 = mm->oj; mm->oj < H && (mm->oj == j0 || (now_us() - t_in + row < budget && m->work + wrow < budget)); mm->oj++) {
        const int j = mm->oj;
        const int64_t r0 = now_us();
        const int w0 = m->work;
        for (int i = 0; i < W; i++) {
            const int k = j * W + i, x0 = d->cx[i], y0 = d->cy[j], w = d->cx[i + 1] - x0, h = d->cy[j + 1] - y0;
            const uint16_t key = d->key[k];
            mm->A.at[k] = (uint32_t)mm->A.n; mm->L0.at[k] = (uint32_t)mm->L0.n;
            for (int y = y0; y < y0 + h; y++) {
                const uint16_t *row = m->snap + (size_t)y * fw;
                for (int x = x0; x < x0 + w; x++) {
                    const uint16_t c = row[x];
                    if (!c) continue;
                    const int p = (y - y0) << 4 | (x - x0);
                    bool line;
                    if (m->keep) line = true;           /* (its dust alone comes: all of it stays as it is) */
                    else if (prev) {                    /* (a change in the middle of one: its old lines still lit) */
                        line = false;
                        for (uint32_t q = prev->L0.at[k]; q < prev->L0.at[k + 1] && !line; q++) line = prev->L0.o[q] == p;
                        line = line && (prev->keep || !mlist_past(&prev->ol, cell_q(k, MP_OLD_LINES0, MP_OLD_LINES1)));
                    } else if (key == KEY_EMPTY) line = false;
                    else if (key_hd(key)) {             /* a scene: its drawing (cached) and the dots it keeps, not its specks */
                        line = (d->hdc && d->hdcv && d->hdcv[k] && d->hdc[(size_t)y * fw + x] == c) ||
                               (c == d->col[k] && in_dot(d, i, x, y, key & 255u & ~(unsigned)(d->sd_hard ? d->sd_hard[k] : 0)));
                    } else if (key < KEY_GLYPH) line = !(c == d->col[k] && in_dot(d, i, x, y, key) && !m->lc[k]);   /* a dust dot */
                    else line = true;                   /* a letter */
                    if (!line || !lines_add(&mm->L0, p, c)) { dm[p] = 1; v[p] = c; }   /* (no room for a line: dust) */
                }
            }
            const int n0 = mm->A.n;
            grains_of(&mm->A, v, dm, w, h);
            for (int n = n0; n < mm->A.n; n++)          /* (its place on the curve) */
                mm->akey[n] = grain_key(x0 + (mm->A.g[n] & 15), y0 + (mm->A.g[n] >> 4 & 15), n);
            m->work += (int)(w * h * MW_READ + (mm->A.n - n0) * MW_ITEM);
        }
        row = now_us() - r0; wrow = m->work - w0;
    }
    if (mm->oj < H) return 0;
    mm->A.at[W * H] = (uint32_t)mm->A.n; mm->L0.at[W * H] = (uint32_t)mm->L0.n;
    mlayer_fit(&mm->A); mlines_fit(&mm->L0);
    mm_free(mm->prev); mm->prev = NULL;
    mm->keep = m->keep;
    m->na = mm->A.n; m->nb = -1; m->nlines = mm->L0.n; m->nfly = 0;
    return 1;
}

/* is a pixel the drawing's own (a speck goes on black or a dim pixel only: fluid_fine.c put) */
static inline bool speck_refused(uint16_t be) {
    const int le = (uint16_t)(be >> 8 | be << 8);
    return (le >> 11) > FINE_DRAWN >> 3 || (le >> 5 & 63) > FINE_DRAWN >> 2 || (le & 31) > FINE_DRAWN >> 3;
}
static inline uint16_t speck_add(uint16_t be, uint16_t c, int lv) {
    int r = c >> 11, g = c >> 5 & 63, b = c & 31;
    if (lv < 255) { r = (r * lv + 127) / 255; g = (g * lv + 127) / 255; b = (b * lv + 127) / 255; }
    const int le = (uint16_t)(be >> 8 | be << 8);
    r += le >> 11; g += le >> 5 & 63; b += le & 31;
    const uint16_t o = (uint16_t)((r > 31 ? 31 : r) << 11 | (g > 63 ? 63 : g) << 5 | (b > 31 ? 31 : b));
    return (uint16_t)(o >> 8 | o << 8);
}
static void draw_rect_layer(const fluid_draw_t *d, const fluid_draw_opts_t *o, const fluid_rect_t *t, bool lines_only);
/* a cell's lines by its key alone: a creature's own dots, a letter's glyph or its cleared cell */
static inline bool lines_now(const fluid_draw_t *d, int k) {
    const uint16_t key = d->key[k];
    return !key_hd(key) && key != KEY_EMPTY && (key >= KEY_GLYPH || d->m.lc[k]);
}
/* the cells of the look that may have lines: those, a scene's kept dots, and where its shapes or letters reach (near) */
static inline bool has_lines(const fluid_draw_t *d, int k, const uint8_t *near) {
    return lines_now(d, k) || (key_hd(d->key[k]) && (d->key[k] & 255u)) || near[k];
}
/* a scene's specks of cell k (i, j) into gb (sw wide, its row's buffer from y0) where lb (if any) has no line, else into
 * the line pixel; without lb, a line pixel is found among the cell's lines read (L1) */
static void specks_of(const fluid_draw_t *d, fluid_morph_mem_t *mm, const fluid_fine_t *f, int i, int j, int y0, int h, uint16_t *lb, uint16_t *gb, int sw) {
    const int W = d->W, k = j * W + i;
    const fine_set_t *st = f->drawn;
    const int dx = f->sx[k], dy = f->sy[k], xa = f->cx[i], xb = f->cx[i + 1], ya = f->cy[j], yb = f->cy[j + 1];
    for (uint32_t q = st->start[k]; q < st->start[k + 1]; q++) {
        const int lv = f->lv[q];
        if (!lv) continue;
        int X = (int)(st->pos[q] & 511) + dx, Y = (int)(st->pos[q] >> 9 & 511) + dy;
        if (X < xa) X += xb - xa; else if (X >= xb) X -= xb - xa;
        if (Y < ya) Y += yb - ya; else if (Y >= yb) Y -= yb - ya;
        const int x = X + f->ox, y = Y + f->oy;
        if (y < y0 || y >= y0 + h || x < 0 || x >= sw) continue;
        uint16_t *l = lb ? &lb[(y - y0) * sw + x] : NULL, *g = &gb[(y - y0) * sw + x];
        if (!lb && mm->keep && d->m.snap[(size_t)y * d->fb_w + x]) {   /* (on the kept glass's line: a line pixel of its own) */
            const uint8_t p = (uint8_t)((y - d->cy[j]) << 4 | (x - d->cx[i]));
            uint32_t r = mm->L1.at[k];
            while (r < (uint32_t)mm->L1.n && mm->L1.o[r] != p) r++;
            if (r == (uint32_t)mm->L1.n && !lines_add(&mm->L1, p, d->m.snap[(size_t)y * d->fb_w + x])) continue;
            if (!speck_refused(mm->L1.col[r])) mm->L1.col[r] = speck_add(mm->L1.col[r], st->col[q], lv);
            continue;
        }
        if (!lb && mm->lines_ok && x >= d->cx[i] && x < d->cx[i + 1]) {   /* (its line, read before) */
            const uint8_t p = (uint8_t)((y - d->cy[j]) << 4 | (x - d->cx[i]));
            for (uint32_t r = mm->L1.at[k]; r < mm->L1.at[k + 1] && !l; r++) if (mm->L1.o[r] == p) l = &mm->L1.col[r];
        }
        if (l && *l) { if (!speck_refused(*l)) *l = speck_add(*l, st->col[q], lv); }
        else *g = speck_add(*g, st->col[q], lv);
    }
}
/* cell k's dust as gathered in gb (its row's buffer from y0) into its grains, each its place on the curve */
static void cell_grains(const fluid_draw_t *d, fluid_morph_mem_t *mm, int i, int j, const uint16_t *gb, int sw) {
    const int k = j * d->W + i, x0 = d->cx[i], y0 = d->cy[j], w = d->cx[i + 1] - x0, h = d->cy[j + 1] - y0;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        const uint16_t g = gb[y * sw + x0 + x];
        if (g) { mm->dm[y << 4 | x] = 1; mm->v[y << 4 | x] = g; }
    }
    mm->B.at[k] = (uint32_t)mm->B.n;
    if (mm->B.n >= mm->B.cap) for (int q = 0; q < 256; q++) if (mm->dm[q]) { mm->stale[k] = 1; break; }
    grains_of(&mm->B, mm->v, mm->dm, w, h);
    for (int n = (int)mm->B.at[k]; n < mm->B.n; n++) {   /* (its place on the curve; when it comes up if it has no pair) */
        mm->bkey[n] = grain_key(x0 + (mm->B.g[n] & 15), y0 + (mm->B.g[n] >> 4 & 15), n);
        mm->B.aux[2 * n] = (uint8_t)(mrand((uint32_t)n, 31) * 255.f + 0.5f); mm->B.aux[2 * n + 1] = (uint8_t)(mhash((uint32_t)n, 32) >> 24);
    }
}
/* a pixel's colour with a speck's added (each channel, saturated) */
static inline uint16_t madd(uint16_t a, uint16_t b) {
    const int A = (uint16_t)(a >> 8 | a << 8), B = (uint16_t)(b >> 8 | b << 8);
    const int r = (A >> 11) + (B >> 11), g = (A >> 5 & 63) + (B >> 5 & 63), bl = (A & 31) + (B & 31);
    const uint16_t o = (uint16_t)((r > 31 ? 31 : r) << 11 | (g > 63 ? 63 : g) << 5 | (bl > 31 ? 31 : bl));
    return (uint16_t)(o >> 8 | o << 8);
}
/* A scene's lines read after its dust: a speck on a line pixel is in that pixel (added, as fluid_fine_rect adds it to a dim
 * one; a bright one refuses it), its grain only shows till the lines come (gl: never drawn whole again). False: no room */
/* each line pixel onto its cell's grain at the same place (the first, in the cell's order), if any: by a table of the
 * cell's places (MG_Z2 | dy << 4 | dx: 512) made from its grains and cleared after, so the grains and the line pixels
 * are each looked at once or twice (it was every pixel against every grain of its cell: ~150,000 looks on the chart,
 * ~30 ms in one go). A cell row a step, at least one a call, in budget (time and the model's work from w0); it carries
 * on at the next call (mm->lod). 1: done, 0: more to do, -1: no memory */
static int lines_on_dust(fluid_draw_t *d, fluid_morph_mem_t *mm, int64_t t_in, int budget, int w0) {
    if (!mm->gl && !(mm->gl = FLUID_CALLOC((size_t)(mm->B.n ? mm->B.n : 1)))) return -1;
    if (!mm->lodt && !(mm->lodt = FLUID_CALLOC(sizeof(uint32_t) * 512))) return -1;
    const int W = d->W, N = W * d->H;
    uint32_t *const at = mm->lodt;
    for (bool first = true; mm->lod < N; first = false) {
        if (!first && (now_us() - t_in >= budget || mm->rw - w0 >= budget)) return 0;
        int looks = 0;
        for (const int e = mm->lod + W; mm->lod < e; mm->lod++) {
            const int k = mm->lod;
            const uint32_t a = mm->B.at[k], b = mm->B.at[k + 1];
            if (a == b || mm->L1.at[k] == mm->L1.at[k + 1]) continue;
            for (uint32_t n = b; n-- > a;) at[mm->B.g[n] & 0x1FFu] = n + 1;   /* (backwards: the first grain at a place stays) */
            for (uint32_t q = mm->L1.at[k]; q < mm->L1.at[k + 1]; q++) {
                const uint32_t o = mm->L1.o[q] & 0x1FFu, n1 = at[o];
                if (!n1) continue;
                const uint32_t n = n1 - 1;
                if (!speck_refused(mm->L1.col[q])) mm->L1.col[q] = madd(mm->L1.col[q], mm->B.col[n]);
                mm->gl[n] = 1;
                at[o] = 0;                                  /* (a grain under one line pixel only, as before) */
            }
            for (uint32_t n = a; n < b; n++) at[mm->B.g[n] & 0x1FFu] = 0;
            looks += (int)(2 * (b - a) + (mm->L1.at[k + 1] - mm->L1.at[k]));
        }
        mm->rw += (int)(looks * MW_ITEM);
    }
    return 1;
}
/* the waits' limits in frames (FLUID_MORPH_HOLD_N, FLUID_MORPH_LINES_N) and the new lines' cells read a frame at most (0:
 * by the budget alone); tests set them (fluid_draw_morph_bounds) to act out the device's slow reads */
static int s_hold_n = FLUID_MORPH_HOLD_N, s_lines_n = FLUID_MORPH_LINES_N, s_read_cells, s_walk_rows;
void fluid_draw_morph_walk_rows(int rows) { s_walk_rows = rows > 0 ? rows : 0; }
/* The new picture, a cell row at a time, as the drawer drew it in the change's one cells pass (it stands after): its
 * lines (drawn without the dust) and its dust (the dots of dust cells; a scene's specks where fluid_fine_rect puts them).
 * A scene's specks come in a second pass once they are in (`ready`), so its lines are read meanwhile; the dust's frame
 * for them takes a call of its own, and only when `fine` (room for it now). `budget` us of it (in time and in the panel
 * model's work, counted in mm->rw). By the reader alone (the drawer, or the job): it writes the reader's fields only.
 * True: a part read whole (its dust, or its lines). */
static bool morph_read_new(fluid_draw_t *d, fluid_morph_mem_t *mm, const fluid_frame_t *s, const fluid_draw_opts_t *o, int budget, bool ready, bool fine) {
    fluid_morph_t *m = &d->m;
    const int W = d->W, H = d->H, N = W * H, sw = mm->sw;
    const bool hd = mm->hd;
    const int64_t t_in = now_us();
    if (!mm->bset) {
        if ((!mm->B.g && !mlayer_new(&mm->B, FLUID_MORPH_GRAINS, N, true)) || (!mm->L1.o && !mlines_new(&mm->L1, FLUID_MORPH_LINES, N)) ||
            (!mm->bkey && !(mm->bkey = FLUID_MALLOC(sizeof(uint32_t) * FLUID_MORPH_GRAINS)))) return false;
        mm->B.n = 0; mm->L1.n = 0;
        memset(mm->nlc, 0, (size_t)N); memset(mm->bd, 0, (size_t)N);
        /* (where its letters and a scene's shapes reach, for now: each row then keeps whether it has lines; by cells) */
        const int ns = hd ? (o->nshapes < FLUID_MAX_SHAPES ? o->nshapes : FLUID_MAX_SHAPES) : 0;
        for (int n = 0; n < d->nlt + ns; n++) {
            const int16_t *b = n < d->nlt ? (const int16_t[4]){ d->lt[n].cx0, d->lt[n].cx1, d->lt[n].cy0, d->lt[n].cy1 } : d->sb[n - d->nlt];
            for (int j = 0; j < H; j++) {
                if (d->cy[j + 1] <= b[2] || d->cy[j] >= b[3]) continue;
                for (int i = 0; i < W; i++) if (d->cx[i + 1] > b[0] && d->cx[i] < b[1]) mm->nlc[j * W + i] = 1;
            }
        }
        mm->bset = true;
    }
    const int w0 = mm->rw;                              /* (the budget as the panel model counts it too: the host's) */
    if (mm->keep && !mm->bj) { memset(mm->L1.at, 0, sizeof(uint32_t) * (size_t)(N + 1)); mm->bj = H; }   /* (its lines: the glass's, kept) */
    if (!mm->dust && (hd || mm->keep) && ready) {       /* a scene's specks, once in (or the change goes on without them) */
        if (!mm->fframe && o->fine && o->fine->drawn) { /* (come since the drawer's pass: worked out as the drawer will show them) */
            if (!fine) return false;                    /* (a call of its own; at the cloud, the next) */
            o->fine->pause = false;
            fluid_fine_frame(o->fine, s, o);
            memcpy(o->fine->sig0, o->fine->sig, sizeof(uint32_t) * (size_t)N);   /* (the dust as it will be on the glass) */
            o->fine->pause = true; mm->fframe = true;
            mm->rw += FLUID_MORPH_FINE_US;
            return false;
        }
        /* (a share of a cell row at a time, 2 x MORPH_READ_CELLS cells: mm->di) */
        for (bool first = true; mm->dj < H && (first || (now_us() - t_in < budget && mm->rw - w0 < budget)); first = false) {
            const int j = mm->dj, y0 = d->cy[j], h = d->cy[j + 1] - y0, i0 = mm->di, i1 = i0 + 2 * MORPH_READ_CELLS < W ? i0 + 2 * MORPH_READ_CELLS : W;
            uint16_t *gb = mm->gbuf;
            if (!i0) memset(gb, 0, sizeof(uint16_t) * (size_t)sw * h);
            const fluid_fine_t *fs = o->fine && o->fine->drawn ? o->fine : NULL;
            if (i1 < W) mm->di = i1;
            else { mm->di = 0; mm->dj++; }
            for (int i = i0; i < i1; i++) {
                const int k = j * W + i;
                if (mm->keep) mm->L1.at[k] = (uint32_t)mm->L1.n;
                if (fs && fs->vis[k]) { specks_of(d, mm, fs, i, j, y0, h, NULL, gb, sw); mm->rw += (int)((fs->drawn->start[k + 1] - fs->drawn->start[k]) * MW_ITEM); }
                cell_grains(d, mm, i, j, gb, sw);
            }
        }
        if (mm->dj < H) return false;
        mm->B.at[N] = (uint32_t)mm->B.n;
        mlayer_fit(&mm->B);
        mm->dust = true;
        if (mm->keep) { mm->L1.at[N] = (uint32_t)mm->L1.n; mlines_fit(&mm->L1); mm->lines_ok = true; }
        return true;
    }
    if (mm->lines_ok) return false;
    /* its lines (and the dust dots of a creature's or a word's cells): a few cells of a row at a time (MORPH_READ_CELLS),
     * at least once a frame, so it always ends; a scene's lines after its dust, while the change runs (they come last) */
    const bool capped = s_read_cells > 0 && budget < INT32_MAX / 4;   /* (a test's cap; never on a wait's limit) */
    for (int read = 0; mm->bj < H && (!read || (now_us() - t_in < budget && mm->rw - w0 < budget && (!capped || read < s_read_cells))); read += MORPH_READ_CELLS) {
        const int j = mm->bj, y0 = d->cy[j], h = d->cy[j + 1] - y0, i0 = mm->bi, i1 = i0 + MORPH_READ_CELLS < W ? i0 + MORPH_READ_CELLS : W;
        uint16_t *lb = mm->lbuf, *gb = mm->gbuf;
        if (!i0) { memset(lb, 0, sizeof(uint16_t) * (size_t)sw * h); memset(gb, 0, sizeof(uint16_t) * (size_t)sw * h); }
        mm->bi = i1 < W ? i1 : 0;
        if (i1 >= W) mm->bj++;
        /* its lines: each run of cells that have any, drawn without the dust (the drawing's cache made for them first) */
        int lit = 0;
        for (int i = i0; i < i1;) {
            if (!has_lines(d, j * W + i, mm->nlc)) { i++; continue; }
            int e = i;
            while (e < i1 && has_lines(d, j * W + e, mm->nlc)) e++;
            for (int q = i; hd && o->fine && d->hdcv && q < e; q++) if (!d->hdcv[j * W + q] && key_hd(d->key[j * W + q])) {
                hdc_room(d);
                const int ncache = o->nshapes < FLUID_MAX_SHAPES ? o->nshapes : FLUID_MAX_SHAPES;
                int nc = ncache;
                while (nc > 0 && o->shapes[nc - 1].tag == FLUID_TAG_CAPTION) nc--;
                hdc_cell(d, o, j * W + q, nc);
                lit += (d->cx[q + 1] - d->cx[q]) * h;
            }
            const fluid_rect_t t = { lb + d->cx[i], sw, d->cx[i], y0, d->cx[e], y0 + h };
            draw_rect_layer(d, o, &t, true);
            mm->rw += (int)((d->cx[e] - d->cx[i]) * h * MW_READ);
            i = e;
        }
        mm->rw += (int)(lit * MW_SHAPE);
        /* the dots of dust cells, beside the lines (a scene's dust is read already) */
        for (int i = i0; i < i1 && !hd; i++) {
            const int k = j * W + i;
            const uint16_t key = d->key[k];
            if (!key_hd(key) && key && key < KEY_GLYPH && !m->lc[k]) {
                mm->bd[k] = (uint8_t)key;
                for (int y = y0; y < y0 + h; y++) for (int x = d->cx[i]; x < d->cx[i + 1]; x++)
                    if (in_dot(d, i, x, y, key) && !lb[(y - y0) * sw + x]) gb[(y - y0) * sw + x] = d->col[k];
            }
        }
        /* each cell: its line pixels, its grains (no room: the cell comes whole at the end) */
        for (int i = i0; i < i1; i++) {
            const int k = j * W + i, x0 = d->cx[i], w = d->cx[i + 1] - x0, n0 = mm->B.n + mm->L1.n;
            mm->L1.at[k] = (uint32_t)mm->L1.n;
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                const uint16_t l = lb[y * sw + x0 + x];
                if (l && !lines_add(&mm->L1, y << 4 | x, l)) mm->stale[k] = 1;
            }
            if (!hd) cell_grains(d, mm, i, j, gb, sw);
            mm->nlc[k] = (uint8_t)(mm->L1.at[k] < (uint32_t)mm->L1.n);
            mm->rw += (int)((mm->B.n + mm->L1.n - n0) * MW_ITEM);
        }
    }
    if (mm->bj < H) return false;
    if (!mm->lfit) { mm->L1.at[N] = (uint32_t)mm->L1.n; mlines_fit(&mm->L1); mm->lfit = true; }
    if (hd && mm->dust) {                               /* (a scene's specks read before: those under its lines, in them) */
        const int r = lines_on_dust(d, mm, t_in, budget, w0);
        if (r < 0) { mm->jfail = true; return false; }
        if (!r) return false;
    }
    if (!hd) { mm->B.at[N] = (uint32_t)mm->B.n; mlayer_fit(&mm->B); mm->dust = true; }
    mm->lines_ok = true;
    return true;
}

/* ── the pairs ── */

static uint32_t hilbert(int x, int y) {
    uint32_t dd = 0;
    for (int s = 256; s > 0; s >>= 1) {
        const int rx = (x & s) > 0, ry = (y & s) > 0;
        dd += (uint32_t)(s * s) * (uint32_t)((3 * rx) ^ ry);
        if (!ry) { if (rx) { x = 511 - x; y = 511 - y; } const int t = x; x = y; y = t; }
    }
    return dd;
}
/* a grain's key: its place on a Hilbert curve over the glass, then its index (the low 14 bits) */
static inline uint32_t grain_key(int x, int y, int n) { return hilbert(x < 511 ? x : 511, y < 511 ? y : 511) << 14 | (uint32_t)n; }
/* A picture's grains put in order along the curve (its keys, a scratch as long): a stable radix sort, 6 bits of the place a
 * pass, three passes, each its digits counted and then its keys moved; `budget` us of it now (in model us and in time),
 * a few at least. True: in order. */
static bool sort_step(msort_t *st, uint32_t **key, int n, int budget, int *rw) {
    const int64_t t_in = now_us();
    const int w0 = *rw;
    if (st->pass >= 3) return true;
    if (!st->tmp && !(st->tmp = FLUID_MALLOC(sizeof(uint32_t) * FLUID_MORPH_GRAINS))) return false;
    for (bool first = true; st->pass < 3; first = false) {
        const int sh = 14 + 6 * st->pass;
        uint32_t *cnt = st->cnt, *k = *key, *t = st->tmp;
        if (!st->spos && !st->counted) {
            memset(cnt, 0, sizeof(st->cnt));
            for (int q = 0; q < n; q++) cnt[(k[q] >> sh & 63) + 1]++;
            for (int b = 0; b < 64; b++) cnt[b + 1] += cnt[b];
            st->counted = true;
            *rw += (int)(n * MW_LOOK);
        }
        while (st->spos < n && (first || (now_us() - t_in < budget && *rw - w0 + 512 * MW_SORT < budget))) {
            const int e = st->spos + 512 < n ? st->spos + 512 : n;
            for (int q = st->spos; q < e; q++) t[cnt[k[q] >> sh & 63]++] = k[q];
            *rw += (int)((e - st->spos) * MW_SORT);
            st->spos = e;
            first = false;
        }
        if (st->spos < n) return false;
        *key = t; st->tmp = k; st->spos = 0; st->counted = false; st->pass++;
        if (now_us() - t_in >= budget || *rw - w0 >= budget) break;
    }
    return st->pass >= 3;
}
/* a grain's cell (its layer goes by cell) and its pixel */
static int grain_cell(const fluid_draw_t *d, const mlayer_t *L, int n) {
    int lo = 0, hi = d->W * d->H - 1;
    while (lo < hi) { const int mid = (lo + hi + 1) / 2; if ((int)L->at[mid] <= n) lo = mid; else hi = mid - 1; }
    return lo;
}
static inline void grain_px(const fluid_draw_t *d, const mlayer_t *L, int n, int k, int *x, int *y) {
    *x = d->cx[k % d->W] + (L->g[n] & 15); *y = d->cy[k / d->W] + (L->g[n] >> 4 & 15);
}
static inline float glass_c(const fluid_draw_t *d) { return d->ox + d->stage * 0.5f; }
static inline float glass_cy(const fluid_draw_t *d) { return d->oy + d->stage * 0.5f; }
static inline float glass_k(const fluid_draw_t *d) { return d->stage * (1.f / 466.f); }
/* a point past the rim goes back into the band inside it */
static inline void in_glass(const fluid_draw_t *d, float *x, float *y, float r) {
    const float cx = glass_c(d), cy = glass_cy(d), R = MP_RIM * glass_k(d), dx = *x - cx, dy = *y - cy, l2 = dx * dx + dy * dy;
    if (l2 <= R * R) return;
    const float k = R * (0.72f + 0.28f * fsqrt(r)) / fsqrt(l2);
    *x = cx + dx * k; *y = cy + dy * k;
}
/* a pair's times' reciprocals (no division each frame): its two legs, its whole flight */
static void fly_inv(mfly_t *p) {
    const float a = p->a * 1e-3f, b = p->b * 1e-3f, dd = p->d * 1e-3f;
    p->ib_a = 1.f / (b - a); p->id_b = 1.f / (dd - b); p->id_a = 1.f / (dd - a);
}
/* The pairs that fly (the old picture read and in order): at most FLUID_MORPH_FLY (and FLUID_MORPH_FLY_PX px of grains),
 * spaced evenly along the old picture's curve; their times and their cloud points (the pull to the new spot comes once
 * that is read) */
static void morph_plan(fluid_draw_t *d) {
    fluid_morph_t *m = &d->m;
    fluid_morph_mem_t *mm = m->mm;
    mlayer_t *A = &mm->A;
    /* as many as the smaller picture has, at most FLUID_MORPH_FLY and FLUID_MORPH_FLY_PX px of grains (a pair as big as
     * the bigger of the two pictures' grains on the mean) */
    int K = A->n < FLUID_MORPH_FLY ? A->n : FLUID_MORPH_FLY, za = 0;
    for (int n = 0; n < A->n; n++) za += A->g[n] & MG_Z2 ? 4 : 1;
    const float area = A->n ? (float)za / A->n : 1.f;
    if (K > FLUID_MORPH_FLY_PX / area) K = (int)(FLUID_MORPH_FLY_PX / area);
    if (m->keep) K = 0;                                 /* (its dust alone: nothing leaves) */
    const uint32_t *oa = mm->akey;
    const float C = glass_c(d), CY = glass_cy(d), gk = glass_k(d);
    for (int q = 0; q < K; q++) {
        const int n = (int)(oa[(int)((q + 0.5f) * A->n / K)] & 0x3FFFu), k = grain_cell(d, A, n);
        mfly_t *p = &mm->fly[q];
        int x, y;
        grain_px(d, A, n, k, &x, &y);
        const uint32_t s = (uint32_t)q;
        const float a = MP_LEAVE * mrand(s, 1), b = MP_MID + MP_MID1 * mrand(s, 2), dd = b + MP_LAND + MP_LAND1 * mrand(s, 4);
        const float an = mrand(s, 5) * 6.2831853f, rr = MP_DISC * gk * fsqrt(mrand(s, 6));
        const uint8_t z = (uint8_t)(A->g[n] & MG_Z2 ? 2 : 1);
        *p = (mfly_t){ (int16_t)x, (int16_t)y, -1, -1,
                       (int16_t)((((1.f - MP_SPREAD) * x + MP_SPREAD * (C + msin(an + 1.5707963f) * rr))) * 8.f),
                       (int16_t)((((1.f - MP_SPREAD) * y + MP_SPREAD * (CY + msin(an) * rr))) * 8.f),
                       (uint16_t)(a * 1000.f), (uint16_t)(b * 1000.f), (uint16_t)(dd * 1000.f),
                       A->col[n], MG_NONE, z, z, (uint8_t)(mhash(s, 7) >> 24), (uint8_t)(mhash(s, 8) >> 24),
                       (int16_t)x, (int16_t)y, A->col[n], z, 0, { 0, 0, 0, 0 }, 0.f, 0.f, 0.f };
        fly_inv(p);
        A->ref[n] = (uint16_t)q;
    }
    FLUID_FREE(mm->akey); mm->akey = NULL; FLUID_FREE(mm->sa.tmp); mm->sa.tmp = NULL;
    mm->nf = K; m->nfly = K; mm->chosen = true;
    if (!mm->job) mm->knf = true;                       /* (read in line; the job is told at the handoff) */
    m->work += (int)(K * MW_FLY);
}

/* ── the still layers' lists (mlist_t): made in two passes over their picture (counted by moment, then placed) ── */
static inline uint32_t mpix(int x, int y, bool z2) { return (uint32_t)y << 10 | (uint32_t)x | (z2 ? 1u << 31 : 0u); }
static void mlist_put(mlist_t *L, int pass, int q, uint32_t e, uint16_t c) {
    if (!pass) { L->at[q + 1]++; return; }
    const uint32_t i = L->at[q]++;
    L->e[i] = e;
    if (L->col) L->col[i] = c;
}
/* a count past what the glass can hold (it never should be): kept for the log ("change ... bound="), and refused */
static bool bound(const fluid_draw_t *d, const char *what, uint64_t n, uint64_t most) {
    if (n <= most) return true;
    fluid_morph_t *m = (fluid_morph_t *)&d->m;
    m->bound = what; m->bound_n = n > INT32_MAX ? INT32_MAX : (int)n;
    return false;
}
/* (between the passes: its room, each moment's first; at most `most` entries) */
static bool mlist_room(const fluid_draw_t *d, mlist_t *L, bool col, int most, const char *what) {
    uint64_t sum = 0;
    for (int q = 0; q < MP_NQ; q++) sum += L->at[q + 1];
    if (!bound(d, what, sum, (uint64_t)most)) return false;
    L->n = 0;
    for (int q = 0; q < MP_NQ; q++) { L->n += (int)L->at[q + 1]; L->at[q + 1] += L->at[q]; }
    const size_t n = (size_t)(L->n ? L->n : 1);
    L->e = FLUID_MALLOC((col ? 6 : 4) * n);
    L->col = col && L->e ? (uint16_t *)(void *)(L->e + n) : NULL;
    return L->e != NULL;
}
static void mlist_done(mlist_t *L) {                    /* (after the second: at[q] back to each moment's first) */
    for (int q = MP_NQ; q > 0; q--) L->at[q] = L->at[q - 1];
    L->at[0] = 0; L->q = 0;
}
static inline bool mlist_over(const mlist_t *L) { return !L->e || L->at[L->q] >= (uint32_t)L->n; }
/* the old picture's (the drawer's, once the pairs are chosen): its grains with no pair going out where they are, each at
 * its own moment, and its lines going a cell at a time (its dust alone coming: its lines stay) */
/* (in shares of `budget` us, a cell row at least: 1 made, 0 not yet, -1 no room) */
static int old_lists(fluid_draw_t *d, fluid_morph_mem_t *mm, int budget) {
    const int W = d->W, N = W * d->H;
    const int64_t t_in = now_us();
    for (; mm->olp < 2; mm->olp++, mm->olk = 0) {
        const int pass = mm->olp;
        if (pass && !mm->olk && (!mlist_room(d, &mm->os, false, mm->A.n, "os") || (!mm->keep && !mlist_room(d, &mm->ol, false, mm->L0.n, "ol")))) return -1;
        for (; mm->olk < N; mm->olk++) {
            const int k = mm->olk;
            if (k % W == 0 && k && now_us() - t_in > budget) return 0;
            const int x0 = d->cx[k % W], y0 = d->cy[k / W];
            for (uint32_t n = mm->A.at[k]; n < mm->A.at[k + 1]; n++)
                if (mm->A.ref[n] == MG_NONE)
                    mlist_put(&mm->os, pass, mq(MP_SPARE0 + (MP_SPARE1 - MP_SPARE0) * fsqrt(mrand(n, 21))),
                              mpix(x0 + (mm->A.g[n] & 15), y0 + (mm->A.g[n] >> 4 & 15), mm->A.g[n] & MG_Z2), 0);
            const int q = cell_q(k, MP_OLD_LINES0, MP_OLD_LINES1);
            for (uint32_t r = mm->L0.at[k]; !mm->keep && r < mm->L0.at[k + 1]; r++)
                mlist_put(&mm->ol, pass, q, mpix(x0 + (mm->L0.o[r] & 15), y0 + (mm->L0.o[r] >> 4), false), 0);
        }
    }
    mlist_done(&mm->os); mlist_done(&mm->ol);
    d->m.work += (int)((mm->A.n + mm->L0.n) * 2 * MW_LOOK);
    return 1;
}
/* The reader's lists over the cells (the new grains', the new lines'), a cell row at a time in its share: `budget` us of
 * the wall clock and of the model, at least a row a call, resumable (w); the first pass counts each moment's entries,
 * then the lists' room (a, with colours; b, without; more: between()), the second places them. cell(): cell k's entries
 * for the pass, how many it looked at (-1: its span past the picture's). 1: made, 0: not yet, -1: no room */
typedef int (*mcell_fn)(const fluid_draw_t *d, fluid_morph_mem_t *mm, int pass, int k);
static int lists_walk(const fluid_draw_t *d, fluid_morph_mem_t *mm, mwalk_t *w, mlist_t *a, int amost, mlist_t *b, mcell_fn cell,
                      bool (*between)(fluid_morph_mem_t *mm), int budget) {
    const int W = d->W, N = W * d->H, w0 = mm->rw, at0 = w->pass * N + w->k;
    const int64_t t_in = now_us();
    for (; w->pass < 2; w->pass++, w->k = 0) {
        for (; w->k < N; w->k++) {
            if (w->k % W == 0 && w->pass * N + w->k != at0 &&
                (now_us() - t_in >= budget || mm->rw - w0 >= budget || (s_walk_rows && (w->pass * N + w->k - at0) / W >= s_walk_rows))) return 0;
            const int c = cell(d, mm, w->pass, w->k);
            if (c < 0) return -1;                       /* (a count past the picture's: bound) */
            mm->rw += (int)(c * MW_ITEM);
        }
        if (!w->pass) {   /* (the room once, as the first pass ends in this same call: a call that stops at the second's first
                           * cell never makes it again over counts already turned into places; bench 33's 8.4 MB) */
            const int64_t a0 = now_us();
            const bool ok = mlist_room(d, a, true, amost, a == &mm->nb ? "nb" : "nl") && (!b || mlist_room(d, b, false, W * d->H, "nw")) &&
                            (!between || between(mm));
            mm->jpart[FLUID_MJ_ALLOC] += (int)(now_us() - a0);
            if (!ok) return -1;
        }
    }
    mlist_done(a);
    if (b) mlist_done(b);
    return 1;
}
/* (a cell's new grains with no pair: one in MP_GLIDERS glides in, the others come up on their spot, each at its own moment,
 * or all at the cell's in a look that grows outward: fluid_draw_morph_order) */
static int grains_cell(const fluid_draw_t *d, fluid_morph_mem_t *mm, int pass, int k) {
    const mlayer_t *B = &mm->B;
    const int x0 = d->cx[k % d->W], y0 = d->cy[k / d->W], o = d->m.ord[k];   /* (its order: none glides, they come in turn) */
    if (!bound(d, "B.at", B->at[k + 1], (uint64_t)B->n) || !bound(d, "B.at", B->at[k], B->at[k + 1])) return -1;
    for (uint32_t n = B->at[k]; n < B->at[k + 1]; n++) {
        if (B->ref[n] != MG_NONE) continue;
        if (!mm->keep && o == 255 && !(B->aux[2 * n + 1] % MP_GLIDERS)) { if (pass) mm->bl[mm->wg.g] = n | (uint32_t)k << 14; mm->wg.g++; continue; }
        mlist_put(&mm->nb, pass, mq(MP_BORN0 + (MP_BORN1 - MP_BORN0) * (o < 255 ? o * (1.f / 254.f) : 1.f - fsqrt(1.f - mrand(n, 36)))),
                  mpix(x0 + (B->g[n] & 15), y0 + (B->g[n] >> 4 & 15), B->g[n] & MG_Z2), B->col[n]);
    }
    return (int)(B->at[k + 1] - B->at[k]);
}
/* (between the grains' passes: the gliders met in the first, their state) */
static bool gliders_room(fluid_morph_mem_t *mm) {
    const int g = mm->wg.g;
    mm->bl = FLUID_MALLOC(sizeof(uint32_t) * (size_t)(g ? g : 1));
    mm->gst = mm->bl ? FLUID_CALLOC(sizeof(mglide_t) * (size_t)(g ? g : 1)) : NULL;
    mm->ngl = g; mm->wg.g = 0;
    return mm->gst != NULL;
}
/* The reader's, once the new picture is read and in order (the pairs chosen: nf): each pair its new grain, rank to rank
 * along the curves, spaced evenly (as many as the new picture has, and FLUID_MORPH_LAND_PX px of its grains; the other
 * pairs fade in the cloud); then the rest of the new grains' list (nb, the gliders: lists_walk). Its grains' sizes first,
 * a share at a time too (mm->wz: so far, mm->za: their px). 1: made, 0: not yet, -1: no room */
static int new_grains(const fluid_draw_t *d, fluid_morph_mem_t *mm, int budget) {
    mlayer_t *B = &mm->B;
    const int K0 = mm->nf, w0 = mm->rw;
    const int64_t t_in = now_us();
    while (!mm->tg && mm->wz < B->n) {
        if (mm->wz && (now_us() - t_in >= budget || mm->rw - w0 >= budget)) return 0;
        const int e = mm->wz + 2048 < B->n ? mm->wz + 2048 : B->n;
        for (int n = mm->wz; n < e; n++) mm->za += B->g[n] & MG_Z2 ? 4 : 1;
        mm->rw += (int)((e - mm->wz) * MW_LOOK);
        mm->wz = e;
    }
    if (!mm->tg) {
        const int kpx = B->n ? (int)(FLUID_MORPH_LAND_PX * (float)B->n / (float)mm->za) : K0;
        int K = B->n < K0 ? B->n : K0;
        if (K > kpx) K = kpx;
        const int64_t a0 = now_us();
        mm->tg = FLUID_MALLOC(sizeof(mtgt_t) * (size_t)(K0 ? K0 : 1));
        mm->jpart[FLUID_MJ_ALLOC] += (int)(now_us() - a0);
        if (!mm->tg) return -1;
        for (int q = 0, kept = 0; q < K0; q++) {
            mtgt_t *g = &mm->tg[q];
            if (kept >= K || (int)((kept + 0.5f) * K0 / K) != q) { g->ib = MG_NONE; continue; }   /* (it fades in the cloud) */
            const int n = (int)(mm->bkey[(int)((kept + 0.5f) * B->n / K)] & 0x3FFFu), k = grain_cell(d, B, n);
            int x, y;
            grain_px(d, B, n, k, &x, &y);
            *g = (mtgt_t){ (int16_t)x, (int16_t)y, (uint16_t)n, (uint8_t)(B->g[n] & MG_Z2 ? 2 : 1) };
            B->ref[n] = (uint16_t)q;
            kept++;
        }
        mm->nk = K;
        mm->wg = (mwalk_t){ 0, 0, 0 };
        mm->rw += (int)(K0 * MW_ITEM * 4.f);
    }
    return lists_walk(d, mm, &mm->wg, &mm->nb, B->n, NULL, grains_cell, gliders_room, budget - (int)(now_us() - t_in));
}
/* a cell the reader could not take apart (no room, or its look changed since it was read): drawn whole at its lines'
 * moment (the list nw, its entries the cells) */
static inline bool cell_whole(const fluid_draw_t *d, const fluid_morph_mem_t *mm, int k) { return mm->stale[k] || (lines_now(d, k) && !mm->nlc[k]); }
/* the new lines' list (the reader's, once its lines and its dust are read: a scene's specks on its lines are in them), a
 * cell at a time (its dust alone coming: at the end); the cells drawn whole at their lines' moment in a list of their own */
static int lines_cell(const fluid_draw_t *d, fluid_morph_mem_t *mm, int pass, int k) {
    const int x0 = d->cx[k % d->W], y0 = d->cy[k / d->W], q = mm->keep ? mq(MP_LEN) - 1 : cell_q(k, MP_NEW_LINES0, MP_NEW_LINES1);
    if (!bound(d, "L1.at", mm->L1.at[k + 1], (uint64_t)mm->L1.n) || !bound(d, "L1.at", mm->L1.at[k], mm->L1.at[k + 1])) return -1;
    for (uint32_t r = mm->L1.at[k]; r < mm->L1.at[k + 1]; r++)
        mlist_put(&mm->nl, pass, q, mpix(x0 + (mm->L1.o[r] & 15), y0 + (mm->L1.o[r] >> 4), false), mm->L1.col[r]);
    if (cell_whole(d, mm, k)) mlist_put(&mm->nw, pass, q, (uint32_t)k, 0);
    return (int)(mm->L1.at[k + 1] - mm->L1.at[k]) + 1;
}
static int new_lines(const fluid_draw_t *d, fluid_morph_mem_t *mm, int budget) {
    return lists_walk(d, mm, &mm->wl, &mm->nl, mm->L1.n, &mm->nw, lines_cell, NULL, budget);
}

/* ── each frame's state ── */

static inline float qtime(int q) { return q * (1.f / (4.f * MP_FPS)); }   /* (the clock: quarter frames) */
/* a grain away from its own spot is not drawn past the glass's rim (less 2 px, as the prototype) */
static inline bool off_glass(const fluid_draw_t *d, int x, int y) {
    const float R = d->stage * 0.5f - 2.f, ex = x - glass_c(d), ey = y - glass_cy(d);
    return ex * ex + ey * ey > R * R;
}
/* this frame's shimmer, by a grain's phase: 0.7 sin(5.1 ts + ph), 0.7 cos(4.3 ts + 1.7 ph) */
static void shimmer(fluid_morph_mem_t *mm, float ts) {
    for (int h = 0; h < 256; h++) {
        const float ph = h * (6.2831853f / 256.f);
        mm->shx[h] = 0.7f * msin(5.1f * ts + ph); mm->shy[h] = 0.7f * msin(4.3f * ts + 1.7f * ph + 1.5707963f);
    }
}
/* a leg's ease (a cubic of its speeds: s0 leaving, s1 arriving, as shares of the leg's mean) */
static inline float leg(float u, float s0, float s1) { return u * (s0 + u * ((3.f - 2.f * s0 - s1) + u * (s0 + s1 - 2.f))); }
/* a pair at time t (s) and the flow's time ts: where it is, its colour and size (z 0: not shown). One path: out of its old
 * spot into its cloud point (fast, easing to MP_TURN), on from there into its new spot (from MP_TURN, landing softly) */
static void fly_at(const fluid_draw_t *d, const mfly_t *p, float t, float ts, float pull, int *ox, int *oy, uint16_t *oc, uint8_t *oz) {
    const fluid_morph_mem_t *mm = d->m.mm;
    const float a = p->a * 1e-3f, b = p->b * 1e-3f, dd = p->d * 1e-3f;
    const bool gone = p->gone & 1, known = p->x1 >= 0 && !gone;
    float cx = p->qx * 0.125f, cy = p->qy * 0.125f;
    if (known) { cx += (1.f - MP_SPREAD) * MP_TOWARD * (p->x1 - p->x0) * pull; cy += (1.f - MP_SPREAD) * MP_TOWARD * (p->y1 - p->y0) * pull; }
    in_glass(d, &cx, &cy, p->r * (1.f / 255.f));
    float x, y;
    if (t <= a) { x = p->x0; y = p->y0; }
    else if (t < b) { const float e = leg((t - a) * p->ib_a, MP_START, MP_TURN); x = p->x0 + (cx - p->x0) * e; y = p->y0 + (cy - p->y0) * e; }
    else if (!known) { x = cx; y = cy; }
    else if (t < dd) { const float e = leg((t - b) * p->id_b, MP_TURN, 0.f); x = cx + (p->x1 - cx) * e; y = cy + (p->y1 - cy) * e; }
    else { x = p->x1; y = p->y1; }
    if (t > a && (t < dd || gone)) {                    /* the turbulence: a shared smooth flow and its own shimmer */
        const float env = gone ? 1.f : msin(3.1415927f * (t - a) * p->id_a), am = MP_FLOW * glass_k(d) * env;
        const float fx = msin(0.021f * y + 1.7f * ts) + 0.6f * msin(0.037f * (x + y) + 2.3f * ts + 1.3f) + mm->shx[p->ph];
        const float fy = msin(0.019f * x - 1.5f * ts + 2.1f) + 0.6f * msin(0.031f * (x - y) - 2.1f * ts + 0.4f) + mm->shy[p->ph];
        x += fx * am; y += fy * am;
    }
    /* its colour turns from its leg's middle to the next's (from when its new grain is known, if later) */
    float mx = 0.f;
    if (known) { const float ms = mm->tk > (a + b) * 0.5f ? mm->tk : (a + b) * 0.5f, me = (b + dd) * 0.5f; mx = mclamp((t - ms) / (me - ms > 0.05f ? me - ms : 0.05f)); }
    uint16_t col = known ? mmix(p->c0, mm->B.col[p->ib], mx) : p->c0;
    uint8_t z = mx < 0.5f ? p->z0 : p->z1;
    if (gone) {                                         /* (no new grain: it fades in the cloud, at its own moment) */
        const float f0 = mm->tk > b ? mm->tk : b;
        if ((t - f0) * (256.f / MP_GONE) + p->ph >= 256.f) z = 0;
    }
    const int ix = ifloor(x + 0.5f), iy = ifloor(y + 0.5f);
    if (!col || (!(ix == p->x0 && iy == p->y0) && !(ix == p->x1 && iy == p->y1) && off_glass(d, ix, iy))) z = 0;
    *ox = ix; *oy = iy; *oc = col; *oz = z;
}
/* a new grain that glides in (glider g, grain n at x1, y1): when it starts; where it comes up, MP_NEAR .. 2 MP_NEAR px from
 * its spot (1/4 px; worked out once, when first due); at t where it is (px from its spot) and whether it shows */
static inline float glide_t(const fluid_morph_mem_t *mm, int n) { return MP_GLIDE0 + (MP_GLIDE1 - MP_GLIDE0) * mm->B.aux[2 * n] * (1.f / 255.f); }
static void glide_start(const fluid_draw_t *d, mglide_t *g, int n, int x1, int y1) {
    const float dd = MP_NEAR * glass_k(d) * (1.f + mrand((uint32_t)n, 33)), an = mrand((uint32_t)n, 34) * 6.2831853f;
    float x = x1 + msin(an + 1.5707963f) * dd, y = y1 + msin(an) * dd;
    in_glass(d, &x, &y, mrand((uint32_t)n, 35));
    const float ex = (x - x1) * 4.f, ey = (y - y1) * 4.f;
    g->ox = (int8_t)(ex < -127.f ? -127 : ex > 127.f ? 127 : (int)(ex + (ex < 0.f ? -0.5f : 0.5f)));
    g->oy = (int8_t)(ey < -127.f ? -127 : ey > 127.f ? 127 : (int)(ey + (ey < 0.f ? -0.5f : 0.5f)));
}
static int glide_at(const fluid_draw_t *d, const mglide_t *g, int n, int x1, int y1, float t, int *dx, int *dy) {
    const fluid_morph_mem_t *mm = d->m.mm;
    const float a = glide_t(mm, n);
    *dx = *dy = 0;
    if ((t - a) * (256.f / MP_BORN_FADE) + mm->B.aux[2 * n + 1] < 256.f) return 0;   /* (it comes up at its own moment) */
    if (t >= a + MP_BORN_FADE + MP_BORN_GLIDE) return 1;
    float x = g->ox * 0.25f, y = g->oy * 0.25f;
    if (t > a + MP_BORN_FADE) {
        const float u = (t - a - MP_BORN_FADE) * (1.f / MP_BORN_GLIDE), e = u * u * u * (u * (u * 6.f - 15.f) + 10.f);
        x -= x * e; y -= y * e;
    }
    *dx = ifloor(x1 + x + 0.5f) - x1; *dy = ifloor(y1 + y + 0.5f) - y1;
    return (*dx || *dy) && off_glass(d, x1 + *dx, y1 + *dy) ? 0 : 1;
}

/* ── composing: straight into the kept glass ── */

/* a pixel of the kept glass on its cells (else NULL), its row marked to send */
static inline uint16_t *gpx(const fluid_draw_t *d, int X, int Y) {
    const fluid_morph_t *m = &d->m;
    if (X < 0 || Y < 0 || X >= d->fb_w || Y >= d->fb_h || m->colx[X] == 255 || m->rowy[Y] == 255) return NULL;
    m->mm->rows[Y] = 1; m->mm->wpx++;
    return &m->snap[(size_t)Y * d->fb_w + X];
}
/* a still grain's or line pixel's pixels written (a 2 x 2 px grain: 4); a new grain never over a lit pixel: the old
 * picture is gone by then (its lists go first) and the movers are off the glass, so that is a new line come early (a
 * speck on it is in it) or a cell drawn whole */
static void gput(const fluid_draw_t *d, uint32_t e, uint16_t c, bool grain) {
    const int x = (int)(e & 1023u), y = (int)(e >> 10 & 1023u), z = e >> 31 ? 2 : 1;
    for (int q = 0; q < z * z; q++) {
        uint16_t *p = gpx(d, x + (q & 1), y + (q >> 1));
        if (p && !(grain && *p)) *p = c;
    }
}
/* A mover drawn, what it covers kept (un), and taken off again: each frame every mover still moving is taken off (the
 * last drawn first), the still layers turn, and they are drawn again (so what a mover passes over is there after it) */
static void mdraw(const fluid_draw_t *d, int x, int y, int z, uint16_t c, uint16_t *un) {
    for (int q = 0; q < z * z; q++) { uint16_t *p = gpx(d, x + (q & 1), y + (q >> 1)); if (p) { un[q] = *p; *p = c; } }
}
static void moff(const fluid_draw_t *d, int x, int y, int z, const uint16_t *un) {
    for (int q = z * z - 1; q >= 0; q--) { uint16_t *p = gpx(d, x + (q & 1), y + (q >> 1)); if (p) *p = un[q]; }
}
/* (a mover come to rest on its spot: what it covered stays under it; a lit pixel there: its cell written again at the end) */
static void mrest(const fluid_draw_t *d, int x, int y, int z, const uint16_t *un) {
    for (int q = 0; q < z * z; q++) {
        const int X = x + (q & 1), Y = y + (q >> 1);
        if (un[q] && X >= 0 && Y >= 0 && X < d->fb_w && Y < d->fb_h && d->m.colx[X] != 255 && d->m.rowy[Y] != 255) d->m.mm->dmg[d->m.rowy[Y] * d->W + d->m.colx[X]] = 1;
    }
}

/* The window t as the change keeps it (tests): a copy of the kept glass. Returns its pixels. */
int fluid_draw_morph_rect(const fluid_draw_t *d, const fluid_rect_t *t) {
    if (!d->m.on || !d->m.mm || !d->m.snap) return 0;
    for (int y = t->y0; y < t->y1; y++) memcpy(rect_px(t, t->x0, y), d->m.snap + (size_t)y * d->fb_w + t->x0, sizeof(uint16_t) * (size_t)(t->x1 - t->x0));
    return (t->x1 - t->x0) * (t->y1 - t->y0);
}
/* a list's entries turned so far */
static inline int mlist_turned(const mlist_t *L) { return L->e ? (int)L->at[L->q] : 0; }

void fluid_draw_morph_census(const fluid_draw_t *d, fluid_morph_census_t *c) {
    memset(c, 0, sizeof(*c));
    const fluid_morph_t *m = &d->m;
    const fluid_morph_mem_t *mm = m->mm;
    if (!m->on || !mm || mm->oj < d->H || !mm->planned) return;
    const float t = m->t;
    for (int q = 0; q < mm->nf; q++) {
        const mfly_t *p = &mm->fly[q];
        if (p->gone & 1) { c->fly += p->z != 0; continue; }
        if (t <= p->a * 1e-3f) c->wait++; else if (p->gone & 4) c->landed++; else c->fly++;
    }
    c->spare = mm->os.e ? mm->os.n - mlist_turned(&mm->os) : mm->A.n - mm->nf;
    c->old_lines = mm->keep ? 0 : mm->ol.e ? mm->ol.n - mlist_turned(&mm->ol) : mm->L0.n;   /* (its dust alone: they stay) */
    c->born = c->born_landed = mlist_turned(&mm->nb);
    const mglide_t *gs = mm->gst;
    for (int g = 0; (mm->got & MGOT_DUST) && g < mm->ngl; g++) { c->born += (gs[g].lv & 1) != 0; c->born_landed += (gs[g].lv & 1) && (gs[g].lv & 0x20); }
    c->new_lines = mlist_turned(&mm->nl);
    if ((mm->got & (MGOT_DUST | MGOT_LINES)) == (MGOT_DUST | MGOT_LINES)) {
        uint32_t h = 2166136261u;
        for (int n = 0; n < mm->B.n; n++) h = mhash(h ^ mm->B.g[n], (uint32_t)mm->B.col[n] | (uint32_t)(mm->gl ? mm->gl[n] : 0) << 16 | (uint32_t)mm->B.aux[2 * n] << 24);
        for (int q = 0; q < mm->L1.n; q++) h = mhash(h ^ mm->L1.o[q], mm->L1.col[q]);
        for (int k = 0; k <= d->W * d->H; k++) h = mhash(h ^ mm->B.at[k], mm->L1.at[k] ^ (k < d->W * d->H ? (uint32_t)mm->stale[k] << 24 : 0u));
        c->read = h | 1u;
    }
}

void fluid_draw_morph_costs(float k) { s_mwk = k > 0.f ? k : 1.f; }
void fluid_draw_morph_bounds(int cloud_n, int lines_n, int read_cells) {
    s_hold_n = cloud_n > 0 ? cloud_n : FLUID_MORPH_HOLD_N; s_lines_n = lines_n > 0 ? lines_n : FLUID_MORPH_LINES_N;
    s_read_cells = read_cells;
}
/* The reader's share (the drawer in line, or the job): the new picture read, `budget` us of it, then its dust put in
 * order, `sort` us of its own; once in order, the pairs' new grains and the new grains' list; once its lines are read
 * too, theirs. False: out of room. */
/* (each list in one go, when the call has room for it: first, before the call's reading takes its room, and after) */
static bool morph_lists(const fluid_draw_t *d, fluid_morph_mem_t *mm, int room) {
    if (mm->dust && mm->sb.pass >= 3 && mm->knf && !mm->bready && room - mm->rw > 0) {
        const int64_t t0 = now_us();
        const int r = new_grains(d, mm, room - mm->rw);
        mm->jpart[FLUID_MJ_GRAINS] += (int)(now_us() - t0);
        if (r < 0) return false;
        mm->bready = r > 0;
    }
    if (mm->bready && mm->lines_ok && !mm->lready && room - mm->rw > 0) {
        const int64_t t0 = now_us();
        const int r = new_lines(d, mm, room - mm->rw);
        mm->jpart[FLUID_MJ_LINES] += (int)(now_us() - t0);
        if (r < 0) return false;
        mm->lready = r > 0;
    }
    return true;
}
static bool morph_read(fluid_draw_t *d, fluid_morph_mem_t *mm, const fluid_frame_t *s, const fluid_draw_opts_t *o, int budget, int sort, bool ready, bool fine) {
    const int room = budget > sort ? budget : sort, w0 = mm->rw;
    const int64_t t_in = now_us();
    if (!morph_lists(d, mm, room)) return false;
    {   /* (what the lists took is not the reading's again) */
        const int took = (int)(now_us() - t_in) > mm->rw - w0 ? (int)(now_us() - t_in) : mm->rw - w0;
        if (budget > 0) budget = budget - took > 1 ? budget - took : 1;
        if (sort > 0) sort = sort - took > 1 ? sort - took : 1;
    }
    /* (inputs of another kind than its picture is read as, a frame of a scene with no shapes: read on with the next ones,
     * never mixed) */
    if (budget > 0 && (!mm->dust || !mm->lines_ok) && (o->scene && o->dots && o->shapes) == mm->hd) {
        const bool dust = mm->dust;
        const int64_t r0 = now_us();
        if (!morph_read_new(d, mm, s, o, budget, ready, fine) && (!mm->bset || mm->jfail)) return false;
        *(dust ? &mm->jlus : &mm->jdus) += (int)(now_us() - r0);
        mm->jpart[FLUID_MJ_READ] += (int)(now_us() - r0);
    }
    if (sort > 0 && mm->dust) {
        const int64_t t0 = now_us();
        const bool done = sort_step(&mm->sb, &mm->bkey, mm->B.n, sort, &mm->rw);
        mm->jpart[FLUID_MJ_SORT] += (int)(now_us() - t0);
        if (!done && !mm->sb.tmp) return false;
    }
    {   /* (one budget for the call: the lists after the reading only in what it left) */
        const int took = (int)(now_us() - t_in) > mm->rw - w0 ? (int)(now_us() - t_in) : mm->rw - w0;
        return room - took <= 0 || morph_lists(d, mm, room - took + (mm->rw - w0));
    }
}
/* What the reader has read, for the drawer: in line right after its own read, with the job at the handoff (with the
 * drawer idle and the job between slices) */
static void morph_got(fluid_draw_t *d) {
    fluid_morph_t *m = &d->m;
    fluid_morph_mem_t *mm = m->mm;
    if (!mm) return;
    if (!(mm->got & MGOT_DUST) && mm->bready) { mm->got |= MGOT_DUST; m->nb = mm->B.n; m->dg[FLUID_MD_DUST] = m->frames; }
    if (!(mm->got & MGOT_LINES) && mm->lready) { mm->got |= MGOT_LINES; m->dg[FLUID_MD_LINES] = m->frames; }
    mm->got |= (mm->jfail ? MGOT_FAIL | MGOT_DONE : 0) | (mm->jdone ? MGOT_DONE : 0);
    m->dg[FLUID_MD_LCELLS] = mm->bj * d->W + mm->bi;
    m->dg[FLUID_MD_DUS] = mm->jdus; m->dg[FLUID_MD_LUS] = mm->jlus;
    m->dg[FLUID_MD_JOBN] = mm->jn; m->dg[FLUID_MD_JOBUS] = mm->jus; m->dg[FLUID_MD_JOBMAX] = mm->jmax;
}
/* the new dust read and its pairs chosen: each pair takes its new grain (the drawer's: its frames read the pairs) */
static void morph_paired(fluid_draw_t *d) {
    fluid_morph_t *m = &d->m;
    fluid_morph_mem_t *mm = m->mm;
    for (int q = 0; q < mm->nf; q++) {
        mfly_t *p = &mm->fly[q];
        const mtgt_t *g = &mm->tg[q];
        if (g->ib == MG_NONE) { p->gone |= 1; p->x1 = (int16_t)(p->qx / 8); p->y1 = (int16_t)(p->qy / 8); p->z1 = p->z0; }
        else { p->x1 = g->x1; p->y1 = g->y1; p->ib = g->ib; p->z1 = g->z1; }
    }
    m->nfly = mm->nk;
    mm->paired = true;
    mm->tk = mm->started && !m->keep ? m->t : -1.f;     /* (paired before the clock moved: its pull from the start) */
    m->dg[FLUID_MD_PAIRED] = m->frames;
    m->work += (int)(mm->nf * MW_ITEM * 2.f);
    FLUID_FREE(mm->bkey); mm->bkey = NULL; FLUID_FREE(mm->sb.tmp); mm->sb.tmp = NULL;   /* (the reader is done with them: MGOT_DUST) */
}
/* A wait's limit reached (the drawer reads in line): what the change waits for is made now, in this frame, and the change
 * goes on as approved (never a cut, never a repaint: the new picture still comes everywhere at once). A scene's specks not
 * in by now are given up (they come after the change, as a change of its own); the rest is read, put in order and
 * listed. False: no room. */
static bool morph_settle(fluid_draw_t *d, const fluid_frame_t *s, const fluid_draw_opts_t *o) {
    fluid_morph_t *m = &d->m;
    fluid_morph_mem_t *mm = m->mm;
    if (!m->ready) { m->ready = m->forced = true; m->ready_at = m->frames; }
    bool ok = true;
    for (int q = 0; q < 8 && ok && (mm->got & (MGOT_DUST | MGOT_LINES)) != (MGOT_DUST | MGOT_LINES); q++) {
        ok = morph_read(d, mm, s, o, INT32_MAX / 2, INT32_MAX / 2, true, true);
        morph_got(d);
    }
    m->work += mm->rw; mm->rw = 0;
    if (ok && (mm->got & MGOT_DUST) && !mm->paired) morph_paired(d);
    return ok && (mm->got & (MGOT_DUST | MGOT_LINES)) == (MGOT_DUST | MGOT_LINES);
}
/* a glider (g of the list) at t: taken off (from where it was drawn), worked out, drawn again where it is; at rest on its
 * spot, it stays. 1: it moved */
static void glide_off(const fluid_draw_t *d, uint32_t g) {
    const fluid_morph_mem_t *mm = d->m.mm;
    mglide_t *gl = mm->gst + g;
    if (!(gl->lv & 1) || (gl->lv & 0x20)) return;       /* (not shown, or at rest) */
    const uint32_t n = mm->bl[g] & 0x3FFFu, k = mm->bl[g] >> 14;
    moff(d, d->cx[k % d->W] + (mm->B.g[n] & 15) + gl->px, d->cy[k / d->W] + (mm->B.g[n] >> 4 & 15) + gl->py, mm->B.g[n] & MG_Z2 ? 2 : 1, gl->un);
}
static int glide_on(const fluid_draw_t *d, uint32_t g, float t) {
    const fluid_morph_mem_t *mm = d->m.mm;
    mglide_t *gl = mm->gst + g;
    if (gl->lv & 0x20) return 0;
    const uint32_t n = mm->bl[g] & 0x3FFFu, k = mm->bl[g] >> 14;
    if (t < glide_t(mm, (int)n)) return 0;
    const int x1 = d->cx[k % d->W] + (mm->B.g[n] & 15), y1 = d->cy[k / d->W] + (mm->B.g[n] >> 4 & 15), z = mm->B.g[n] & MG_Z2 ? 2 : 1;
    if (!(gl->lv & 0x40)) { glide_start(d, gl, (int)n, x1, y1); gl->lv |= 0x40; }
    int dx, dy;
    const int lv = glide_at(d, gl, (int)n, x1, y1, t, &dx, &dy);
    const int moved = (lv != (gl->lv & 1)) || dx != gl->px || dy != gl->py;
    gl->lv = (uint8_t)((gl->lv & 0x40) | lv); gl->px = (int8_t)dx; gl->py = (int8_t)dy;
    if (!lv) return moved;
    mdraw(d, x1 + dx, y1 + dy, z, mm->B.col[n], gl->un);
    if (t >= glide_t(mm, (int)n) + MP_BORN_FADE + MP_BORN_GLIDE) { gl->lv |= 0x20; mrest(d, x1, y1, z, gl->un); }
    return moved;
}
/* a cell drawn whole, as the look has it (it marks its rows); its px */
static int cell_draw(const fluid_draw_t *d, const fluid_draw_opts_t *o, int k) {
    const int W = d->W, x0 = d->cx[k % W], y0 = d->cy[k / W], x1 = d->cx[k % W + 1], y1 = d->cy[k / W + 1];
    const fluid_rect_t c = { d->m.snap + (size_t)y0 * d->fb_w + x0, d->fb_w, x0, y0, x1, y1 };
    draw_rect_layer(d, o, &c, false);
    for (int y = y0; y < y1; y++) d->m.mm->rows[y] = 1;
    return (x1 - x0) * (y1 - y0);
}
/* a cell drawn again from its picture as read (its still pixels: where a mover came to rest over a lit one, or a grain
 * turned over a lit one): cleared, then the new grains, the lines over them (its dust alone: the kept lines first) */
static int cell_again(const fluid_draw_t *d, int k) {
    const fluid_morph_mem_t *mm = d->m.mm;
    const int W = d->W, x0 = d->cx[k % W], y0 = d->cy[k / W], x1 = d->cx[k % W + 1], y1 = d->cy[k / W + 1];
    for (int y = y0; y < y1; y++) { memset(d->m.snap + (size_t)y * d->fb_w + x0, 0, sizeof(uint16_t) * (size_t)(x1 - x0)); mm->rows[y] = 1; }
    for (uint32_t r = mm->L0.at[k]; mm->keep && r < mm->L0.at[k + 1]; r++) d->m.snap[(size_t)(y0 + (mm->L0.o[r] >> 4)) * d->fb_w + x0 + (mm->L0.o[r] & 15)] = mm->L0.col[r];
    for (uint32_t n = mm->B.at[k]; n < mm->B.at[k + 1]; n++) {
        const int z = mm->B.g[n] & MG_Z2 ? 2 : 1, x = x0 + (mm->B.g[n] & 15), y = y0 + (mm->B.g[n] >> 4 & 15);
        for (int q = 0; q < z * z; q++) d->m.snap[(size_t)(y + (q >> 1)) * d->fb_w + x + (q & 1)] = mm->B.col[n];
    }
    for (uint32_t r = mm->L1.at[k]; r < mm->L1.at[k + 1]; r++) d->m.snap[(size_t)(y0 + (mm->L1.o[r] >> 4)) * d->fb_w + x0 + (mm->L1.o[r] & 15)] = mm->L1.col[r];
    return (x1 - x0) * (y1 - y0);
}
/* where the clock may go this frame while it waits for w (not in yet) at quarter frame qw: slower over the last
 * MP_NEAR_S before it, never past it */
static inline int wait_rate(int n, int qw, int rate) {
    const int left = qw - n, near = (int)(MP_NEAR_S * 4.f * MP_FPS), cap = left <= near / 3 ? 1 : left <= near ? 2 : rate;
    if (left <= 0) return 0;
    rate = rate < cap ? rate : cap;
    return rate < left ? rate : left;
}

/* what the change's own work has left of this frame (in time and in the panel model's work) */
static int morph_left(const fluid_draw_t *d) {
    const fluid_morph_t *m = &d->m;
    const int wall = m->budget - (int)(now_us() - m->mm->f0), work = m->budget - m->work - (m->pass ? FLUID_MORPH_PASS_US : 0);
    return wall < work ? wall : work;
}
/* Read in line (no job), from the frame after the drawer's pass on: in what the frame has left, the new picture read (its
 * lines at once, a scene's specks when in), then put in order and listed in a share of its own; while the clock stands a
 * share at least, so it never waits long. False: the change ended (no room) */
static bool morph_inline(fluid_draw_t *d, const fluid_frame_t *s, const fluid_draw_opts_t *o) {
    fluid_morph_t *m = &d->m;
    fluid_morph_mem_t *mm = m->mm;
    if (mm->job || !mm->passed || m->pass) return true;
    const bool stand = mm->waits && mm->rate <= 1;
    const int left = s_read_cells < 0 ? INT32_MAX / 4 : stand && morph_left(d) < FLUID_MORPH_READ_MIN_US ? FLUID_MORPH_READ_MIN_US : morph_left(d);
    if (!mm->dust || !mm->lines_ok) {
        m->dg[FLUID_MD_LEFT] += left > 0 ? left : 0;
        if (!morph_read(d, mm, s, o, left, 0, m->ready, left >= FLUID_MORPH_FINE_US || mm->waits)) { morph_end(d, true); return false; }
        m->work += mm->rw; mm->rw = 0;
    }
    if (mm->dust && (mm->sb.pass < 3 || !mm->lready) && (morph_left(d) > 0 || mm->waits || s_read_cells < 0)) {
        /* (its own share of the frame: a sort that got only what the grains leave crawled at 512 keys a frame) */
        const int slice = s_read_cells < 0 ? INT32_MAX / 4 : stand && morph_left(d) < FLUID_MORPH_SORT_MIN_US ? FLUID_MORPH_SORT_MIN_US : morph_left(d);
        m->dg[FLUID_MD_SORTF] += mm->sb.pass < 3;
        if (!morph_read(d, mm, s, o, 0, slice, m->ready, false)) { morph_end(d, true); return false; }
        m->work += mm->rw; mm->rw = 0;
    }
    morph_got(d);
    return true;
}

/* This frame of the change (the drawer stands: fluid_draw_cells runs once, for its new look). Before it moves: the old
 * picture in order, the pairs chosen, its lists. Then: the new picture read and paired (each a share of the frame), the
 * clock, and what changes, straight into the kept glass: every mover still moving taken off, the still layers' moments
 * turned (from their lists: what turns now, everywhere at once), the movers drawn where they are now; its last frame
 * draws the cells that need it whole. The rows written go to the panel (d->span). */
static void morph_frame(fluid_draw_t *d, const fluid_frame_t *s, const fluid_draw_opts_t *o) {
    fluid_morph_t *m = &d->m;
    fluid_morph_mem_t *mm = m->mm;
    const int W = d->W, H = d->H, N = W * H;
    int *const pf = m->prof;
    int64_t tp = now_us(), tq;
#define MP_LAP(part) (tq = now_us(), pf[part] += (int)(tq - tp), tp = tq)
#define MP_LEFT() morph_left(d)
    memset(d->span, 0, sizeof(int16_t) * 2 * (size_t)H);
    memset(d->dirty, 0, (size_t)N);
    if (!mm || mm->oj < H) return;                      /* (the old picture being read: nothing moves) */
    memset(mm->rows, 0, (size_t)d->fb_h);
    mm->wpx = 0;
    /* the old picture in order, then (the drawer's one pass for its new look made) the pairs chosen and its lists */
    if (!mm->planned) {
        const int room = m->budget - (m->prof[FLUID_MP_OLD] > m->work ? m->prof[FLUID_MP_OLD] : m->work) - (m->pass ? FLUID_MORPH_PASS_US : 0);
        if (!mm->chosen && room < (int)(mm->A.n * MW_LOOK + 512 * MW_SORT)) { MP_LAP(FLUID_MP_PAIR); return; }   /* (its first step's room: the next frame) */
        const bool sorted = mm->chosen || sort_step(&mm->sa, &mm->akey, mm->A.n, room, &m->work);
        if (!sorted) { if (!mm->sa.tmp) morph_end(d, true); MP_LAP(FLUID_MP_PAIR); return; }
        /* the pairs chosen, then the lists made, each in what the frame leaves (else the next), before the drawer's pass:
         * that frame has its cells */
        if (!mm->chosen && MP_LEFT() > (int)((mm->A.n < FLUID_MORPH_FLY ? mm->A.n : FLUID_MORPH_FLY) * MW_FLY)) morph_plan(d);
        if (mm->chosen && MP_LEFT() > 1000) {
            const int r = old_lists(d, mm, MP_LEFT() - 500);
            if (r < 0) { morph_end(d, true); return; }
            mm->planned = r > 0;
        }
        MP_LAP(FLUID_MP_PAIR);
        morph_inline(d, s, o);                          /* (its new picture read meanwhile, as the job would) */
        return;
    }
    if (!mm->clock) {                                   /* (its clock from the drawer's pass for the new look) */
        if (!mm->passed) return;
        mm->clock = true;
        m->dg[FLUID_MD_PASS] = m->frames;
        m->n = m->keep ? mq(MP_BORN0) : 0;              /* (its dust alone: from just before its grains come up) */
        m->t = qtime(m->n);
        mm->slow = !m->ready && !m->keep;               /* (its dust built late: the dissolve a little slower) */
        mm->rate = mm->slow ? MP_SLOW : MP_RATE;
        mm->started = true;                             /* (it moves this frame: the frames before sent nothing) */
    }
    if (mm->got & MGOT_FAIL) { morph_end(d, true); return; }   /* (the job had no room to read it) */
    if ((mm->got & MGOT_DUST) && !mm->paired) morph_paired(d);   /* (this frame's movers fly to their new grains) */
    MP_LAP(FLUID_MP_PAIR);
    /* the clock: it slows before what it needs and is not in yet (the pairs' cloud points need the new grains, the new
     * lines their list), never past it; it stands there briefly at most: FLUID_MORPH_DUST_* for the new look's inputs
     * (then it is read as it is: its dust comes after the change), FLUID_MORPH_HOLD_* (the cloud) or FLUID_MORPH_LINES_*
     * in all, then what is missing is made at once */
    const int base = mm->paired || !mm->slow ? MP_RATE : MP_SLOW, qend = mq(m->dur);
    int rate = base;
    const bool wc = !mm->paired && !m->keep, wl = !(mm->got & MGOT_LINES);
    if (wc) rate = wait_rate(m->n, mq(MP_MID), rate);
    if (wl) { const int r = wait_rate(m->n, mq(MP_NEW_LINES0), base); rate = r < rate ? r : rate; }
    rate = rate < mm->rate - 1 ? (mm->rate - 1 < 0 ? 0 : mm->rate - 1) : rate > mm->rate + 1 ? mm->rate + 1 : rate;   /* (its pace changes a step a frame) */
    if (wc && m->n + rate > mq(MP_MID)) rate = mq(MP_MID) - m->n;
    if (wl && m->n + rate > mq(MP_NEW_LINES0)) rate = mq(MP_NEW_LINES0) - m->n;
    if (rate < 0) rate = 0;
    mm->waits = mm->started && rate < base && (wc || wl);
    if (!mm->waits || rate > 1) { m->held = 0; m->nheld = 0; }   /* (each stand its own limit) */
    if (mm->waits) {
        m->dg[wc ? FLUID_MD_WAITC : FLUID_MD_WAITL]++;
        if (rate <= 1) {
            const int64_t w = m->held ? tp - m->held : 0;
            if (!m->held) m->held = tp;
            m->nheld++;
            if (!m->ready && (w >= FLUID_MORPH_DUST_US || m->nheld >= FLUID_MORPH_DUST_N)) { m->ready = m->forced = true; m->ready_at = m->frames; }
            if (w >= (wc ? FLUID_MORPH_HOLD_US : FLUID_MORPH_LINES_US) || m->nheld >= (wc ? s_hold_n : s_lines_n)) {
                if (mm->job) {                          /* (read on the job: the rest at once there, here from the next handoff) */
                    if (!m->ready) { m->ready = m->forced = true; m->ready_at = m->frames; }
                    if (!m->jhurry) m->cuts++;
                    m->jhurry = true;
                } else {
                    m->cuts++;
                    if (!morph_settle(d, s, o)) { morph_end(d, true); return; }
                    rate = base;
                }
            }
        }
    }
    if (mm->started) {
        m->w++;
        m->n = m->n + rate < qend ? m->n + rate : qend;  /* (at its end it stands till every list and mover is there too) */
        mm->rate = rate;
        if (rate < MP_RATE) m->dg[FLUID_MD_SLOW]++;
    }
    mm->started = true;
    m->t = qtime(m->n);
    const float t = m->t, ts = m->w * (1.f / MP_FPS), pull = mm->tk < 0.f ? 1.f : msstep(mm->tk, mm->tk + MP_KNOWN, t);
    int moved = 0, came = 0, stepped = 0, w0 = mm->wpx;
    mglide_t *const gs = mm->gst;
    const int ngl = (mm->got & MGOT_DUST) ? mm->ngl : 0;
    /* every mover still moving taken off the glass, the last drawn first (what each covered back under it) */
    for (int g = ngl - 1; g >= 0; g--) glide_off(d, (uint32_t)g);
    for (int q = mm->nf - 1; q >= 0; q--) {
        const mfly_t *p = &mm->fly[q];
        if ((p->gone & 6) == 2 && p->z) moff(d, p->x, p->y, p->z, p->un);
    }
    /* (a pair leaving: its old grain goes as the still ones do, under whatever mover covers it) */
    for (int q = 0; q < mm->nf; q++) {
        mfly_t *p = &mm->fly[q];
        if (p->gone & 2 || t <= p->a * 1e-3f) continue;
        gput(d, mpix(p->x0, p->y0, p->z0 == 2), 0, false);
        p->gone |= 2; p->z = 0;
    }
    m->work += (int)((mm->wpx - w0) * MW_PX + mm->nf * MW_LOOK);
    MP_LAP(FLUID_MP_FLY);
    /* the still layers: each list's moments up to now turned (the old ones first: the new never pass them), a moment at
     * a time while the frame has room (each moment at what the last frames' cost against the model's), one at least when
     * one lags and the frame is not over: late everywhere at once, never by rows. The movers drawn after keep their room
     * (last frame's and its growth since the one before: as the new grains start gliding) */
    mlist_t *const L[5] = { &mm->os, &mm->ol, &mm->nb, &mm->nl, &mm->nw };
    {
        const int qn = m->n, room = mm->cost_draw + (mm->cost_draw > mm->cost_was ? mm->cost_draw - mm->cost_was : 0);
        const float lk = mm->lk > 1.f ? mm->lk : 1.f;
        const int64_t l0 = now_us();
        const int lw0 = m->work;
        static const uint8_t lap[5] = { FLUID_MP_SPARE, FLUID_MP_SPARE, FLUID_MP_LINES, FLUID_MP_LINES, FLUID_MP_END };
        int qold = MP_NQ;                               /* (the old ones' next moment) */
        for (int l = 0; l < 5; l++) {
            mlist_t *const li = L[l];
            if (!li->e || (l >= 2 && !(mm->got & (l == 2 ? MGOT_DUST : MGOT_LINES)))) continue;
            const int qto = l >= 2 && qold < qn ? qold - 1 : qn;
            int done = 0, forced = 0;
            while (li->q <= qto && li->q < MP_NQ && li->at[li->q] < (uint32_t)li->n) {
                const int cnt = (int)(li->at[li->q + 1] - li->at[li->q]);
                const int left = MP_LEFT() - room;
                const bool fits = left >= (int)(cnt * lk * (l == 4 ? 128.f * MW_SHAPE : MW_ITEM + 2.f * MW_PX));
                if (!fits && (forced || li->q > qn - 3 * MP_RATE || left <= 0)) break;   /* (no room: it waits; 3 frames late, a moment a frame still, inside the frame) */
                forced += !fits;
                w0 = mm->wpx;
                int whole = 0;
                for (uint32_t i = li->at[li->q]; i < li->at[li->q + 1]; i++) {
                    if (l == 4) whole += cell_draw(d, o, (int)li->e[i]);
                    else gput(d, li->e[i], li->col ? li->col[i] : 0, l == 2);
                }
                li->q++; done += cnt;
                m->work += (int)(cnt * MW_ITEM + (mm->wpx - w0) * MW_PX + whole * MW_SHAPE);
            }
            while (li->q <= qto && li->q < MP_NQ && li->at[li->q] >= (uint32_t)li->n) li->q++;   /* (the empty moments) */
            stepped += done;
            if (l < 2) qold = li->q < qold && !mlist_over(li) ? li->q : qold;
            if (l == 3 && done && !m->dg[FLUID_MD_LIN0]) m->dg[FLUID_MD_LIN0] = m->frames;
            if (l == 4 && (mm->got & MGOT_LINES) && mlist_over(li) && mlist_over(L[3]) && !m->dg[FLUID_MD_LINF]) m->dg[FLUID_MD_LINF] = m->frames;
            MP_LAP(lap[l]);
        }
        const int lus = (int)(now_us() - l0), lwk = m->work - lw0;   /* (their cost against the model's, for the next frames) */
        if (lwk > 500) { const float r = (float)lus / lwk, held = mm->lk - mm->lk / 16.f; mm->lk = r > held ? (r < 4.f ? r : 4.f) : held; }
    }
    /* the movers drawn where they are now: the pairs, the new grains that glide in (each that lands at rest there) */
    {
        const int64_t c0 = now_us();
        const int k0 = m->work;
        int resting = 0;
        w0 = mm->wpx;
        shimmer(mm, ts);
        for (int q = 0; q < mm->nf; q++) {
            mfly_t *p = &mm->fly[q];
            if (p->gone & 4) { resting++; continue; }
            if (t <= p->a * 1e-3f) continue;            /* (on its spot still) */
            int x, y; uint16_t c; uint8_t z;
            fly_at(d, p, t, ts, pull, &x, &y, &c, &z);
            moved += x != p->x || y != p->y || c != p->col || z != p->z;
            p->x = (int16_t)x; p->y = (int16_t)y; p->col = c; p->z = z;
            if (z) mdraw(d, x, y, z, c, p->un);
            if (p->x1 >= 0 && !(p->gone & 1) && t >= p->d * 1e-3f) { p->gone |= 4; mrest(d, x, y, z, p->un); }   /* (landed) */
            else if ((p->gone & 1) && !z && t > p->b * 1e-3f) p->gone |= 4;   /* (faded in the cloud) */
        }
        m->work += (int)((mm->nf - resting > 0 ? mm->nf - resting : 0) * MW_FLY + (mm->wpx - w0) * MW_PX);
        MP_LAP(FLUID_MP_COMPOSE);
        w0 = mm->wpx;
        for (int g = 0; g < ngl; g++) { came += glide_on(d, (uint32_t)g, t); resting += (gs[g].lv & 0x20) != 0; }
        m->work += (int)(came * MW_BORN + ngl * MW_LOOK + (mm->wpx - w0) * MW_PX);
        const int us = (int)(now_us() - c0), wk = m->work - k0;
        mm->cost_was = mm->cost_draw; mm->cost_draw = us > wk ? us : wk;
        if (!m->dg[FLUID_MD_MOVE0] && moved) m->dg[FLUID_MD_MOVE0] = m->frames;
        /* (the dust down: every pair at rest or faded, every glider at rest, every new grain up) */
        if (mm->paired && resting == mm->nf + ngl && (mm->got & MGOT_DUST) && mlist_over(&mm->nb) && !m->dg[FLUID_MD_SETTLE]) m->dg[FLUID_MD_SETTLE] = m->frames;
        if (m->dg[FLUID_MD_MOVE0] && !m->dg[FLUID_MD_SETTLE] && (!(moved + came) || rate <= 1)) m->dg[FLUID_MD_HANG]++;
    }
    MP_LAP(FLUID_MP_BORN);
    m->comp = moved + came + stepped;
    if (came > m->dg[FLUID_MD_BORN]) m->dg[FLUID_MD_BORN] = came;   /* (the log: its heaviest frames) */
    if (mm->wpx > m->dg[FLUID_MD_WPX]) m->dg[FLUID_MD_WPX] = mm->wpx;
    /* the end: the clock at its end, every list through, the dust down; the cells where a mover came to rest over a lit
     * pixel written again (a few: at once, the frame the dust is down) */
    bool caught = m->n >= qend && m->dg[FLUID_MD_SETTLE] && (mm->got & (MGOT_DUST | MGOT_LINES)) == (MGOT_DUST | MGOT_LINES);
    for (int l = 0; caught && l < 5; l++) caught = mlist_over(L[l]);
    if (caught) {
        float wk = 0.f;
        for (int k = 0; k < N; k++) if (mm->dmg[k]) wk += cell_whole(d, mm, k) ? cell_draw(d, o, k) * MW_SHAPE : cell_again(d, k) * MW_PX;
        m->work += (int)wk;
        mm->last = true;
    }
    MP_LAP(FLUID_MP_END);
    if (m->ready && !m->dg[FLUID_MD_READY]) m->dg[FLUID_MD_READY] = m->frames;
    if (!morph_inline(d, s, o)) return;
    MP_LAP(FLUID_MP_PAIR);
    for (int j = 0; j < H; j++) {                       /* (the cell rows written: whole, straight from the glass) */
        bool any = false;
        for (int y = d->cy[j]; y < d->cy[j + 1] && !any; y++) any = mm->rows[y];
        d->span[2 * j + 1] = (int16_t)(any ? W : 0);
    }
    MP_LAP(FLUID_MP_MARKS);
#undef MP_LAP
#undef MP_LEFT
}

/* The top of each cells pass in a change: the old picture read from the glass, a share of a frame at a time, before the
 * new look's inputs reach the drawer's state (its one pass, after: morph_frame chooses the pairs then). Till the clock
 * runs nothing is sent: those frames have FLUID_MORPH_IDLE_US */
void fluid_draw_morph_spent(fluid_draw_t *d, int us) { if (d) d->m.spent = us > 0 ? us : 1; }
void fluid_draw_morph_room(fluid_draw_t *d, int us) { if (d) d->m.room = us > 0 ? us : 0; }
static void morph_begin(fluid_draw_t *d) {
    fluid_morph_t *m = &d->m;
    m->work = m->comp = 0; m->pass = false;
    const bool idle = m->on && (m->na < 0 || !m->mm || !m->mm->planned);   /* (the drawer's pass among them: MP_LEFT keeps its room) */
    /* (a frame that sends: what the send it is likely to take leaves of the frame's FLUID_MORPH_FRAME_US) */
    const int send = m->sendp > FLUID_MORPH_SEND_US ? m->sendp : FLUID_MORPH_SEND_US;
    const int cells = FLUID_MORPH_FRAME_US - send < FLUID_MORPH_CELLS_US ? FLUID_MORPH_FRAME_US - send : FLUID_MORPH_CELLS_US;
    /* (sent on the next pulse, fluid_draw_morph_room: its cells have till then, counted from here) */
    const int gate = m->room - FLUID_MORPH_GATE_US > FLUID_MORPH_READ_MIN_US ? m->room - FLUID_MORPH_GATE_US : FLUID_MORPH_READ_MIN_US;
    m->budget = idle ? FLUID_MORPH_IDLE_US - (m->spent ? m->spent : FLUID_MORPH_SPENT_US)
              : m->room ? (gate < FLUID_MORPH_CELLS_US ? gate : FLUID_MORPH_CELLS_US)
              : cells - (m->spent ? m->spent : FLUID_MORPH_SPENT_US);
    m->room = 0;                                        /* (told again each frame) */
    m->c0 = now_us() - (m->spent ? m->spent : 0);
    m->spent = 0;                                       /* (told again each frame) */
    memset(m->prof, 0, sizeof(m->prof));
    if (!m->on) return;
    const int64_t t0 = now_us();
    if (m->na >= 0) { m->mm->f0 = t0; return; }
    const bool again = m->mm != NULL && m->mm->oj >= d->H;
    const int r = morph_read_old(d, m->budget);
    m->prof[FLUID_MP_OLD] = (int)(now_us() - t0);
    if (r < 0) { morph_end(d, true); return; }
    m->mm->f0 = t0;
    if (again) m->mm->again = true;
    if (!r) return;
    m->dg[FLUID_MD_OLD] = m->frames;
    fluid_morph_mem_t *mm = m->mm;
    m->dur = MP_LEN;
    m->n = m->w = 0; m->t = 0.f;
    if (!mm->again) m->px = m->wins = 0;
}
bool fluid_draw_morph_still(const fluid_draw_t *d) { return d && d->m.still; }
bool fluid_draw_morph_open(const fluid_draw_t *d) {
    const fluid_morph_t *m = d ? &d->m : NULL;
    return m && m->on && m->nb < 0 && (!m->mm || !m->mm->fframe);   /* (its specks' frame not worked out yet) */
}

/* ── its new picture read on another task ── */

void fluid_draw_morph_job(fluid_draw_t *d, bool on) { if (d) d->m.job = on; }
void fluid_draw_morph_work(fluid_draw_t *d, const fluid_frame_t *s, const fluid_draw_opts_t *o, int budget) {
    fluid_morph_mem_t *mm = d ? d->m.jmm : NULL;        /* (only the handoff sets it: never freed while this runs) */
    if (!mm || mm->jdone) return;
    const int64_t t_in = now_us();
    if (mm->jhurry || s_read_cells < 0) budget = INT32_MAX / 2;
    mm->rw = 0;
    memset(mm->jpart, 0, sizeof(mm->jpart));
    /* (a few rounds, each what the slice has left, in time and in the model's work: the dust's frame takes the slice's
     * first call, a part read whole returns; the dust once in is put in order first, it alone holds the pairs; the lists
     * made as each part is there) */
    for (int q = 0; q < 8 && !(mm->bready && mm->lready); q++) {
        const int wall = budget - (int)(now_us() - t_in), work = budget - mm->rw, b = wall < work ? wall : work;
        const bool sort = mm->dust && mm->sb.pass < 3;
        if (b <= 0) break;
        if (!morph_read(d, mm, s, o, sort ? 0 : b, sort ? b : 0, mm->jready, !q || mm->jhurry)) { mm->jfail = true; break; }
    }
    mm->jdone = mm->jfail || (mm->bready && mm->lready);
    if (budget < INT32_MAX / 4 && mm->rw - budget > d->m.jover) d->m.jover = mm->rw - budget;   /* (a step past its budget: tests) */
    const int us = (int)(now_us() - t_in);
    mm->jn++; mm->jus += us; mm->jmax = us > mm->jmax ? us : mm->jmax;
    fluid_morph_t *m = &d->m;
    if (us > m->jslow[0]) {                             /* (the change's longest slice, by step: the log's "change job") */
        m->jslow[0] = us; m->jslow[1] = budget < INT32_MAX / 4 ? budget : -1;
        memcpy(m->jslow + 2, mm->jpart, sizeof(mm->jpart));
    }
}
void fluid_draw_morph_handoff(fluid_draw_t *d) {
    if (!d) return;
    fluid_morph_t *m = &d->m;
    if (m->jfree) { m->jmm = NULL; mm_free(m->jfree); m->jfree = NULL; }   /* (ended in the drawer's frame: the job had it) */
    m->jhold = false;
    fluid_morph_mem_t *mm = m->on ? m->mm : NULL;
    /* its new look made (the drawer's pass), its old picture in order: the rest of the reading is the job's (never a change
     * another has just replaced: its memory is the next one's old picture, freed by the drawer once read, m->na < 0) */
    if (m->job && mm && m->na >= 0 && !m->jmm && mm->passed && !(mm->got & MGOT_DONE)) { mm->job = true; m->jmm = mm; }
    if (m->jmm) {
        if (m->jmm->chosen) m->jmm->knf = true;        /* (its pairs chosen: the drawer's, before this handoff) */
        morph_got(d);
        m->jmm->jready = m->ready; m->jmm->jhurry = m->jhurry;
    }
    m->still = mm && mm->passed;
}
/* (its new picture's kind is the pass's: the reader reads it as that) */
static void morph_pass(fluid_draw_t *d, bool hd) { if (d->m.mm) { d->m.mm->passed = d->m.pass = true; d->m.mm->hd = hd; } }
/* while a change runs the drawer stands (its state as the change read it), but for the one pass that makes its new look */
static inline bool morph_frozen(const fluid_draw_t *d) {
    const fluid_morph_t *m = &d->m;
    return m->on && m->mm && (m->na < 0 || m->prof[FLUID_MP_OLD] || m->mm->passed);   /* (its pass the frame after the read) */
}

/* EXPERIMENT kind 3: the hard dots of window t's cells (all, else the dirty ones), each 2 x 2 px dot on the pixels still
 * black once the drawing and the specks are drawn: never over a line, a speck or a letter */
static void sd_hard_rect(const fluid_draw_t *d, const fluid_rect_t *t, int bx, bool all) {
    const int W = d->W, H = d->H;
    for (int j = 0; j < H; j++) {
        const int ya = d->cy[j], yb = d->cy[j + 1];
        if (yb <= t->y0) continue;
        if (ya >= t->y1) break;
        for (int i = 0; i < W; i++) {
            const int k = j * W + i, hb = d->sd_hard[k];
            if (!hb || (!all && !d->dirty[k]) || d->cx[i + 1] <= t->x0 || d->cx[i] >= t->x1) continue;
            const uint16_t col = d->sd_hcol[k];
            for (int q = 0; q < 8; q++) {
                const int c = q >> 2, r = q & 3;
                if (!(hb >> fluid_braille_bit(c, r) & 1)) continue;
                const int x0 = d->dx[2 * i + c] > t->x0 ? d->dx[2 * i + c] : t->x0, x1 = d->dx[2 * i + c] + d->dot_w < t->x1 ? d->dx[2 * i + c] + d->dot_w : t->x1;
                const int y0 = d->dy[4 * j + r] > t->y0 ? d->dy[4 * j + r] : t->y0, y1 = d->dy[4 * j + r] + d->dot_h < t->y1 ? d->dy[4 * j + r] + d->dot_h : t->y1;
                for (int y = y0; y < y1; y++) {
                    uint16_t *row = rect_px(t, bx, y);
                    for (int x = x0; x < x1; x++) if (!row[x - bx]) row[x - bx] = col;
                }
            }
        }
    }
}

static bool s_lines_only;                               /* (draw_rect_layer: the look's lines alone, no dust) */
/* the shapes in the drawing's cache: all but the caption's letters, at the end */
static int cache_shapes(const fluid_draw_opts_t *o) {
    int n = o->nshapes < FLUID_MAX_SHAPES ? o->nshapes : FLUID_MAX_SHAPES;
    while (n > 0 && o->shapes[n - 1].tag == FLUID_TAG_CAPTION) n--;
    return n;
}
void fluid_draw_rect(const fluid_draw_t *d, const fluid_draw_opts_t *o, const fluid_rect_t *t, bool all) {
    const int W = d->W, H = d->H, bx = t->x0;
    const bool hd = o->scene && o->dots && o->shapes;
    uint32_t c1 = fluid_cycles();
    /* a scene on fine dust: its drawing from the cache (the caption's shapes, at the end, live) */
    fluid_draw_t *dm = (fluid_draw_t *)d;
    if (hd && o->fine) hdc_room(dm);
    const bool cached = hd && o->fine && d->hdc;
    const bool sd3 = hd && o->fine && d->sd_kind == FINE_DUST_FINE_DOTS && d->sd_hard;
    const int ncache = cached ? cache_shapes(o) : (o->nshapes < FLUID_MAX_SHAPES ? o->nshapes : FLUID_MAX_SHAPES);
    if (all) for (int y = t->y0; y < t->y1; y++) memset(rect_px(t, bx, y), 0, (size_t)(t->x1 - bx) * sizeof(uint16_t));
    for (int j = 0; j < H; j++) {
        const int ya = d->cy[j], yb = d->cy[j + 1];
        if (yb <= t->y0) continue;                                  /* the cell rows inside t only */
        if (ya >= t->y1) break;
        const int y0 = ya > t->y0 ? ya : t->y0, y1 = yb < t->y1 ? yb : t->y1;
        int i0 = all ? 0 : d->span[2 * j], i1 = all ? W : d->span[2 * j + 1];
        while (i0 < i1 && d->cx[i0 + 1] <= t->x0) i0++;             /* the cells inside t */
        while (i1 > i0 && d->cx[i1 - 1] >= t->x1) i1--;
        if (y0 >= y1 || i0 >= i1) continue;
        /* a scene's shapes: each pixel at its cell's weight (the cells drawn here, else 0) */
        int ha = t->x1, hb = t->x0;
        for (int i = i0; hd && i < i1; i++) {
            const int k = j * W + i, x0 = d->cx[i] > t->x0 ? d->cx[i] : t->x0, x1 = d->cx[i + 1] < t->x1 ? d->cx[i + 1] : t->x1;
            const int wt = (all || d->dirty[k]) && key_hd(d->key[k]) ? d->key[k] >> 8 & 15 : 0;
            if (x1 > x0) memset(d->hdw + x0, wt, (size_t)(x1 - x0));
            if (wt && x0 < ha) ha = x0;
            if (wt && x1 > hb) hb = x1;
        }
        /* the cells, one pixel row at a time (sequential writes) */
        for (int y = y0; y < y1; y++) {
            uint16_t *row = rect_px(t, bx, y);
            const int r = d->yrow[y];
            for (int i = i0; i < i1 && !all; i++) {     /* the changed cells, cleared (not those their cached drawing
                                                         * covers whole just below: one write a pixel less, PSRAM) */
                if (!d->dirty[j * W + i]) continue;
                const int x0 = d->cx[i] > t->x0 ? d->cx[i] : t->x0, x1 = d->cx[i + 1] < t->x1 ? d->cx[i + 1] : t->x1;
                if (cached && ha < hb && x1 > x0 && d->hdw[x0] && d->hdcv[j * W + i] == 2) continue;
                for (int x = x0; x < x1; x++) row[x - bx] = 0;
            }
            if (ha < hb && cached) {
                for (int i = i0; i < i1; i++) {
                    const int k = j * W + i, x0 = d->cx[i] > t->x0 ? d->cx[i] : t->x0, x1 = d->cx[i + 1] < t->x1 ? d->cx[i + 1] : t->x1;
                    if (x1 <= x0 || !d->hdw[x0]) continue;
                    if (!d->hdcv[k]) hdc_cell(dm, o, k, ncache);
                    if (d->hdcv[k] == 2) hdc_copy(d, o, row, bx, y, x0, x1);   /* (a black cell: the row is cleared already) */
                }
                if (ncache < o->nshapes)
                    fluid_shapes_row_dim(o->shapes + ncache, o->nshapes - ncache, (const int16_t (*)[4])(d->sb + ncache), d->ox, d->oy, o->palette, d->hdw,
                                         o->cap_unformed, NULL, row, bx, y, ha, hb);
            } else if (ha < hb) fluid_shapes_row_dim(o->shapes, o->nshapes, (const int16_t (*)[4])d->sb, d->ox, d->oy, o->palette, d->hdw, o->cap_unformed,
                                              o->fine ? o->fine->dim : NULL, row, bx, y, ha, hb);
            for (int i = i0; i < i1; i++) {
                const int k = j * W + i;
                if (!all && !d->dirty[k]) continue;
                const int cx0 = d->cx[i], cx1 = d->cx[i + 1];
                const int x0 = cx0 > t->x0 ? cx0 : t->x0, x1 = cx1 < t->x1 ? cx1 : t->x1;
                const uint16_t key = d->key[k], col = d->col[k];
                if (key == KEY_EMPTY || key == KEY_SKIP) continue;
                if (key < KEY_GLYPH || key_hd(key)) {        /* dots (beside a scene's shapes, never on them) */
                    if (r < 0 || (s_lines_only && !key_hd(key) && !(d->m.lc && d->m.lc[k]))) continue;   /* (dust dots) */
                    const bool beside = key_hd(key);
                    const unsigned own = key & ~(unsigned)(sd3 ? d->sd_hard[k] : 0);   /* (EXPERIMENT kind 3: the hard dots after the specks) */
                    for (int c = 0; c < 2; c++) {
                        if (!(own & (1u << fluid_braille_bit(c, r)))) continue;
                        int a = d->dx[i * 2 + c], b = a + d->dot_w;
                        if (a < t->x0) a = t->x0;
                        if (b > t->x1) b = t->x1;
                        for (int x = a; x < b; x++) if (!beside || !row[x - bx]) row[x - bx] = col;
                    }
                } else {
                    raster_glyph_row(row, bx, x0, x1, y, cx0, cx1, ya, yb, key, col, o);
                }
            }
        }
    }
    if (hd && o->fine && !s_lines_only) fluid_fine_rect(o->fine, d->dirty, t, all);   /* the fine dust over it all, never on the drawing */
    if (sd3 && !s_lines_only) sd_hard_rect(d, t, bx, all);   /* (EXPERIMENT kind 3) the hard dots over that, on black only */
    for (int n = 0; n < d->nlt; n++) {
        if (d->lt[n].mask)
            draw_mask(t, d->lt[n].mask, d->lt[n].mw, d->lt[n].mh, d->lt[n].x, d->lt[n].y, d->lt[n].w, d->lt[n].h, d->lt[n].c,
                      d->lt[n].cx0, d->lt[n].cy0, d->lt[n].cx1, d->lt[n].cy1);
        else
            draw_glyph(t, d->lt[n].gl, d->lt[n].x, d->lt[n].y, d->lt[n].w, d->lt[n].h, d->lt[n].c,
                       d->lt[n].cx0, d->lt[n].cy0, d->lt[n].cx1, d->lt[n].cy1);
    }
    FLUID_PROF_END(FP_DRASTER, c1);
}
/* the window t as fluid_draw_rect draws it whole, or its lines alone: without the dust (a scene's specks, the dots of
 * cells that are not lines) */
static void draw_rect_layer(const fluid_draw_t *d, const fluid_draw_opts_t *o, const fluid_rect_t *t, bool lines_only) {
    s_lines_only = lines_only;
    fluid_draw_rect(d, o, t, true);
    s_lines_only = false;
}

/* Crisp dots' changed cells to the panel, in few windows: a window costs about as much as CRISP_WIN_PX pixels (its
 * commands and its wait against a pixel's ~0.1-0.2 us on the CO5300's quad SPI, measured; a scene on fine dust draws a
 * pixel slower: WAVE_WIN_PX), so each row sends its whole span
 * of changed cells, and the next rows join it while one box over them costs less than separate windows (and fits the
 * buffer). Windows start and end on even rows: a panel that takes whole row pairs only (the CO5300) otherwise resends
 * a full-width pair at each odd edge. */
#define CRISP_WIN_PX 1500
#define WAVE_WIN_PX  354                                /* (a scene on fine dust) */
#define GLASS_WIN_PX 3300                               /* (... over its kept glass: a pixel sent from it, ~0.058 us, against 0.19 ms: bench3) */
#define MORPH_WIN_PX 3000                               /* (a change: its pixels mostly copied, 0.05 us against a window's 0.15 ms) */
/* the windows' buffers: with two, each window is drawn into the one the panel is not reading (the send may return
 * while the panel still reads the other: fluid_draw_send2) */
typedef struct {
    uint16_t *buf[2]; int max_px, cur; fluid_send_fn send; void *ctx;
    bool glass;                                         /* the windows copied from the kept glass (fluid_draw_send2) */
    int win_px; bool dry; long cost;                    /* crisp_send: a window's cost in pixels; dry: its boxes' cost counted, not sent */
} sender_t;
static int s_wins;                                      /* windows sent (a change's counted for the log) */
static fluid_glass_fn s_gq;                             /* the kept glass's windows handed to another task (fluid_draw_glass_sender) */
static void *s_gq_ctx;
void fluid_draw_glass_sender(fluid_glass_fn fn, void *ctx) { s_gq = fn; s_gq_ctx = ctx; }
/* a window of the kept glass to the sender (copied and the mark over it, or direct: whole rows straight from it), or with
 * no sender whole rows sent here (a copied one goes through window()) */
static int glass_window(const fluid_draw_t *d, const fluid_draw_opts_t *o, int mark, sender_t *sd, int x0, int y, int w, int h, bool direct) {
    const int64_t t0 = now_us();
    const int e = s_gq ? s_gq(s_gq_ctx, d->m.snap, d->fb_w, x0, y, w, h, direct, mark, o->palette)
                       : sd->send(sd->ctx, 0, y, w, h, d->m.snap + (size_t)y * d->fb_w);
    ((fluid_morph_t *)&d->m)->push_us += (int)(now_us() - t0);
    s_wins++;
    return e != 0 ? -1 : w * h;
}
static int window(const fluid_draw_t *d, const fluid_draw_opts_t *o, const fluid_overlay_t *ov, int mark, sender_t *sd,
                  int x0, int y, int x1, int h) {
    if (sd->glass && s_gq) return glass_window(d, o, mark, sd, x0, y, x1 - x0, h, false);
    uint16_t *buf = sd->buf[sd->cur];
    const int w = x1 - x0;
    const fluid_rect_t t = { buf, w, x0, y, x1, y + h };
    const int64_t t0 = now_us();
    const fluid_morph_t *m = &d->m;
    if (m->on || sd->glass)                             /* the glass as composed (morph_frame, or the changed cells: fluid_draw_send2) */
        for (int yy = y; yy < y + h; yy++) memcpy(buf + (size_t)(yy - y) * w, m->snap + (size_t)yy * d->fb_w + x0, sizeof(uint16_t) * (size_t)w);
    else {
        fluid_draw_rect(d, o, &t, true);
        if (m->snap && (m->look & FLUID_LOOK_KEEP))     /* a kept look: the glass as it is sent */
            for (int yy = y; yy < y + h; yy++) memcpy(m->snap + (size_t)yy * d->fb_w + x0, buf + (size_t)(yy - y) * w, sizeof(uint16_t) * (size_t)w);
    }
    fluid_draw_overlay(d, &t, o->palette, ov);
    fluid_draw_mark(d, &t, o->palette, mark);
    const int64_t t1 = now_us();
    if (sd->send(sd->ctx, x0, y, w, h, buf) != 0) return -1;
    fluid_morph_t *mm = (fluid_morph_t *)m;              /* (the frame's time drawing its windows, and handing them over; a
                                                         * copy of the kept glass counts with its hand-over: cmp is then the
                                                         * changed cells drawn into the glass) */
    if (sd->glass) mm->push_us += (int)(now_us() - t0);
    else { mm->cmp_us += (int)(t1 - t0); mm->push_us += (int)(now_us() - t1); }
    s_wins++;
    if (sd->buf[1]) sd->cur ^= 1;
    return w * h;
}
static int crisp_box(const fluid_draw_t *d, const fluid_draw_opts_t *o, const fluid_overlay_t *ov, int mark, int x0, int x1,
                     int y0, int y1, sender_t *sd) {
    const int w = x1 - x0, rows = (sd->max_px / w) & ~1;
    int sent = 0;
    if (rows < 2) return -1;
    if (sd->dry) { sd->cost += (long)sd->win_px * ((y1 - y0 + rows - 1) / rows) + (long)w * (y1 - y0); return 0; }
    for (int y = y0; y < y1; y += rows) {
        const int n = window(d, o, ov, mark, sd, x0, y, x1, y1 - y < rows ? y1 - y : rows);
        if (n < 0) return -1;
        sent += n;
    }
    return sent;
}
/* (the changed cells of cell rows [ja, jb)) */
static int crisp_send(const fluid_draw_t *d, const fluid_draw_opts_t *o, const fluid_overlay_t *ov, int mark, sender_t *sd, int ja, int jb) {
    const int W = d->W, win_px = sd->win_px;
    int sent = 0;
#define XA(i) (d->cx[i] & ~3)
#define XB(i) (((d->cx[i] + 3) & ~3) < d->fb_w ? ((d->cx[i] + 3) & ~3) : d->fb_w)
#define YA(j) (d->cy[j] & ~1)
#define YB(j) (((d->cy[(j) + 1] + 1) & ~1) < d->fb_h ? ((d->cy[(j) + 1] + 1) & ~1) : d->fb_h)
    /* open boxes (cells [a, b) x rows [j0, j1)): a row's runs of changed cells (split where the gap costs more than a
     * window) join the box above them when one box costs less than two, else start their own; a box no run joined
     * is sent */
    enum { NB = 64 };
    struct { int16_t a, b, j0, j1; } box[NB], nxt[NB];
    int nb = 0;
    for (int j = ja; j <= jb; j++) {
        int nn = 0;
        if (j < jb && d->span[2 * j] < d->span[2 * j + 1]) {
            const int ch = YB(j) - YA(j);
            for (int c = d->span[2 * j], r0 = -1, last = -1; c <= d->span[2 * j + 1]; c++) {
                const bool dt = c < d->span[2 * j + 1] && d->dirty[j * W + c];
                if (!dt && c < d->span[2 * j + 1]) continue;
                if (r0 >= 0 && (!dt || (XA(c) - XB(last + 1)) * ch > win_px)) {
                    /* the run [r0, last]: into the open box it costs least to join, if any is cheaper than its own */
                    int best = -1, gain = 0;
                    for (int q = 0; q < nb; q++) {
                        if (box[q].j1 != j) continue;
                        const int na = box[q].a < r0 ? box[q].a : r0, nbb = box[q].b > last + 1 ? box[q].b : last + 1;
                        const int jw = XB(nbb) - XA(na), jh = YB(j) - YA(box[q].j0), ow = XB(box[q].b) - XA(box[q].a), oh = YB(j - 1) - YA(box[q].j0);
                        const int jr = (sd->max_px / jw) & ~1, orr = (sd->max_px / ow) & ~1;   /* a box goes in chunks of a buffer */
                        if (jr < 2 || orr < 2) continue;
                        const int joined = jw * jh + win_px * ((jh + jr - 1) / jr - 1);
                        const int apart = ow * oh + win_px * ((oh + orr - 1) / orr - 1) + (XB(last + 1) - XA(r0)) * (YB(j) - YB(j - 1)) + win_px;
                        if (apart - joined > gain) { gain = apart - joined; best = q; }
                    }
                    if (best >= 0) {
                        if (r0 < box[best].a) box[best].a = (int16_t)r0;
                        if (last + 1 > box[best].b) box[best].b = (int16_t)(last + 1);
                        box[best].j1 = (int16_t)(j + 1);
                    } else if (nn < NB) { nxt[nn].a = (int16_t)r0; nxt[nn].b = (int16_t)(last + 1); nxt[nn].j0 = (int16_t)j; nxt[nn].j1 = (int16_t)(j + 1); nn++; }
                    else {                              /* no room to hold it open: it goes now */
                        const int n = crisp_box(d, o, ov, mark, XA(r0), XB(last + 1), YA(j), YB(j), sd);
                        if (n < 0) return -1;
                        sent += n;
                    }
                    r0 = -1;
                }
                if (dt) { if (r0 < 0) r0 = c; last = c; }
            }
        }
        int keep = 0;
        for (int q = 0; q < nb; q++) {                  /* a box no run of this row joined goes out */
            if (box[q].j1 == j + 1 && j < jb) { box[keep++] = box[q]; continue; }
            const int n = crisp_box(d, o, ov, mark, XA(box[q].a), XB(box[q].b), YA(box[q].j0), YB(box[q].j1 - 1), sd);
            if (n < 0) return -1;
            sent += n;
        }
        nb = keep;
        for (int q = 0; q < nn; q++) {
            if (nb < NB) { box[nb++] = nxt[q]; continue; }
            const int n = crisp_box(d, o, ov, mark, XA(nxt[q].a), XB(nxt[q].b), YA(nxt[q].j0), YB(nxt[q].j1 - 1), sd);
            if (n < 0) return -1;
            sent += n;
        }
    }
#undef XA
#undef XB
#undef YA
#undef YB
    return sent;
}
/* Over a kept glass the drawing's cache is made where a changed cell needs it within GLASS_CACHE_US a frame, and cells are
 * drawn whole up to GLASS_DRAW_PX a frame (cells in a scattered order); a cell with no room is owed (drawn and sent the
 * next frame, and its animation counted late that frame), so a change over most of the glass (a set of dust coming in, a
 * shape change) arrives over two or three frames, never as one long one. Every frame of a tap or a reading stays under
 * the bound (bench6: 15-31 kpx drawn a frame). A light frame makes the cache ahead (GLASS_AHEAD_US). */
#define GLASS_CACHE_US 8000
#define GLASS_DRAW_PX  40000                            /* (~19 ms at bench4's 0.47 us a px drawn into the glass) */
#define GLASS_LIGHT_US 12000
#define GLASS_AHEAD_US 3000
static inline int cell_px(const fluid_draw_t *d, int k) { return (d->cx[k % d->W + 1] - d->cx[k % d->W]) * (d->cy[k / d->W + 1] - d->cy[k / d->W]); }
static inline bool cache_wanted(const fluid_draw_t *d, int k) { return !d->hdcv[k] && key_hd(d->key[k]) && (d->key[k] >> 8 & 15); }
static void glass_cache(fluid_draw_t *d, const fluid_draw_opts_t *o, int64_t t0) {
    const int N = d->W * d->H, nc = cache_shapes(o);
    if (!hdc_room(d)) return;
    for (int q = 0, k = 0; q < N; q++, k = (k + 997) % N) {   /* (997: prime to the grid's cells, a scattered order) */
        if (!d->dirty[k]) continue;
        const int px = cell_px(d, k);
        if (d->ndrawn + px <= GLASS_DRAW_PX && (!cache_wanted(d, k) || now_us() - t0 < GLASS_CACHE_US)) {
            if (cache_wanted(d, k)) hdc_cell(d, o, k, nc);
            d->ndrawn += px;
            continue;
        }
        d->dirty[k] = 0; d->owe[k] = 1; d->nowed++; d->anim_late |= d->why[k];
    }
}
/* the changed cells whose dust alone moved, over black or over their drawing as cached (no dot or letter in them; by the
 * caption only black ones no letter of it reaches, as its backdrop dims a drawing): only their specks and the word's
 * breaking specks in them moved (fluid_fine_move: a few pixels, not the cell's), the cell left out of the drawing pass (glass_move) and back after it,
 * to be sent (glass_moved, cell rows [ja, jb)) */
static bool by_letter(const fluid_draw_t *d, const fluid_draw_opts_t *o, int k) {
    const int i = k % d->W, j = k / d->W;
    for (int n = 0; n < o->nshapes && n < FLUID_MAX_SHAPES; n++) {
        const int16_t *b = d->sb[n];
        if (o->shapes[n].tag == FLUID_TAG_CAPTION && b[0] < d->cx[i + 1] && b[1] > d->cx[i] && b[2] < d->cy[j + 1] && b[3] > d->cy[j]) return true;
    }
    return false;
}
static void glass_move(fluid_draw_t *d, const fluid_draw_opts_t *o) {
    const int W = d->W, N = W * d->H;
    int n = 0;
    for (int k = 0; k < N; k++) {
        const int i = k % W, j = k / W;
        d->owe[k] &= ~2;
        if (d->dirty[k] != 3 || !d->hdcv[k] || !key_hd(d->key[k]) || (d->key[k] & 0xFFu) ||
            (i >= d->hold[0] && i <= d->hold[1] && j >= d->hold[2] && j <= d->hold[3] && (d->hdcv[k] != 1 || by_letter(d, o, k))) ||
            !fluid_fine_known(o->fine, k)) continue;
        d->dirty[k] = 0; d->owe[k] |= 2; n++;
    }
    if (n) fluid_fine_move(o->fine, d->owe, d->m.snap, d->hdc, d->hdcv, d->fb_w);
}
static void glass_moved(fluid_draw_t *d, int ja, int jb) {
    for (int k = ja * d->W; k < jb * d->W; k++) if (d->owe[k] & 2) { d->owe[k] &= ~2; d->dirty[k] = 1; }
}
static void glass_ahead(fluid_draw_t *d, const fluid_draw_opts_t *o) {
    const int N = d->W * d->H, nc = cache_shapes(o);
    const int64_t t0 = now_us();
    for (int q = 0; q < N; q++, d->hdc_next = (d->hdc_next + 1) % N) {
        if (!cache_wanted(d, d->hdc_next)) continue;
        if (now_us() - t0 >= GLASS_AHEAD_US) break;
        hdc_cell(d, o, d->hdc_next, nc);
    }
}
/* A scene on fine dust over its kept glass: each band of cell rows with changes, its changed cells drawn into the glass (a
 * cell row at a time: its specks go into the glass's lines while they are still in the cache, PSRAM), then sent as boxes
 * copied from the glass (crisp_send), or as its rows whole straight from the glass when that costs less (one window, no
 * copy: a wave's ring); the mark's rows always through a window (it is drawn over one, never into the glass). With a
 * sender (fluid_draw_glass_sender) a band (or a part of one, GLASS_CHUNK, cut where a cell row starts on an even pixel
 * row) goes out on the other core while this one draws the next: a band's windows
 * reach a row before the next band's cells (a whole cell row apart) and the glass's rows they send are not drawn again
 * this frame. */
#define GLASS_CHUNK 6                                   /* a band in parts of about this many cell rows (a wave's ring is one band
                                                         * of nearly every row: in parts, the sender starts on the first while this
                                                         * core draws the next; a part costs a window or two more) */
static int glass_send(fluid_draw_t *d, const fluid_draw_opts_t *o, const fluid_overlay_t *ov, int mark, sender_t *sd) {
    const int H = d->H, fw = d->fb_w;
    const int ym = mark > FLUID_MARK_NONE ? (d->oy + d->stage - (int)(16.f * d->stage / (float)FLUID_STAGE_REF)) & ~1 : d->fb_h;
    fluid_morph_t *m = &d->m;
    int sent = 0;
    for (int j = 0, e; j < H; j = e) {
        if (d->span[2 * j] >= d->span[2 * j + 1]) { e = j + 1; continue; }
        for (e = j; e < H && d->span[2 * e] < d->span[2 * e + 1]; e++) {}
        for (int c = j + GLASS_CHUNK; c < e; c++) if (!(d->cy[c] & 1)) { e = c; break; }   /* (cut on an even row: no pair shared) */
        const int64_t t0 = now_us();
        for (int r = j; r < e; r++)
            fluid_draw_rect(d, o, &(fluid_rect_t){ m->snap + (size_t)d->cy[r] * fw, fw, 0, d->cy[r], fw, d->cy[r + 1] }, false);
        glass_moved(d, j, e);
        m->cmp_us += (int)(now_us() - t0); d->draw_us += (int)(now_us() - t0);
        const int y0 = d->cy[j] & ~1, y1 = ((d->cy[e] + 1) & ~1) < d->fb_h ? (d->cy[e] + 1) & ~1 : d->fb_h, yd = y1 < ym ? y1 : ym;
        sd->dry = true; sd->cost = 0;
        crisp_send(d, o, ov, mark, sd, j, e);
        sd->dry = false;
        int n;
        if (yd > y0 && sd->win_px + (long)fw * (yd - y0) < sd->cost) {
            n = glass_window(d, o, mark, sd, 0, y0, fw, yd - y0, true);
            if (n >= 0 && yd < y1) { const int m2 = crisp_box(d, o, ov, mark, 0, fw, yd, y1, sd); n = m2 < 0 ? -1 : n + m2; }
        } else n = crisp_send(d, o, ov, mark, sd, j, e);
        if (n < 0) return -1;
        sent += n;
    }
    return sent;
}

/* A change's frame to the panel: each band of cell rows with changed cells as one window of whole rows straight from the
 * kept glass (it holds the change as composed: no copy, no buffer's limit), or as boxes through the buffers where that
 * costs less, and always under an overlay or the mark (drawn over the window, never into the glass) */
static inline int mark_rows(const fluid_draw_t *d, int mark) {   /* (the mark's rows: from here down) */
    return mark > FLUID_MARK_NONE ? ((d->oy + d->stage - (int)(16.f * ((float)d->stage / (float)FLUID_STAGE_REF))) & ~1) : d->fb_h;
}
static int morph_send(const fluid_draw_t *d, const fluid_draw_opts_t *o, const fluid_overlay_t *ov, int mark, sender_t *sd) {
    const int W = d->W, H = d->H, fw = d->fb_w;
    const fluid_morph_t *m = &d->m;
    const int ym = mark_rows(d, mark);
    const bool direct = !fluid_overlay_on(ov);
    int sent = 0;
    for (int j = 0; j < H;) {
        if (d->span[2 * j] >= d->span[2 * j + 1]) { j++; continue; }
        int e = j, a = W, b = 0;
        for (; e < H && d->span[2 * e] < d->span[2 * e + 1]; e++) {
            if (d->span[2 * e] < a) a = d->span[2 * e];
            if (d->span[2 * e + 1] > b) b = d->span[2 * e + 1];
        }
        const int y0 = d->cy[j] & ~1, y1 = ((d->cy[e] + 1) & ~1) < d->fb_h ? (d->cy[e] + 1) & ~1 : d->fb_h;
        const int xa = d->cx[a] & ~3, xb = ((d->cx[b] + 3) & ~3) < fw ? (d->cx[b] + 3) & ~3 : fw;
        int yd = direct ? (y1 < ym ? y1 : ym) : y0;     /* rows straight from the glass: [y0, yd) */
        if (yd > y0) {                                  /* .. when one window of whole rows costs less than boxes of the band */
            const int h = yd - y0, rows = (sd->max_px / (xb - xa)) & ~1;
            const long whole = MORPH_WIN_PX + (long)fw * h, boxes = (long)MORPH_WIN_PX * (rows >= 2 ? (h + rows - 1) / rows : h) + (long)(xb - xa) * h;
            if (whole > boxes) yd = y0;
        }
        if (yd > y0) {
            const int64_t t0 = now_us();
            if (sd->send(sd->ctx, 0, y0, fw, yd - y0, m->snap + (size_t)y0 * fw) != 0) return -1;
            ((fluid_morph_t *)m)->push_us += (int)(now_us() - t0);
            s_wins++; sent += fw * (yd - y0);
        }
        if (yd < y1) {
            const int n = crisp_box(d, o, ov, mark, xa, xb, yd, y1, sd);
            if (n < 0) return -1;
            sent += n;
        }
        j = e;
    }
    return sent;
}

int fluid_draw_send(const fluid_draw_t *d, const fluid_draw_opts_t *o, const fluid_overlay_t *ov, int mark, bool full,
                    uint16_t *buf, int max_px, fluid_send_fn send, void *ctx) {
    return fluid_draw_send2(d, o, ov, mark, full, buf, NULL, max_px, send, ctx);
}

static inline bool hd_glass_keeps(const fluid_draw_t *d) { return d->m.snap && (d->m.look & FLUID_LOOK_KEEP) && !d->m.on; }
int fluid_draw_send2(const fluid_draw_t *d, const fluid_draw_opts_t *o, const fluid_overlay_t *ov, int mark, bool full,
                     uint16_t *buf, uint16_t *buf2, int max_px, fluid_send_fn send, void *ctx) {
    sender_t sd = { { buf, buf2 }, max_px, 0, send, ctx, false, 0, false, 0 };
    int sent = 0;
    /* a scene in high definition on the dots: each run of changed cells its own window, so its haze changing all over
     * does not redraw the shapes between */
    const bool runs = o->scene && o->dots && o->shapes;
    fluid_morph_t *m = (fluid_morph_t *)&d->m;
    if (m->on && full) morph_end((fluid_draw_t *)d, false);   /* (all of the look sent at once: the change is over) */
    if (m->jhold) return 0;                             /* (its reader may still be in the drawing's state: from the next frame) */
    if (m->full) { full = true; m->full = false; }
    m->fulls = full ? (uint8_t)(m->fulls < 255 ? m->fulls + 1 : 255) : 0;
    if (m->on && o->fine) memset(o->fine->dok, 0, (size_t)d->W * d->H);   /* (the glass is the change's: no speck as last drawn) */
    const int n0 = s_wins;
    m->cmp_us = m->push_us = 0;
    /* crisp dots alone (a rigged creature), and a scene on fine dust: few, larger windows on whole row pairs (crisp_send:
     * a panel that takes whole pairs only, the CO5300, otherwise resends a full-width pair at each odd edge) */
    if (m->on && !full) {
        const int64_t t0 = now_us();
        const int n = morph_send(d, o, ov, mark, &sd);
        m->px = n; m->wins = s_wins - n0;
        const int us = (int)(now_us() - t0);
        if (n > 0) { const int held = m->sendp - m->sendp / 16; m->sendp = us > held ? us : held; }   /* (the next one's guess: its peaks held) */
        if (n > 0) {   /* (its three frames that sent with the longest cells, for the log: the gate waits on those; what moved in
                        * them, what the still layers turned) */
            const int cells = (int)(t0 - m->c0), *pf = m->prof;
            int big = 0;
            for (int p = 1; p < FLUID_MP_N; p++) if (pf[p] > pf[big]) big = p;
            int q = 3;
            while (q > 0 && cells > m->top[q - 1][1]) q--;
            if (q < 3) {
                memmove(m->top[q + 1], m->top[q], sizeof(m->top[0]) * (size_t)(2 - q));
                m->top[q][0] = m->frames; m->top[q][1] = cells; m->top[q][2] = us; m->top[q][3] = big;
                m->top[q][4] = pf[FLUID_MP_FLY] + pf[FLUID_MP_COMPOSE] + pf[FLUID_MP_BORN];
                m->top[q][5] = pf[FLUID_MP_SPARE] + pf[FLUID_MP_LINES] + pf[FLUID_MP_END];
            }
        }
        if (m->mm && m->mm->last) {                     /* (its last frame: the look whole, its specks as the dust has them) */
            morph_end((fluid_draw_t *)d, false);
            if (o->fine) fluid_fine_drawn(o->fine);
        }
        return n;
    }
    if (o->scene && o->dots && (!o->shapes || o->fine) && !full) {
        /* a scene on fine dust over its kept glass (the panel's as sent): its changed cells drawn into it, alone, and
         * every box copied from it, so a box costs a copy for what did not change (merged boxes, fewer windows) */
        sd.glass = o->shapes && !d->sd_kind && m->whole && m->snap && (m->look & FLUID_LOOK_KEEP) && !fluid_overlay_on(ov);
        fluid_draw_t *dm = (fluid_draw_t *)d;
        dm->nbuilt = dm->ndrawn = dm->move_us = dm->cache_us = dm->draw_us = 0;
        if (sd.glass) {
            const int64_t t0 = now_us();
            glass_move(dm, o);                          /* (first: what moves alone is not drawn whole) */
            const int64_t t1 = now_us();
            glass_cache(dm, o, t1);
            dm->move_us = (int)(t1 - t0); dm->cache_us = (int)(now_us() - t1);
            m->cmp_us += (int)(now_us() - t0);
        }
        sd.win_px = sd.glass ? GLASS_WIN_PX : o->shapes ? (d->sd_kind ? SD_WIN_PX : WAVE_WIN_PX) : CRISP_WIN_PX;
        const int n = sd.glass ? glass_send(dm, o, ov, mark, &sd) : crisp_send(d, o, ov, mark, &sd, 0, d->H);
        if (sd.glass && m->cmp_us + m->push_us < GLASS_LIGHT_US) glass_ahead(dm, o);
        if (o->fine && n >= 0) fluid_fine_sent(o->fine);
        return n;
    }
    for (int j = 0; j < (full ? 1 : d->H); j++) {
        int a = full ? 0 : d->span[2 * j];
        const int end = full ? 0 : d->span[2 * j + 1];
        if (!full && a >= end) continue;
        do {
            int last = end - 1;
            if (runs) {                                 /* split only where shapes sit between */
                last = a;
                for (int c = a + 1, gs = 0; c < end; c++) {
                    const uint16_t kk = d->key[j * d->W + c];
                    if (d->dirty[j * d->W + c]) { last = c; gs = 0; }
                    else if (key_hd(kk) && (kk >> 8 & 15) && ++gs >= RUN_GAP) break;
                }
            }
            int x0 = 0, x1 = d->fb_w, y0 = 0, y1 = d->fb_h;
            if (!full) {
                x0 = d->cx[a] & ~3; x1 = (d->cx[last + 1] + 3) & ~3;
                if (x1 > d->fb_w) x1 = d->fb_w;
                y0 = d->cy[j]; y1 = d->cy[j + 1];
            }
            const int w = x1 - x0, rows = max_px / w;
            if (rows < 1) return -1;
            for (int y = y0; y < y1; y += rows) {
                const int n = window(d, o, ov, mark, &sd, x0, y, x1, y1 - y < rows ? y1 - y : rows);
                if (n < 0) return -1;
                sent += n;
            }
            for (a = last + 1; a < end && !d->dirty[j * d->W + a]; a++) {}
        } while (!full && a < end);
    }
    /* (all of a kept scene's look sent at once, its glass with it: every cell's specks are known as drawn, so the dust's
     * next moves go on from them, not from each cell drawn whole once more) */
    if (full && o->fine && hd_glass_keeps(d)) fluid_fine_drawn(o->fine);
    else if (o->fine) fluid_fine_sent(o->fine);
    return sent;
}

int fluid_draw(fluid_draw_t *d, const fluid_frame_t *s, uint16_t *fb,
               const fluid_draw_opts_t *o, int *y0out, int *y1out) {
    const int drawn = fluid_draw_cells(d, s, o);
    const fluid_rect_t t = { fb, d->fb_w, 0, 0, d->fb_w, d->fb_h };
    fluid_draw_rect(d, o, &t, false);
    int dy0 = d->fb_h, dy1 = 0;
    for (int j = 0; j < d->H; j++) {
        if (d->span[2 * j] >= d->span[2 * j + 1]) continue;
        if (d->cy[j] < dy0) dy0 = d->cy[j];
        if (d->cy[j + 1] > dy1) dy1 = d->cy[j + 1];
    }
    *y0out = dy1 > dy0 ? dy0 : 0;
    *y1out = dy1 > dy0 ? dy1 : 0;
    return drawn;
}

int fluid_draw_mark(const fluid_draw_t *d, const fluid_rect_t *t, int palette, int mark) {
    if (!d || !t || !t->px || mark <= FLUID_MARK_NONE || mark >= FLUID_MARK_COUNT) return 0;
    const int pal = (palette >= 0 && palette < FLUID_PAL_COUNT) ? palette : FLUID_PAL_PASTEL;
    const float *lo = PAL[pal][0], *c = mark == FLUID_MARK_ALERT ? PAL[pal][2] : PAL[pal][1];
    const float k = mark == FLUID_MARK_ALERT ? 0.8f : 0.4f;           /* subtle: well below the fluid */
    const uint16_t col = rgb565_be((int)(lo[0] + (c[0] - lo[0]) * k), (int)(lo[1] + (c[1] - lo[1]) * k),
                                   (int)(lo[2] + (c[2] - lo[2]) * k));
    const float sc = (float)d->stage / (float)FLUID_STAGE_REF;
    const float cx = d->ox + d->stage * 0.5f, cy = d->oy + d->stage * 0.5f;
    const float r = d->stage * 0.5f - 12.f * sc;                      /* inside the panel's rim */
    const float half = fmaxf(1.f, 1.5f * sc), dot = fmaxf(2.f, 3.f * sc), span = r * 0.15f;   /* arc ≈ ±8.6° */
    const bool arc = mark == FLUID_MARK_ARC;
    const float ext = arc ? span : dot;
    int x0 = (int)(cx - ext) - 1, x1 = (int)(cx + ext) + 2;
    int y0 = (int)(arc ? cy + sqrtf(r * r - span * span) - half : cy + r - dot) - 1, y1 = (int)(cy + r + (arc ? half : dot)) + 2;
    if (x0 < t->x0) x0 = t->x0;
    if (y0 < t->y0) y0 = t->y0;
    if (x1 > t->x1) x1 = t->x1;
    if (y1 > t->y1) y1 = t->y1;
    int n = 0;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
        float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
        bool on;
        if (arc) { float dr = sqrtf(dx * dx + dy * dy) - r; on = dy > 0.f && fabsf(dx) <= span && fabsf(dr) <= half; }
        else { float ey = dy - r; on = dx * dx + ey * ey <= dot * dot; }
        if (on) { *rect_px(t, x, y) = col; n++; }
    }
    return n;
}

/* ── overlays: a question's dividers ─────────────────────────────────────── */

#define DIV_FROM     0.06f    /* dividers start this far out (of the radius) ...          */
#define DIV_INSET    10.f     /* ...and stop this many px inside the rim                  */
#define DIV_ALPHA    0.32f    /* faint: the palette's bright colour at this opacity        */

static inline void ov_geom(const fluid_draw_t *d, float *cx, float *cy, float *sc) {
    *sc = (float)d->stage / (float)FLUID_STAGE_REF;
    *cx = d->ox + d->stage * 0.5f; *cy = d->oy + d->stage * 0.5f;
}

/* the cell under framebuffer px (x, y); false outside the grid */
static bool cell_at(const fluid_draw_t *d, int x, int y, int *k) {
    if (x < d->cx[0] || x >= d->cx[d->W] || y < d->cy[0] || y >= d->cy[d->H]) return false;
    int i = (int)((float)(x - d->ox) * d->W / d->stage), j = (int)((float)(y - d->oy) * d->H / d->stage);
    if (i < 0) i = 0;
    if (i > d->W - 1) i = d->W - 1;
    if (j < 0) j = 0;
    if (j > d->H - 1) j = d->H - 1;
    while (i > 0 && x < d->cx[i]) i--;
    while (i < d->W - 1 && x >= d->cx[i + 1]) i++;
    while (j > 0 && y < d->cy[j]) j--;
    while (j < d->H - 1 && y >= d->cy[j + 1]) j++;
    *k = j * d->W + i;
    return true;
}

void fluid_draw_overlay_dirty(fluid_draw_t *d, const fluid_overlay_t *ov) {
    if (!d || !ov || !fluid_overlay_on(ov)) return;
    float cx, cy, sc; ov_geom(d, &cx, &cy, &sc);
    if (ov->sectors >= 2 && ov->divider > 0.f) {
        const float R = d->stage * 0.5f, r0 = DIV_FROM * R, r1 = R - DIV_INSET * sc;
        for (int q = 0; q < ov->sectors; q++) {
            float a = 6.2831853f * q / ov->sectors, ux = sinf(a), uy = -cosf(a);
            for (float t = r0; t <= r1 + 1.f; t += 1.f) {
                float x = cx + ux * t, y = cy + uy * t;
                for (int c = 0; c < 4; c++) {         /* the anti-aliased line covers 2 px */
                    int k;
                    if (cell_at(d, (int)floorf(x + (c & 1 ? 1.f : -1.f)), (int)floorf(y + (c & 2 ? 1.f : -1.f)), &k))
                        d->key[k] = KEY_STALE;
                }
            }
        }
    }
}

/* blend colour (r, g, b) at opacity a over one big-endian rgb565 pixel */
static inline void blend(uint16_t *px, float r, float g, float b, float a) {
    if (a <= 0.f) return;
    if (a > 1.f) a = 1.f;
    uint16_t le = (uint16_t)((*px >> 8) | (*px << 8));
    float r0 = (float)((le >> 11) & 31) * (255.f / 31.f), g0 = (float)((le >> 5) & 63) * (255.f / 63.f), b0 = (float)(le & 31) * (255.f / 31.f);
    *px = rgb565_be((int)(r0 + (r - r0) * a + 0.5f), (int)(g0 + (g - g0) * a + 0.5f), (int)(b0 + (b - b0) * a + 0.5f));
}

/* one anti-aliased (Wu) line, clipped to the window t */
static int wu_line(const fluid_rect_t *t, float x0, float y0, float x1, float y1, const float *c, float a) {
    int n = 0;
    const bool steep = fabsf(y1 - y0) > fabsf(x1 - x0);
    if (steep) { float t = x0; x0 = y0; y0 = t; t = x1; x1 = y1; y1 = t; }
    if (x0 > x1) { float t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
    const float grad = x1 > x0 ? (y1 - y0) / (x1 - x0) : 0.f;
    for (int x = (int)floorf(x0); x <= (int)ceilf(x1); x++) {
        float y = y0 + grad * ((float)x + 0.5f - x0) - 0.5f;
        int yi = (int)floorf(y);
        float f = y - (float)yi;
        for (int s2 = 0; s2 < 2; s2++) {
            int px = steep ? yi + s2 : x, py = steep ? x : yi + s2;
            if (px < t->x0 || py < t->y0 || px >= t->x1 || py >= t->y1) continue;
            blend(rect_px(t, px, py), c[0], c[1], c[2], a * (s2 ? f : 1.f - f));
            n++;
        }
    }
    return n;
}

int fluid_draw_overlay(const fluid_draw_t *d, const fluid_rect_t *t, int palette, const fluid_overlay_t *ov) {
    if (!d || !t || !t->px || !ov || !fluid_overlay_on(ov)) return 0;
    const int pal = (palette >= 0 && palette < FLUID_PAL_COUNT) ? palette : FLUID_PAL_PASTEL;
    float cx, cy, sc; ov_geom(d, &cx, &cy, &sc);
    int n = 0;
    if (ov->sectors >= 2 && ov->divider > 0.f) {
        const float R = d->stage * 0.5f, r0 = DIV_FROM * R, r1 = R - DIV_INSET * sc;
        for (int q = 0; q < ov->sectors; q++) {
            float a = 6.2831853f * q / ov->sectors, ux = sinf(a), uy = -cosf(a);
            n += wu_line(t, cx + ux * r0, cy + uy * r0, cx + ux * r1, cy + uy * r1, PAL[pal][1], DIV_ALPHA * ov->divider);
        }
    }
    return n;
}
