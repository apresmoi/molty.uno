/*
 * fluid_draw.c — draw the fluid as braille dots / ASCII glyphs into the framebuffer.
 *
 * Target board: Waveshare ESP32-S3-Touch-LCD-1.46 (412x412 round LCD)
 * FQBN: esp32:esp32:esp32s3:FlashSize=16M,PSRAM=enabled,CDCOnBoot=cdc
 *
 * Port of creatures/js/fluid/render.js. Braille is drawn as our own
 * 2×4 dot rectangles (no font): sub-samples are bilinear reads of the view field at
 * the 8 dot centres, then either
 *   contour — the cell straddles an edge: dots on the dense side of its own mid-level
 *   tonal   — ordered dither on the GLOBAL 4×4 Bayer lattice of sub-dots (+ a per-dot
 *             jitter = the living shimmer of the dim field) → 0…8 dots
 * edge_mode "off" falls back to glyph-ramp ASCII via the 5×7 device font, with
 * streamline strokes (- \ | /) where the ink moves fast.
 *
 * Dirty cells only: each cell remembers (kind|dots, rgb565) and is cleared + redrawn
 * only when that changes; the caller flushes the returned row band. Cells under a
 * condensed speed-reader letter are the word's dark box: cleared every frame, then
 * the letters are drawn on top.
 */
#include "fluid_sim.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define FLUID_CALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM)
#define FLUID_FREE(p)   heap_caps_free(p)
#else
#define FLUID_CALLOC(n) calloc(1, (n))
#define FLUID_FREE(p)   free(p)
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
#define DOT_H         0.15f

#define KEY_EMPTY 0x0000u
#define KEY_GLYPH 0x0100u     /* | char                                                   */
#define KEY_SKIP  0xFFFFu

const char *const FLUID_PAL_NAMES[FLUID_PAL_COUNT]   = { "green", "amber", "pastel" };
const char *const FLUID_EDGE_NAMES[FLUID_EDGE_COUNT] = { "off", "contours", "all" };

static const float PAL[FLUID_PAL_COUNT][3][3] = {     /* lo, hi, acc (0..255) */
    { { 10, 60, 28 },  { 150, 255, 175 }, { 255, 214, 90 } },
    { { 70, 30, 0 },   { 255, 190, 90 },  { 255, 255, 245 } },
    { { 0, 0, 0 },     { 255, 255, 255 }, { 255, 140, 60 } },
};

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

fluid_draw_t *fluid_draw_create(int W, int H) {
    if (!s_glut_ok) {
        for (int i = 0; i <= 64; i++) s_glut[i] = DITHER_GAIN * powf(i / 64.f, DITHER_GAMMA);
        s_glut_ok = true;
    }
    fluid_draw_t *d = FLUID_CALLOC(sizeof(fluid_draw_t));
    if (!d) return NULL;
    size_t N = (size_t)W * H;
    size_t bytes = (size_t)(W + 1 + H + 1 + 2 * W + 4 * H) * sizeof(int16_t) + 2 * N * sizeof(uint16_t) + N;
    d->block = FLUID_CALLOC(bytes);
    if (!d->block) { FLUID_FREE(d); return NULL; }
    int16_t *p16 = (int16_t *)d->block;
    d->cx = p16; d->cy = d->cx + W + 1; d->dx = d->cy + H + 1; d->dy = d->dx + 2 * W;
    d->key = (uint16_t *)(d->dy + 4 * H); d->col = d->key + N;
    d->skip = (uint8_t *)(d->col + N);
    d->W = W; d->H = H;
    const float cw = (float)FLUID_PANEL / W, ch = (float)FLUID_PANEL / H;
    float dw = cw * DOT_W, dh = ch * DOT_H;
    d->dot_w = (int)lrintf(dw < 1.5f ? 1.5f : dw); if (d->dot_w < 1) d->dot_w = 1;
    d->dot_h = (int)lrintf(dh < 1.5f ? 1.5f : dh); if (d->dot_h < 1) d->dot_h = 1;
    for (int i = 0; i <= W; i++) d->cx[i] = (int16_t)lrintf(i * cw);
    for (int j = 0; j <= H; j++) d->cy[j] = (int16_t)lrintf(j * ch);
    for (int i = 0; i < W; i++) for (int c = 0; c < 2; c++)
        d->dx[i * 2 + c] = (int16_t)lrintf(i * cw + (c ? 0.7f : 0.3f) * cw - d->dot_w * 0.5f);
    for (int j = 0; j < H; j++) for (int r = 0; r < 4; r++)
        d->dy[j * 4 + r] = (int16_t)lrintf(j * ch + (0.14f + 0.24f * r) * ch - d->dot_h * 0.5f);
    memset(d->yrow, -1, sizeof(d->yrow));
    for (int j = 0; j < H; j++) for (int r = 0; r < 4; r++)
        for (int y = d->dy[j * 4 + r]; y < d->dy[j * 4 + r] + d->dot_h; y++)
            if (y >= d->cy[j] && y < d->cy[j + 1]) d->yrow[y] = (int8_t)r;
    d->force = true;
    return d;
}

