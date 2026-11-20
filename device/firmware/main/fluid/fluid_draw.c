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
static inline int64_t now_us(void) { return esp_timer_get_time(); }
#else
#include <time.h>
#define FLUID_CALLOC(n) calloc(1, (n))
#define FLUID_MALLOC(n) malloc(n)
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
    FLUID_FREE(d->m.mem); FLUID_FREE(d->why);
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
    const int W = s->W, H = s->H;
    const int16_t *h = s->h;
    const float rk = DOT_REFR * (1.f / FQ_H);
    *fx = (float)(h[k + 1] - h[k - 1]) * rk * 2.f;
    *fy = (float)(h[k + W] - h[k - W]) * rk * 4.f * H / W;   /* cell aspect A = W/H */
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
    const float steep = DUST_GAIN * (sqrtf(*fx * *fx + *fy * *fy) - DUST_CALM);
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
    if (hd_dust(s, i, j, 0.f) >= 0.5f) return false;
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

/* a cell's stage in a change: the old picture; under the crest (its drawing thinning out); the cloud (black but for the
 * grains); its new picture coming up; that picture, whole. MS_SEND: it goes this frame whatever it shows (a picture
 * coming up that an interrupting change turned back to dust); MS_VALID: the new look is drawn in the kept glass there */
enum { MS_OLD, MS_CREST, MS_CLOUD, MS_FADE, MS_DONE, MS_STAGE = 7, MS_SEND = 0x40, MS_VALID = 0x80 };
static bool hdc_room(fluid_draw_t *d);
static void morph_frame(fluid_draw_t *d, const fluid_frame_t *s, const fluid_draw_opts_t *o);
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

    if (d->m.on) d->m.frame_us = now_us();
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
        if (o->fine) o->fine->pause = d->m.on || (d->sd_kind && d->sd_m > 2);
        if (o->fine) fluid_fine_frame(o->fine, s, o);   /* its fine dust instead of the haze */
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
                d->why[k] |= ff->why[k]; ff->sig0[k] = ff->sig[k]; d->dirty[k] = 1;
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
            d->dirty[k] = (uint8_t)(lt_cell ? 2 : dirty);
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
        if (!d->dirty[k] && !(d->m.on && (d->m.cst[k] & MS_STAGE) != MS_DONE)) d->anim_late |= d->why[k];
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
    d->hdcv[k] = 1;
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

/* ── the whole picture changing: dust on a wave (fluid_morph_t) ─────────────── */

/* each pixel's own number (its grain's chance, the crest's thinning) */
static inline uint32_t morph_hash(int x, int y) {
    uint32_t h = (uint32_t)x * 0x9E3779B1u ^ (uint32_t)y * 0x85EBCA77u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
    return h;
}
/* an rgb565 (big-endian) pixel at lv quarters of its brightness (1..4), by shifts */
static inline uint16_t dim565(uint16_t be, int lv) {
    if (lv >= 4) return be;
    const uint16_t le = (uint16_t)(be >> 8 | be << 8), h = (le >> 1) & 0x7BEFu, q = (le >> 2) & 0x39E7u;
    const uint16_t o = lv <= 1 ? q : lv == 2 ? h : (uint16_t)(h + q);
    return (uint16_t)(o >> 8 | o << 8);
}
/* a + (b - a) s / 256, per channel, rgb565 big-endian */
static inline uint16_t mix565(uint16_t a, uint16_t b, int s) {
    const int A = (uint16_t)(a >> 8 | a << 8), B = (uint16_t)(b >> 8 | b << 8);
    const int r = (A >> 11) + (((B >> 11) - (A >> 11)) * s >> 8), g = (A >> 5 & 63) + (((B >> 5 & 63) - (A >> 5 & 63)) * s >> 8);
    const int bl = (A & 31) + (((B & 31) - (A & 31)) * s >> 8);
    const uint16_t o = (uint16_t)(r << 11 | g << 5 | bl);
    return (uint16_t)(o >> 8 | o << 8);
}
/* the sector of a direction (its diamond angle: no libm call) */
static inline int morph_sector(float dx, float dy) {
    const float s = fabsf(dx) + fabsf(dy);
    if (s < 1e-3f) return 0;
    const float a = dy >= 0.f ? (dx >= 0.f ? dy / s : 1.f - dx / s) : (dx < 0.f ? 2.f - dy / s : 3.f + dx / s);
    return (int)(a * (FLUID_MORPH_SECTORS / 4)) & (FLUID_MORPH_SECTORS - 1);
}
static float s_cs[256][2];                              /* cos, sin of k / 256 of a turn */

static bool morph_mem(fluid_draw_t *d) {
    fluid_morph_t *m = &d->m;
    if (m->mem) return true;
    const size_t N = (size_t)d->W * d->H, K = FLUID_MORPH_SPECKS;
    uint8_t *p = FLUID_CALLOC(sizeof(fluid_speck_t) * K + sizeof(uint32_t) * N + sizeof(int16_t) * 3 * FLUID_MORPH_CALLS +
                              sizeof(uint16_t) * (4 * N + K + (size_t)d->H + 1) + 6 * N + (size_t)d->fb_w + d->fb_h);
    if (!p) return false;
    m->mem = p;
    m->sp = (fluid_speck_t *)p; p += sizeof(fluid_speck_t) * K;
    m->cum = (uint32_t *)p; p += sizeof(uint32_t) * N;
    m->call = (int16_t (*)[3])p; p += sizeof(int16_t) * 3 * FLUID_MORPH_CALLS;
    m->td = (int16_t *)p; m->tr = m->td + N; m->cd = (uint16_t *)(m->tr + N); m->order = m->cd + N;
    m->bidx = m->order + N; m->bstart = m->bidx + K;
    p = (uint8_t *)(m->bstart + d->H + 1);
    m->cst = p; m->olit = p + N; m->nlit = p + 2 * N; m->spn = p + 3 * N; m->spn0 = p + 4 * N;
    m->colx = p + 6 * N; m->rowy = m->colx + d->fb_w;
    memset(m->colx, 255, (size_t)d->fb_w); memset(m->rowy, 255, (size_t)d->fb_h);
    for (int i = 0; i < d->W; i++) for (int x = d->cx[i]; x < d->cx[i + 1]; x++) m->colx[x] = (uint8_t)i;
    for (int j = 0; j < d->H; j++) for (int y = d->cy[j]; y < d->cy[j + 1]; y++) m->rowy[y] = (uint8_t)j;
    for (int k = 0; k < 256; k++) { s_cs[k][0] = cosf(k * 6.2831853f / 256.f); s_cs[k][1] = sinf(k * 6.2831853f / 256.f); }
    return true;
}