void fluid_draw_destroy(fluid_draw_t *d) {
    if (!d) return;
    FLUID_FREE(d->block);
    FLUID_FREE(d);
}

/* ── primitives ──────────────────────────────────────────────────────────── */

/* 5×7 glyph scaled (nearest) into the box (x0,y0,w,h), clipped to [cx0,cx1)×[cy0,cy1) */
static void draw_glyph(uint16_t *fb, const uint8_t *gl, int x0, int y0, int w, int h, uint16_t c,
                       int cx0, int cy0, int cx1, int cy1) {
    if (!gl || w <= 0 || h <= 0) return;
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 > FLUID_PANEL) cx1 = FLUID_PANEL;
    if (cy1 > FLUID_PANEL) cy1 = FLUID_PANEL;
    for (int py = 0; py < h; py++) {
        int y = y0 + py; if (y < cy0 || y >= cy1) continue;
        uint8_t bits = gl[py * 7 / h];
        if (!bits) continue;
        uint16_t *row = fb + y * FLUID_PANEL;
        for (int px = 0; px < w; px++) {
            int x = x0 + px; if (x < cx0 || x >= cx1) continue;
            if ((bits >> (4 - px * 5 / w)) & 1) row[x] = c;
        }
    }
}

/* no contour ring around the condensed letters: cells next to a letter only dither */
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

/* (key, colour) of one cell — what render.js would draw there */
static inline void cell_look(const fluid_draw_t *d, const fluid_frame_t *s, const fluid_draw_opts_t *o,
                             int i, int j, int k, int rmax, uint32_t frame, uint16_t *key_out, uint16_t *col_out) {
    const int W = s->W;
    const int mode = o->edge_mode;
    const float v = s->view[k] * IU;
    const bool feat = s->feat[k] != 0;
    int dots = -1;
    float smax = 0.f, cell_mean = -1.f;
    if (feat) {
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
    const fluid_rgba16_t *c = &s->col[k];
    if (pal != FLUID_PAL_PASTEL) { r = lo[0] + (hi[0] - lo[0]) * br; g = lo[1] + (hi[1] - lo[1]) * br; b = lo[2] + (hi[2] - lo[2]) * br; }
    else {
        float bt = (cell_mean >= 0.f ? vb * 1.1f : 0.3f + 0.85f * vb) * (255.f / 65535.f);
        r = c->r * bt; g = c->g * bt; b = c->b * bt;
    }
    if (feat) { if (pal != FLUID_PAL_PASTEL) { r = hi[0]; g = hi[1]; b = hi[2]; } else { r = 250; g = 246; b = 255; } }
    float a = c->a * (1.f / 65535.f);
    if (a > 0.02f) { r += (ac[0] - r) * a; g += (ac[1] - g) * a; b += (ac[2] - b) * a; }
    *col_out = rgb565_be((int)r, (int)g, (int)b);
    *key_out = dots > 0 ? (uint16_t)dots : (uint16_t)(KEY_GLYPH | glyph);
}

/* one pixel row of a glyph / stroke cell */
static inline void raster_glyph_row(uint16_t *row, int y, int x0, int x1, int y0, int y1, uint16_t key, uint16_t col,
                                    const fluid_draw_opts_t *o) {
    int cwp = x1 - x0, chp = y1 - y0;
    int gw = cwp - 2 < 5 ? (cwp < 5 ? cwp : 5) : cwp - 2, gh = chp - 3 < 7 ? chp : chp - 3;
    int gx = x0 + (cwp - gw) / 2, gy = y0 + (chp - gh) / 2;
    int py = y - gy;
    if (py < 0 || py >= gh || gw <= 0) return;
    int gch = key & 0xFF;
    if (gch & 0x80) {                     /* streamline: 0 '-', 1 '\', 2 '|', 3 '/' */
        int x;
        switch (gch & 3) {
        case 0:  if (py != gh / 2) return; for (x = gx; x < gx + gw; x++) row[x] = col; return;
        case 1:  x = gx + py * gw / gh; break;
        case 2:  x = gx + gw / 2; break;
        default: x = gx + gw - 1 - py * gw / gh; break;
        }
        if (x >= x0 && x < x1) row[x] = col;
        return;
    }
    const uint8_t *gl = o->font((char)gch);
    uint8_t bits = gl ? gl[py * 7 / gh] : 0;
    if (!bits) return;
    for (int px = 0; px < gw; px++) if ((bits >> (4 - px * 5 / gw)) & 1) row[gx + px] = col;
}

int fluid_draw(fluid_draw_t *d, const fluid_frame_t *s, uint16_t *fb,
               const fluid_draw_opts_t *o, int *y0out, int *y1out) {
    const int W = s->W, H = s->H;
    const int pal = (o->palette >= 0 && o->palette < FLUID_PAL_COUNT) ? o->palette : FLUID_PAL_PASTEL;
    const bool mono = pal != FLUID_PAL_PASTEL;
    const float *lo = PAL[pal][0], *hi = PAL[pal][1], *ac = PAL[pal][2];
    const char *ramp = o->ramp ? o->ramp : " .:-=+*#";
    fluid_draw_opts_t oo = *o; oo.ramp = ramp;
    const int rmax = (int)strlen(ramp) - 1;
    const float cw = (float)FLUID_PANEL / W, ch = (float)FLUID_PANEL / H;
    const float melt = o->melt;
    const uint32_t frame = (uint32_t)(o->time * SHIMMER_HZ);
    int dy0 = FLUID_PANEL, dy1 = 0, drawn = 0;

    /* condensed letters: which have enough ink, and which cells they cover (the box) */
    struct { int li; float ink, hh, acc; } shown[FLUID_MAX_LETTERS];
    int nshown = 0;
    memset(d->skip, 0, (size_t)W * H);
    for (int li = 0; li < o->nletters && li < FLUID_MAX_LETTERS && o->font; li++) {
        const fluid_letter_t *L = &o->letters[li];
        if (L->ch == ' ') continue;
        float sx = (L->x + L->w / 2) / FLUID_PANEL * 2.f - 1.f, sy = (L->y + L->h / 2) / FLUID_PANEL * 2.f - 1.f;
        float ink = fluid_frame_sample(s, s->ink, 1.f / FQ_UNIT, sx, sy);
        if (ink < 0.14f + melt) continue;
        shown[nshown].li = li; shown[nshown].ink = ink;
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

    for (int j = 0; j < H; j++) {
        /* A: what each cell of this row looks like; which changed */
        FLUID_PROF_BEGIN(c0);
        bool any = false;
        for (int i = 0; i < W; i++) {
            const int k = j * W + i;
            uint16_t key = KEY_EMPTY, col = 0;
            if (d->skip[k]) key = KEY_SKIP;
            else if (s->mask[k]) cell_look(d, s, &oo, i, j, k, rmax, frame, &key, &col);
            bool dirty = key == KEY_SKIP || d->force || key != d->key[k] || col != d->col[k];
            d->rdirty[i] = dirty;
            if (dirty) {
                any = true;
                d->key[k] = key; d->col[k] = col;
                if (key != KEY_EMPTY && key != KEY_SKIP) drawn++;
            }
        }
        FLUID_PROF_END(FP_DSAMPLE, c0);
        if (!any) continue;
        /* B: rasterise the changed cells, one pixel row at a time (sequential fb writes) */
        uint32_t c1 = fluid_cycles();
        const int ya = d->cy[j], yb = d->cy[j + 1];
        if (ya < dy0) dy0 = ya;
        if (yb > dy1) dy1 = yb;
        for (int y = ya; y < yb; y++) {
            uint16_t *row = fb + y * FLUID_PANEL;
            const int r = d->yrow[y];
            for (int i = 0; i < W; i++) {
                if (!d->rdirty[i]) continue;
                const int k = j * W + i, x0 = d->cx[i], x1 = d->cx[i + 1];
                const uint16_t key = d->key[k], col = d->col[k];
                for (int x = x0; x < x1; x++) row[x] = 0;
                if (key == KEY_EMPTY || key == KEY_SKIP) continue;
                if (key < KEY_GLYPH) {
                    if (r < 0) continue;
                    for (int c = 0; c < 2; c++) {
                        if (!(key & (1u << fluid_braille_bit(c, r)))) continue;
                        uint16_t *px = row + d->dx[i * 2 + c];
                        for (int q = 0; q < d->dot_w; q++) px[q] = col;
                    }
                } else {
                    raster_glyph_row(row, y, x0, x1, ya, yb, key, col, &oo);
                }
            }
        }
        FLUID_PROF_END(FP_DRASTER, c1);
    }
    d->force = false;

    /* condensed letters ride the surface (y follows the wave height) and decode from
     * ramp glyphs into the real letter as ink arrives */
    for (int n = 0; n < nshown; n++) {
        const fluid_letter_t *L = &o->letters[shown[n].li];
        float ink = shown[n].ink;
        char c2;
        if (ink > 0.36f + melt * 1.5f) c2 = L->ch;
        else { int idx = (int)lrintf(fmaxf(0.f, ink - melt) * rmax); c2 = ramp[idx < 1 ? 1 : idx > rmax ? rmax : idx]; }
        float br = fminf(1.f, 0.55f + ink), r, g, b;
        if (mono) { r = lo[0] + (hi[0] - lo[0]) * br; g = lo[1] + (hi[1] - lo[1]) * br; b = lo[2] + (hi[2] - lo[2]) * br; }
        else { r = g = b = 255.f * br; }
        if (L->acc) { float a = fmaxf(shown[n].acc, 0.85f); if (a > 1.f) a = 1.f; r += (ac[0] - r) * a; g += (ac[1] - g) * a; b += (ac[2] - b) * a; }
        int gw = (int)(L->w * 0.8f), gh = (int)(L->h * 0.72f);
        int margin = ((int)L->h - gh) / 2 - 1;
        if (margin < 0) margin = 0;
        float yo = -shown[n].hh * 7.f;
        if (yo > 3.f) yo = 3.f;
        if (yo < -3.f) yo = -3.f;
        int yoff = (int)lrintf(yo);
        if (yoff > margin) yoff = margin;
        if (yoff < -margin) yoff = -margin;
        int gx = (int)lrintf(L->x + (L->w - gw) * 0.5f), gy = (int)lrintf(L->y) + margin + yoff;
        draw_glyph(fb, o->font(c2), gx, gy, gw, gh, rgb565_be((int)r, (int)g, (int)b),
                   (int)floorf(L->x), (int)floorf(L->y), (int)ceilf(L->x + L->w), (int)ceilf(L->y + L->h));
        drawn++;
    }

    *y0out = dy1 > dy0 ? dy0 : 0;
    *y1out = dy1 > dy0 ? dy1 : 0;
    return drawn;
}