static void morph_end(fluid_draw_t *d, bool cut) {
    fluid_morph_t *m = &d->m;
    m->on = false; m->nsp = 0; m->fx = -1.f;
    if (cut) d->force = true;                           /* (the next frame sends every cell: the glass is the look's then) */
    m->whole = (m->look & (FLUID_LOOK_KEEP | FLUID_LOOK_GLASS)) && m->snap;
}

void fluid_draw_morph_focus(fluid_draw_t *d, float x, float y) { if (d) { d->m.fx = x; d->m.fy = y; } }

/* a change from the glass as it is now, centred at the focus (else where the one it interrupts was, else the middle).
 * In the middle of one: what still shows the old picture stays it, the cloud stays the cloud (its grains thrown again
 * by the new crest), a picture coming up goes back to dust, a whole one is the old picture now */
static void morph_start(fluid_draw_t *d, uint32_t look) {
    fluid_morph_t *m = &d->m;
    const int W = d->W, N = W * d->H, f0 = m->frames;
    const float R = d->stage * 0.5f, gx = d->ox + R, gy = d->oy + R;
    const bool was = m->on;
    const float ex = m->fx - gx, ey = m->fy - gy;
    if (m->fx >= 0.f && ex * ex + ey * ey <= R * R) { m->cx = m->fx; m->cy = m->fy; }
    else if (!was) { m->cx = gx; m->cy = gy; }
    m->fx = -1.f;
    m->D = fsqrt((m->cx - gx) * (m->cx - gx) + (m->cy - gy) * (m->cy - gy)) + R + 1.f;
    static uint16_t s_bucket[1025];                     /* (the cells by distance: a count over whole px) */
    memset(s_bucket, 0, sizeof(s_bucket));
    for (int k = 0; k < N; k++) {
        const int i = k % W, j = k / W;
        const float x = (d->cx[i] + d->cx[i + 1]) * 0.5f - m->cx, y = (d->cy[j] + d->cy[j + 1]) * 0.5f - m->cy;
        const int dd = (int)fsqrt(x * x + y * y);
        m->cd[k] = (uint16_t)dd;
        s_bucket[dd > 1023 ? 1023 : dd]++;
        uint8_t st = was ? m->cst[k] & MS_STAGE : MS_OLD, send = 0;
        if (st == MS_OLD || st == MS_DONE) { st = MS_OLD; m->td[k] = INT16_MAX; m->olit[k] = 0; }
        else if (st == MS_CREST) m->td[k] = (int16_t)(m->td[k] - f0 + 1);   /* (its crest goes on as it was: f0 was not drawn) */
        else { send = st == MS_FADE ? MS_SEND : 0; st = MS_CLOUD; m->td[k] = -100; }
        m->cst[k] = st | send; m->tr[k] = -1; m->nlit[k] = 0;
        if (!was) m->spn[k] = 0;
    }
    for (int q = 1, sum = s_bucket[0]; q < 1025; q++) { const int c = s_bucket[q]; s_bucket[q] = (uint16_t)sum; sum += c; }
    s_bucket[0] = 0;
    for (int k = 0; k < N; k++) { const int dd = m->cd[k]; m->order[s_bucket[dd > 1023 ? 1023 : dd]++] = (uint16_t)k; }
    for (int n = 0, sum = 0; n < N; n++) {             /* the glass's px nearest the centre first (a cell whose middle is on it) */
        const int k = m->order[n], i = k % W, j = k / W;
        const float x = (d->cx[i] + d->cx[i + 1]) * 0.5f - gx, y = (d->cy[j] + d->cy[j + 1]) * 0.5f - gy;
        if (x * x + y * y <= R * R) sum += (d->cx[i + 1] - d->cx[i]) * (d->cy[j + 1] - d->cy[j]);
        m->cum[n] = (uint32_t)sum;
    }
    m->rc = m->rg = 0.f; m->ci = m->gi = 0; m->ncall = m->call0 = 0;
    if (!was) m->px = m->wins = m->comp = m->paint_px = m->paint_comp = 0;
    memset(m->hist, 0, sizeof(m->hist));
    /* the grains on the glass stay in the cloud, thrown again from where they are */
    int n = 0;
    for (int q = 0; q < FLUID_MORPH_SECTORS; q++) m->head[q] = -1;
    for (int q = 0; was && q < m->nsp; q++) {
        fluid_speck_t p = m->sp[q];
        if (p.x < 0) continue;
        p.ax = p.x; p.ay = p.y; p.c0 = p.c; p.t0 = 1; p.tx = p.ty = -1;
        p.sec = (uint8_t)morph_sector(p.x + 0.5f - m->cx, p.y + 0.5f - m->cy);
        p.next = m->head[p.sec]; m->head[p.sec] = (int16_t)n;
        m->sp[n++] = p;
    }
    m->nsp = n;
    if (!was) {                                         /* of the old picture's lit pixels (every 8th row), its grains' share */
        int lit = 0;
        for (int y = d->oy + 4; y < d->oy + d->stage; y += 8) {
            const float gdy = y + 0.5f - gy, g2 = R * R - gdy * gdy;
            if (g2 <= 0.f) continue;
            const int g = (int)fsqrt(g2);
            const uint16_t *row = m->snap + (size_t)y * d->fb_w;
            for (int x = (int)gx - g + 1; x < (int)gx + g - 1; x++) lit += row[x] != 0;
        }
        lit *= 8;
        m->take = lit > FLUID_MORPH_TAKE ? (float)FLUID_MORPH_TAKE / lit : 1.f;
    }
    m->on = true; m->look = look; m->whole = false; m->painted = false;
    m->frames = 1; m->fill = 0;
}

void fluid_draw_morph_look(fluid_draw_t *d, uint32_t look, uint16_t *snap, bool ready) {
    if (!d) return;
    fluid_morph_t *m = &d->m;
    if (snap != m->snap) { if (m->on) morph_end(d, true); m->snap = snap; m->whole = false; }
    if (m->on) { m->frames++; m->ready |= ready || m->frames >= FLUID_MORPH_WAIT; }
    if (look == m->look) return;
    /* nothing kept to change from, or into a look the panel's mirror keeps (it writes over the kept glass): at once */
    if (!look || (look & FLUID_LOOK_GLASS) || !snap || !(m->on || m->whole) || !morph_mem(d)) {
        if (m->on) morph_end(d, true);
        else d->force = true;
        m->look = look; m->whole = (look & (FLUID_LOOK_KEEP | FLUID_LOOK_GLASS)) && snap;
        return;
    }
    morph_start(d, look);
    m->ready = ready;
}

static bool hdc_room(fluid_draw_t *d) {
    if (d->hdc) return true;
    /* (not zeroed: 434 KB of PSRAM is ~25 ms on the device, and a cell of it is drawn before it is read: hdcv) */
    d->hdc = FLUID_MALLOC(sizeof(uint16_t) * (size_t)d->fb_w * d->fb_h);
    d->hdcv = FLUID_CALLOC((size_t)d->W * d->H);
    if (!d->hdc || !d->hdcv) { FLUID_FREE(d->hdc); FLUID_FREE(d->hdcv); d->hdc = NULL; d->hdcv = NULL; }
    return d->hdc != NULL;
}

#define FLUID_MORPH_CELLS_US 9000       /* the new drawing's cache made ahead, nearest first: until a frame's cells have
                                         * taken this in all (a cell drawn before it is cached is cached then) */

/* The new drawing into its cache, the cells nearest the centre first */
static void morph_cache(fluid_draw_t *d, const fluid_frame_t *s, const fluid_draw_opts_t *o) {
    fluid_morph_t *m = &d->m;
    const int N = d->W * d->H;
    const int64_t until = m->frame_us + FLUID_MORPH_CELLS_US;
    int ncache = o->nshapes < FLUID_MAX_SHAPES ? o->nshapes : FLUID_MAX_SHAPES;
    while (ncache > 0 && o->shapes[ncache - 1].tag == FLUID_TAG_CAPTION) ncache--;
    for (int n = 0; m->fill < N && ((n & 3) || now_us() < until); m->fill++) {
        const int k = m->order[m->fill];
        if (!d->hdcv[k] && s->mask[k]) n += hdc_cell(d, o, k, ncache) ? 2 : 1;
    }
}

/* a grain thrown up by the crest from (x, y), colour c: to a pixel of the new picture that waits for one, once thrown
 * up a little, else into the cloud */
static void morph_grain(fluid_morph_t *m, int x, int y, uint16_t c, uint32_t h) {
    if (m->nsp >= FLUID_MORPH_SPECKS) return;
    const int sec = morph_sector(x + 0.5f - m->cx, y + 0.5f - m->cy), n = m->nsp++;
    fluid_speck_t *p = &m->sp[n];
    *p = (fluid_speck_t){ (int16_t)x, (int16_t)y, (int16_t)m->frames, -1, -1, 0, 0, -1, -1, c, 0, c, (uint8_t)(h >> 24), (uint8_t)sec, -1 };
    if (m->call0 < m->ncall) {
        const int16_t *w = m->call[m->call0++];
        p->tx = w[0]; p->ty = w[1]; p->c1 = (uint16_t)w[2];
        p->g0 = (int16_t)(m->frames + FLUID_MORPH_LIFT / 3); p->g1 = (int16_t)(p->g0 + FLUID_MORPH_GATHER);
    } else { p->next = m->head[sec]; m->head[sec] = (int16_t)n; }
}

/* the crest reaches cell k: its old picture's lit pixels counted, some of them grains (one at least where it has any,
 * so the cloud is where the picture was) */
static void morph_throw(fluid_draw_t *d, int k) {
    fluid_morph_t *m = &d->m;
    const int i = k % d->W, j = k / d->W, fw = d->fb_w;
    const float R = d->stage * 0.5f, gx = d->ox + R, gy = d->oy + R, R2 = (R - 1.f) * (R - 1.f);
    const uint32_t take = (uint32_t)(m->take * 65535.f);
    int lit = 0, thrown = 0, bx = -1, by = 0;
    uint32_t best = UINT32_MAX;
    for (int y = d->cy[j]; y < d->cy[j + 1]; y++) {
        const uint16_t *row = m->snap + (size_t)y * fw;
        const float gdy = y + 0.5f - gy;
        for (int x = d->cx[i]; x < d->cx[i + 1]; x++) {
            if (!row[x]) continue;
            lit++;
            const float gdx = x + 0.5f - gx;
            if (gdx * gdx + gdy * gdy > R2) continue;  /* (grains on the glass) */
            const uint32_t h = morph_hash(x, y);
            if ((h & 0xFFFFu) < take) { morph_grain(m, x, y, row[x], h); thrown++; }
            else if (h < best) { best = h; bx = x; by = y; }
        }
    }
    if (!thrown && bx >= 0) morph_grain(m, bx, by, m->snap[(size_t)by * fw + bx], best);
    m->olit[k] = (uint8_t)(lit > 255 ? 255 : lit);
}

/* a grain of the cloud flies to the new picture's pixel (x, y) of colour c, landing at frame g1: one of the sector's that
 * has no pixel yet (the nearest sector's else), or the next one the crest throws up */
static void morph_land(fluid_draw_t *d, int x, int y, uint16_t c, int g1) {
    fluid_morph_t *m = &d->m;
    const int sec = morph_sector(x + 0.5f - m->cx, y + 0.5f - m->cy);
    int q = -1;
    for (int r = 0; r <= FLUID_MORPH_SECTORS / 2 && q < 0; r++) {
        const int a = (sec + r) & (FLUID_MORPH_SECTORS - 1), b = (sec - r) & (FLUID_MORPH_SECTORS - 1);
        const int s = m->head[a] >= 0 ? a : m->head[b] >= 0 ? b : -1;
        if (s >= 0) { q = m->head[s]; m->head[s] = m->sp[q].next; }
    }
    if (q < 0) {
        if (m->ncall < FLUID_MORPH_CALLS) { m->call[m->ncall][0] = (int16_t)x; m->call[m->ncall][1] = (int16_t)y; m->call[m->ncall++][2] = (int16_t)c; }
        return;
    }
    fluid_speck_t *p = &m->sp[q];
    p->tx = (int16_t)x; p->ty = (int16_t)y; p->c1 = c;
    p->g0 = (int16_t)(g1 - FLUID_MORPH_GATHER > m->frames ? g1 - FLUID_MORPH_GATHER : m->frames);
    p->g1 = (int16_t)(g1 > p->g0 ? g1 : p->g0 + 1);
}

/* The new look drawn into the kept glass over cells [i0, i1) of row j. Returns its cost on the panel model (us): its
 * pixels 0.05 us, those drawn one by one (not black, not the drawing's cache copied) 0.9 */
static int morph_paint_run(fluid_draw_t *d, const fluid_draw_opts_t *o, int j, int i0, int i1) {
    fluid_morph_t *m = &d->m;
    const int fw = d->fb_w, x0 = d->cx[i0], x1 = d->cx[i1], y0 = d->cy[j], y1 = d->cy[j + 1];
    const fluid_rect_t t = { m->snap + (size_t)y0 * fw + x0, fw, x0, y0, x1, y1 };
    fluid_draw_rect(d, o, &t, true);
    m->painted = true;
    int comp = 0;
    for (int i = i0; i < i1; i++) {
        const int k = j * d->W + i;
        const bool lines = m->lines && d->hdcv[k];
        int lit = 0;
        for (int y = y0; y < y1; y++) {
            const uint16_t *row = m->snap + (size_t)y * fw, *hc = lines ? d->hdc + (size_t)y * fw : NULL;
            for (int x = d->cx[i]; x < d->cx[i + 1]; x++) if (row[x]) { lit++; comp += !hc || hc[x] != row[x]; }
        }
        m->nlit[k] = (uint8_t)(lit > 255 ? 255 : lit);
        m->cst[k] |= MS_VALID;
    }
    const int px = (x1 - x0) * (y1 - y0);
    m->paint_px += px; m->paint_comp += comp;
    return (px + 18 * comp) / 20;
}

/* cell k's new picture calls the cloud: some of its pixels a grain each, landing in FLUID_MORPH_GATHER frames, when it
 * comes up at the soonest */
static void morph_call(fluid_draw_t *d, int k) {
    fluid_morph_t *m = &d->m;
    const int i = k % d->W, j = k / d->W, fw = d->fb_w, g1 = m->frames + FLUID_MORPH_GATHER;
    const float R = d->stage * 0.5f, gx = d->ox + R, gy = d->oy + R, R2 = (R - 1.f) * (R - 1.f);
    const uint32_t take = (uint32_t)(m->take * 65535.f);
    const bool lines = m->lines && d->hdcv[k];
    for (int y = d->cy[j]; y < d->cy[j + 1]; y++) {
        const uint16_t *row = m->snap + (size_t)y * fw, *hc = lines ? d->hdc + (size_t)y * fw : NULL;
        const float gdy = y + 0.5f - gy;
        for (int x = d->cx[i]; x < d->cx[i + 1]; x++) {
            const float gdx = x + 0.5f - gx;
            if (!row[x] || (morph_hash(x, y) >> 16) >= take || gdx * gdx + gdy * gdy > R2) continue;
            morph_land(d, x, y, hc && hc[x] == row[x] ? dim565(row[x], 2) : row[x], g1);   /* (on a line, a grain is dust: dim) */
        }
    }
    m->tr[k] = (int16_t)g1;
}

/* The new look into the kept glass once its inputs are whole: first where it must show now (coming up, whole, or about
 * to), then in the cloud, nearest the centre first, FLUID_MORPH_PAINT_US a frame on the panel model */
static void morph_paint(fluid_draw_t *d, const fluid_draw_opts_t *o) {
    fluid_morph_t *m = &d->m;
    if (!m->ready || !m->snap) return;
    const int W = d->W, N = W * d->H, f = m->frames;
    int budget = FLUID_MORPH_PAINT_US;
#define WANT(k, pass) (!(m->cst[k] & MS_VALID) && ((pass) ? (m->cst[k] & MS_STAGE) == MS_CLOUD \
                       : (m->cst[k] & MS_STAGE) >= MS_FADE || ((m->cst[k] & MS_STAGE) == MS_CLOUD && m->tr[k] >= 0 && f >= m->tr[k] - 1)))
    for (int pass = 0; pass < 2; pass++)
        for (int n = 0; n < N && (!pass || budget > 0); n++) {
            const int k = m->order[n];
            if (!WANT(k, pass)) continue;
            int i0 = k % W, i1 = i0 + 1;
            while (i0 > 0 && WANT(k - (k % W - i0) - 1, pass)) i0--;
            while (i1 < W && WANT(k - k % W + i1, pass)) i1++;
            budget -= morph_paint_run(d, o, k / W, i0, i1);
        }
#undef WANT
}

/* Each grain this frame: thrown out from the centre by the crest and scattered round that, turning and drifting on;
 * once it has a pixel, flying to it and landing there as the cell's picture starts coming up. Then the grains by cell
 * row, and how many each cell shows (only the dust's cells show any: the crest's, the cloud's, those coming up) */
static void morph_specks(fluid_draw_t *d) {
    fluid_morph_t *m = &d->m;
    const int W = d->W, H = d->H, N = W * H, f = m->frames;
    const float R = d->stage * 0.5f, gx = d->ox + R, gy = d->oy + R, Rin = R - 2.5f;
    memcpy(m->spn0, m->spn, (size_t)N); memset(m->spn, 0, (size_t)N);
    memset(m->bstart, 0, sizeof(uint16_t) * (size_t)(H + 1));
    for (int q = 0; q < m->nsp; q++) {
        fluid_speck_t *p = &m->sp[q];
        const int u = f - p->t0;
        float x = p->ax, y = p->ay;
        if (u > 0) {
            const float l = (float)u / FLUID_MORPH_LIFT, e = l >= 1.f ? 1.f : 1.f - (1.f - l) * (1.f - l);
            float nx = p->ax + 0.5f - m->cx, ny = p->ay + 0.5f - m->cy;
            const float len = fsqrt(nx * nx + ny * ny);
            if (len > 0.5f) { nx /= len; ny /= len; } else { nx = s_cs[p->h][0]; ny = s_cs[p->h][1]; }
            const float push = FLUID_MORPH_PUSH * (1.f + (p->h & 127) * (0.5f / 127.f)) * e + (u > FLUID_MORPH_LIFT ? fminf((float)(u - FLUID_MORPH_LIFT), 30.f) * 0.8f : 0.f);
            const int a = (p->h * 7 + (p->h & 1 ? 2 * u : -2 * u)) & 255;
            const float sc = FLUID_MORPH_SCATTER * (0.4f + (p->h >> 4) * (0.6f / 15.f)) * e;
            x += nx * push + s_cs[a][0] * sc; y += ny * push + s_cs[a][1] * sc;
        }
        uint16_t c = p->c0;
        if (p->tx >= 0 && f > p->g0) {
            const float v = f >= p->g1 ? 1.f : (float)(f - p->g0) / (float)(p->g1 - p->g0), s = v * v * (3.f - 2.f * v);
            x += (p->tx - x) * s; y += (p->ty - y) * s;
            c = mix565(p->c0, p->c1, (int)(s * 256.f));
        }
        const float dx = x + 0.5f - gx, dy = y + 0.5f - gy, r2 = dx * dx + dy * dy;
        if (r2 > Rin * Rin) { const float k = Rin / fsqrt(r2); x = gx + dx * k - 0.5f; y = gy + dy * k - 0.5f; }
        p->c = c; p->x = -1;
        const int ix = ifloor(x + 0.5f), iy = ifloor(y + 0.5f), big = p->tx < 0 || f < p->g1 ? 2 : 1;
        if (ix < 0 || iy < 0 || ix + 1 >= d->fb_w || iy + 1 >= d->fb_h) continue;
        bool shown = false;
        for (int r = 0; r < big * big; r++) {
            const int ci = m->colx[ix + (r & 1)], cj = m->rowy[iy + (r >> 1)];
            if (ci == 255 || cj == 255) continue;
            const int k = cj * W + ci, st = m->cst[k] & MS_STAGE;
            if (st == MS_OLD || st == MS_DONE) continue;
            shown = true;
            if (m->spn[k] < 255) m->spn[k]++;
        }
        if (!shown || m->rowy[iy] == 255) continue;
        p->x = (int16_t)ix; p->y = (int16_t)iy;
        m->bstart[m->rowy[iy] + 1]++;
    }
    for (int j = 0; j < H; j++) m->bstart[j + 1] += m->bstart[j];
    static uint16_t s_at[256];                          /* (each row's next place) */
    memcpy(s_at, m->bstart, sizeof(uint16_t) * (size_t)(H < 255 ? H : 255));
    for (int q = 0; q < m->nsp; q++) if (m->sp[q].x >= 0) m->bidx[s_at[m->rowy[m->sp[q].y]]++] = (uint16_t)q;
}

/* This frame of the change, after the cells (fluid_draw_cells): the look's own changes, the stages, the new picture into
 * the kept glass, the grains, and which cells are sent (any whose pixels change: d->dirty, d->span) */
static void morph_frame(fluid_draw_t *d, const fluid_frame_t *s, const fluid_draw_opts_t *o) {
    fluid_morph_t *m = &d->m;
    const int W = d->W, H = d->H, N = W * H, f = m->frames;
    const bool hd = o->scene && o->dots && o->shapes;
    /* the last frame's on the panel model (us): its windows, its pixels, those worked out one by one (the cells' part,
     * ~11 ms on the device, is the rest of the frame) */
    const int load = m->wins * 150 + (m->px + m->paint_px) / 20 + (m->comp + m->paint_comp) * 9 / 10;
    m->comp = m->paint_px = m->paint_comp = 0;
    m->lines = hd && o->fine && d->hdc && d->hdcv;
    /* the look's own changes: where its picture is whole they go as ever (drawn into the kept glass first); elsewhere
     * what is drawn of it is stale */
    for (int k = 0; k < N; k++) {
        if (!d->dirty[k]) continue;
        m->cst[k] &= (uint8_t)~MS_VALID;
        if ((m->cst[k] & MS_STAGE) != MS_DONE) d->dirty[k] = 0;
    }
    if (hd && o->fine && hdc_room(d)) morph_cache(d, s, o);
    morph_paint(d, o);
    {   /* the fronts, over the cells nearest the centre first. The crest: FLUID_MORPH_SWEEP px of glass and as many of the
         * old picture's lit pixels as FLUID_MORPH_LIT a frame, while the cloud behind it holds less than FLUID_MORPH_BAND
         * and the change's last frame took the panel less than FLUID_MORPH_LOAD_US. The gathering, once the new look is
         * being drawn: as much glass, as many of its lit pixels (a third while the last frame was heavy), never nearer the crest than where that was
         * CREST + CLOUD frames ago */
        enum { LAG = FLUID_MORPH_CREST + FLUID_MORPH_CLOUD };
        const uint32_t ag = m->gi ? m->cum[m->gi - 1] : 0;
        const uint32_t a0 = m->ci ? m->cum[m->ci - 1] : 0;
        for (int lit = 0; load <= FLUID_MORPH_LOAD_US && m->ci < N && m->cum[m->ci] <= a0 + FLUID_MORPH_SWEEP && m->cum[m->ci] <= ag + FLUID_MORPH_BAND &&
                          lit <= FLUID_MORPH_LIT; m->ci++) {
            const int k = m->order[m->ci];
            if ((m->cst[k] & MS_STAGE) == MS_OLD) { m->td[k] = (int16_t)f; morph_throw(d, k); lit += m->olit[k]; }
        }
        m->rc = m->ci < N ? (m->ci ? m->cd[m->order[m->ci - 1]] : 0.f) : 1e4f;
        m->hist[f & 31] = m->rc;
        if (m->ready && f > LAG) {
            const float lim = m->hist[(f - LAG) & 31];
            const int most = load <= FLUID_MORPH_LOAD_US ? FLUID_MORPH_LIT : FLUID_MORPH_LIT / 3;   /* (slower while it is heavy) */
            for (int lit = 0; m->gi < N && m->cd[m->order[m->gi]] <= lim && m->cum[m->gi] <= ag + FLUID_MORPH_SWEEP && lit <= most; m->gi++)
                lit += m->nlit[m->order[m->gi]];
            m->rg = m->gi < N ? (m->gi ? m->cd[m->order[m->gi - 1]] : 0.f) : 1e4f;
        }
    }
    bool busy = false;
    for (int k = 0; k < N; k++) {
        const uint8_t c = m->cst[k];
        uint8_t st = c & MS_STAGE;
        if (st == MS_OLD && m->td[k] <= f) st = MS_CREST;
        if (st == MS_CREST && f >= m->td[k] + FLUID_MORPH_CREST) st = MS_CLOUD;
        if (st == MS_CLOUD && (c & MS_VALID) && m->tr[k] < 0 && f >= m->td[k] + FLUID_MORPH_CREST + FLUID_MORPH_CLOUD - FLUID_MORPH_GATHER) morph_call(d, k);
        if (st == MS_CLOUD && (c & MS_VALID) && m->tr[k] >= 0 && f >= m->tr[k] && m->rg >= m->cd[k] && f >= m->td[k] + FLUID_MORPH_CREST + FLUID_MORPH_CLOUD) {
            st = MS_FADE; m->tr[k] = (int16_t)f;
        }
        if (st == MS_FADE && f >= m->tr[k] + FLUID_MORPH_FADE) st = MS_DONE;
        m->cst[k] = (uint8_t)((c & ~MS_STAGE) | st);
        busy |= st != MS_DONE;
    }
    morph_specks(d);
    /* sent: a cell whose grains come or go, under the crest while it has old pixels (and the frame its last go), coming up
     * while it has new ones; the cells whole and changed (above) */
    int any = 0;
    for (int k = 0; k < N; k++) {
        const uint8_t c = m->cst[k], st = c & MS_STAGE;
        const bool send = m->spn[k] || m->spn0[k] || (c & MS_SEND) || (st == MS_CREST && m->olit[k]) ||
                          (st == MS_CLOUD && f == m->td[k] + FLUID_MORPH_CREST && m->olit[k]) || (st == MS_FADE && m->nlit[k]);
        m->cst[k] = c & (uint8_t)~MS_SEND;
        if (send) { d->dirty[k] = 1; any++; }
    }
    for (int j = 0; j < H; j++) {
        int i0 = W, i1 = 0;
        for (int i = 0; i < W; i++) if (d->dirty[j * W + i]) { if (i < i0) i0 = i; i1 = i + 1; }
        d->span[2 * j] = (int16_t)(i0 < i1 ? i0 : 0); d->span[2 * j + 1] = (int16_t)(i0 < i1 ? i1 : 0);
    }
    if (!busy && !any) morph_end(d, false);             /* every cell its new picture, whole, and the cloud gone */
}

int fluid_draw_morph_rect(const fluid_draw_t *d, const fluid_rect_t *t) {
    const fluid_morph_t *m = &d->m;
    const int W = d->W, fw = d->fb_w, f = m->frames, tw = t->x1 - t->x0;
    int comp = 0;
    for (int y = t->y0; y < t->y1; y++) {
        uint16_t *row = rect_px(t, t->x0, y);
        const int j = m->rowy[y], a = t->x0, b = t->x1;
        if (j == 255) { memset(row, 0, sizeof(uint16_t) * (size_t)tw); continue; }
        const uint16_t *src = m->snap + (size_t)y * fw;
        for (int x = a; x < b;) {
            const int i = m->colx[x];
            if (i == 255) { row[x - t->x0] = 0; x++; continue; }
            const int k = j * W + i, xe = d->cx[i + 1] < b ? d->cx[i + 1] : b, n = xe - x, st = m->cst[k] & MS_STAGE;
            uint16_t *o = row + (x - t->x0);
            if (st == MS_OLD || st == MS_DONE) memcpy(o, src + x, sizeof(uint16_t) * (size_t)n);
            else if (st == MS_CLOUD) memset(o, 0, sizeof(uint16_t) * (size_t)n);
            else if (st == MS_CREST) {                  /* the old drawing thinning out: 2/3, 1/3 of its pixels */
                const int keep = (FLUID_MORPH_CREST - (f - m->td[k])) * 256 / (FLUID_MORPH_CREST + 1);
                for (int q = 0; q < n; q++) {
                    const uint16_t p = src[x + q];
                    o[q] = p && (int)(morph_hash(x + q, y) & 255u) < keep ? p : 0;
                    comp += p != 0;
                }
            } else if (f - m->tr[k] >= FLUID_MORPH_FADE - 1) memcpy(o, src + x, sizeof(uint16_t) * (size_t)n);
            else {                                      /* coming up: its dust at half, then whole as its lines are at half */
                const int lv = f - m->tr[k];
                const uint16_t *hc = m->lines && d->hdcv[k] ? d->hdc + (size_t)y * fw : NULL;
                for (int q = 0; q < n; q++) {
                    const uint16_t p = src[x + q];
                    if (!p) { o[q] = 0; continue; }
                    o[q] = hc && hc[x + q] == p ? (lv ? dim565(p, 2) : 0) : lv ? p : dim565(p, 2);
                    comp++;
                }
            }
            x = xe;
        }
    }
    /* the grains over it (a flying one 2 x 2 px, a landed one its pixel), on the dust's cells only */
    int ja = -1, jb = -1;
    for (int y = t->y0 > 0 ? t->y0 - 1 : 0; y < t->y1; y++) if (m->rowy[y] != 255) { if (ja < 0) ja = m->rowy[y]; jb = m->rowy[y]; }
    for (int j = ja; ja >= 0 && j <= jb; j++)
        for (int q = m->bstart[j]; q < m->bstart[j + 1]; q++) {
            const fluid_speck_t *p = &m->sp[m->bidx[q]];
            const int big = p->tx < 0 || f < p->g1 ? 2 : 1;
            if (p->x + big <= t->x0 || p->x >= t->x1 || p->y + big <= t->y0 || p->y >= t->y1) continue;
            comp++;
            for (int r = 0; r < big * big; r++) {
                const int X = p->x + (r & 1), Y = p->y + (r >> 1);
                if (X < t->x0 || X >= t->x1 || Y < t->y0 || Y >= t->y1 || m->colx[X] == 255 || m->rowy[Y] == 255) continue;
                const int k = m->rowy[Y] * W + m->colx[X], st = m->cst[k] & MS_STAGE;
                if (st == MS_OLD || st == MS_DONE) continue;
                uint16_t c = p->c;
                if (p->tx < 0 && st == MS_FADE) {       /* (a grain with no pixel: it goes as the picture comes up) */
                    const int lv = f - m->tr[k];
                    if (lv >= 2) continue;
                    c = dim565(c, 2 - lv);
                }
                *rect_px(t, X, Y) = c;
            }
        }
    return comp;
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

void fluid_draw_rect(const fluid_draw_t *d, const fluid_draw_opts_t *o, const fluid_rect_t *t, bool all) {
    const int W = d->W, H = d->H, bx = t->x0;
    const bool hd = o->scene && o->dots && o->shapes;
    uint32_t c1 = fluid_cycles();
    /* a scene on fine dust: its drawing from the cache (the caption's shapes, at the end, live) */
    fluid_draw_t *dm = (fluid_draw_t *)d;
    if (hd && o->fine) hdc_room(dm);
    const bool cached = hd && o->fine && d->hdc;
    const bool sd3 = hd && o->fine && d->sd_kind == FINE_DUST_FINE_DOTS && d->sd_hard;
    int ncache = o->nshapes < FLUID_MAX_SHAPES ? o->nshapes : FLUID_MAX_SHAPES;
    while (cached && ncache > 0 && o->shapes[ncache - 1].tag == FLUID_TAG_CAPTION) ncache--;
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
            for (int i = i0; i < i1 && !all; i++) {     /* the changed cells, cleared */
                if (!d->dirty[j * W + i]) continue;
                const int x0 = d->cx[i] > t->x0 ? d->cx[i] : t->x0, x1 = d->cx[i + 1] < t->x1 ? d->cx[i + 1] : t->x1;
                for (int x = x0; x < x1; x++) row[x - bx] = 0;
            }
            if (ha < hb && cached) {
                for (int i = i0; i < i1; i++) {
                    const int k = j * W + i, x0 = d->cx[i] > t->x0 ? d->cx[i] : t->x0, x1 = d->cx[i + 1] < t->x1 ? d->cx[i + 1] : t->x1;
                    if (x1 <= x0 || !d->hdw[x0]) continue;
                    if (!d->hdcv[k]) hdc_cell(dm, o, k, ncache);
                    hdc_copy(d, o, row, bx, y, x0, x1);
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
                    if (r < 0) continue;
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
    if (hd && o->fine) fluid_fine_rect(o->fine, d->dirty, t, all);   /* the fine dust over it all, never on the drawing */
    if (sd3) sd_hard_rect(d, t, bx, all);               /* (EXPERIMENT kind 3) the hard dots over that, on black only */
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

/* Crisp dots' changed cells to the panel, in few windows: a window costs about as much as CRISP_WIN_PX pixels (its
 * commands and its wait against a pixel's ~0.1-0.2 us on the CO5300's quad SPI, measured; a scene on fine dust draws a
 * pixel slower: WAVE_WIN_PX), so each row sends its whole span
 * of changed cells, and the next rows join it while one box over them costs less than separate windows (and fits the
 * buffer). Windows start and end on even rows: a panel that takes whole row pairs only (the CO5300) otherwise resends
 * a full-width pair at each odd edge. */
#define CRISP_WIN_PX 1500
#define WAVE_WIN_PX  354                                /* (a scene on fine dust) */
#define MORPH_WIN_PX 3000                               /* (a change: its pixels mostly copied, 0.05 us against a window's 0.15 ms) */
/* the windows' buffers: with two, each window is drawn into the one the panel is not reading (the send may return
 * while the panel still reads the other: fluid_draw_send2) */
typedef struct { uint16_t *buf[2]; int max_px, cur; fluid_send_fn send; void *ctx; } sender_t;
static int s_wins;                                      /* windows sent (a change's counted for the log) */
static int window(const fluid_draw_t *d, const fluid_draw_opts_t *o, const fluid_overlay_t *ov, int mark, sender_t *sd,
                  int x0, int y, int x1, int h) {
    uint16_t *buf = sd->buf[sd->cur];
    const int w = x1 - x0;
    const fluid_rect_t t = { buf, w, x0, y, x1, y + h };
    const int64_t t0 = now_us();
    const fluid_morph_t *m = &d->m;
    if (m->on) ((fluid_morph_t *)m)->comp += fluid_draw_morph_rect(d, &t);   /* the picture changing: as the change shows it */
    else {
        fluid_draw_rect(d, o, &t, true);
        if (m->snap && (m->look & FLUID_LOOK_KEEP))     /* a kept look: the glass as it is sent */
            for (int yy = y; yy < y + h; yy++) memcpy(m->snap + (size_t)yy * d->fb_w + x0, buf + (size_t)(yy - y) * w, sizeof(uint16_t) * (size_t)w);
    }
    fluid_draw_overlay(d, &t, o->palette, ov);
    fluid_draw_mark(d, &t, o->palette, mark);
    const int64_t t1 = now_us();
    if (sd->send(sd->ctx, x0, y, w, h, buf) != 0) return -1;
    if (m->on) { fluid_morph_t *mm = (fluid_morph_t *)m; mm->cmp_us += (int)(t1 - t0); mm->push_us += (int)(now_us() - t1); }
    s_wins++;
    if (sd->buf[1]) sd->cur ^= 1;
    return w * h;
}
static int crisp_box(const fluid_draw_t *d, const fluid_draw_opts_t *o, const fluid_overlay_t *ov, int mark, int x0, int x1,
                     int y0, int y1, sender_t *sd) {
    const int w = x1 - x0, rows = (sd->max_px / w) & ~1;
    int sent = 0;
    if (rows < 2) return -1;
    for (int y = y0; y < y1; y += rows) {
        const int n = window(d, o, ov, mark, sd, x0, y, x1, y1 - y < rows ? y1 - y : rows);
        if (n < 0) return -1;
        sent += n;
    }
    return sent;
}
static int crisp_send(const fluid_draw_t *d, const fluid_draw_opts_t *o, const fluid_overlay_t *ov, int mark, sender_t *sd, int win_px) {
    const int W = d->W, H = d->H;
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
    for (int j = 0; j <= H; j++) {
        int nn = 0;
        if (j < H && d->span[2 * j] < d->span[2 * j + 1]) {
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
            if (box[q].j1 == j + 1 && j < H) { box[keep++] = box[q]; continue; }
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

int fluid_draw_send(const fluid_draw_t *d, const fluid_draw_opts_t *o, const fluid_overlay_t *ov, int mark, bool full,
                    uint16_t *buf, int max_px, fluid_send_fn send, void *ctx) {
    return fluid_draw_send2(d, o, ov, mark, full, buf, NULL, max_px, send, ctx);
}

int fluid_draw_send2(const fluid_draw_t *d, const fluid_draw_opts_t *o, const fluid_overlay_t *ov, int mark, bool full,
                     uint16_t *buf, uint16_t *buf2, int max_px, fluid_send_fn send, void *ctx) {
    sender_t sd = { { buf, buf2 }, max_px, 0, send, ctx };
    int sent = 0;
    /* a scene in high definition on the dots: each run of changed cells its own window, so its haze changing all over
     * does not redraw the shapes between */
    const bool runs = o->scene && o->dots && o->shapes;
    fluid_morph_t *m = (fluid_morph_t *)&d->m;
    if (m->on && full) morph_end((fluid_draw_t *)d, false);   /* (all of the look sent at once: the change is over) */
    const int n0 = s_wins;
    m->cmp_us = m->push_us = 0;
    /* crisp dots alone (a rigged creature), and a scene on fine dust: few, larger windows on whole row pairs (crisp_send:
     * a panel that takes whole pairs only, the CO5300, otherwise resends a full-width pair at each odd edge) */
    if (o->scene && o->dots && (!o->shapes || o->fine) && !full) {
        const int n = crisp_send(d, o, ov, mark, &sd, m->on ? MORPH_WIN_PX : o->shapes ? (d->sd_kind ? SD_WIN_PX : WAVE_WIN_PX) : CRISP_WIN_PX);
        if (m->on) { m->px = n; m->wins = s_wins - n0; }
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
