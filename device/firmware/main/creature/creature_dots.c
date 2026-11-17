/*
 * SPDX-License-Identifier: MIT
 *
 * creature_dots.c: a rigged creature on the braille lattice (creature_dots.h). Port of device/creatures/js/clip-dots.js;
 * the steps are numbered as there.
 *
 * The cost is memory, not arithmetic: the dots live in PSRAM behind a 32 KB cache, so every pass over the creature's
 * box pays for the bytes it touches. A dot is therefore one 8-byte record (field, depth, owner, priority, colour index,
 * flags), cleared with one memset a row, and the passes are few. The row-parallel ones run as two bands of rows, the
 * second on the other core (ESP), joined before the next; features run alone between them.
 */
#include "creature_dots.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
static inline uint32_t cycles(void) { return (uint32_t)esp_cpu_get_cycle_count(); }
#define CD_CALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define CD_FREE(p)   heap_caps_free(p)
#define CD_BAND_CORE 0           /* the second band's core: the fluid task (the first band) runs on core 1          */
#define CD_SHARE     55          /* percent of the rows' work in the first band: core 0 also draws the panel  */
#define CD_BAND_PRIO 3           /* above the drawer (2) on that core: the band is waited on (below it, the band
                                  * starved and core 1 waited ~20 ms walking); the drawer, light now, has the slack */
#else
static inline uint32_t cycles(void) { return 0; }
#define CD_CALLOC(n) calloc(1, (n))
#define CD_FREE(p)   free(p)
#define CD_SHARE     50
#endif

#define THRESH 0.6f
#define FS     2048.f                       /* the field in fixed point                                              */
#define TQ     ((uint16_t)(THRESH * FS + 0.5f))
#define ZS     4096.f                       /* a depth in fixed point, biased by 32768; 0 is none                    */
#define Z01    41                           /* 0.01, 0.02, 0.05 in depth units                                       */
#define Z02    82
#define Z05    205
#define TWINKLE 12                          /* the dust: each cell row's motes twinkle every this many frames, rows
                                             * scattered (clip-dots.js TWINKLE): a few rows a frame for the panel to resend */
#define NLINE  128                          /* the outline's shades by depth (palette 0..NLINE-1)                    */
enum { FR = 1, FE = 2, FH = 4, FA = 8, FB = 16 };   /* flags: region, eye, halo, and the halo's two scratch rows */
enum { C_IRIS = NLINE, C_PUPIL, C_SHINE, C_NOSE, C_EAR, C_BELLY, C_WHISKER, C_BOWL, C_N, C_LINE = 255 };
enum { ST_PROJ, ST_SOLID, ST_LINES, ST_FEAT, ST_HALO, ST_CELLS, ST_WAIT, ST_BAND };
#define FON    0x1000000u                   /* the floor layer: a dot's colour, this bit when it is on               */

typedef struct { float d[3], r[3], u[3], c[3]; } cam_t;
typedef struct { float u, v, d, i00, i01, i11, eu, ev, h, A[3][3]; } ell_t;
typedef struct { float a[3], b[3], r0, r1, facing, invL; } capz_t;
typedef struct { uint16_t f, z; uint8_t g, pri, col, fl; } dot_t;   /* g: owner group + 1 (0: none); pri 0: off */

struct cd {
    int W, H, DW, DH, N;
    float dsx, dsy, floor_row;
    dot_t *dot;
    uint8_t *cdots; uint32_t *ccol;
    int bx0, by0, bx1, by1;                 /* this frame's box (dots) [x0, x1) x [y0, y1)                          */
    int ux0, uy0, ux1, uy1;                 /* ... and its union with last frame's: what is cleared                 */
    int split, csplit;                      /* the bands' split: a dot row for the box passes, a cell row for cells */
    cam_t cam;
    uint32_t pal[C_N]; float body[3];
    int thigh[2]; float hip[2];
    int zlo[2], zhi[2], zlo_all; float lscale;
    /* the world, what does not change (made once): each dot's vignette (0: outside, never drawn), each floor row's depth
     * and nearness, and the dust's motes (a few percent of the dots: index, how much they lean to the horizon) */
    float *vig, *row_zz, *row_near; float horizon;
    int nmote; uint32_t *mote_i; float *mote_band; int32_t *mote_ph; uint32_t *mote_rgb; int *mote_row;
    int ry0, ry1, rx0, rx1;                 /* the last feature region's box (dots, inclusive)                       */
    ell_t pe[CC_SHAPES_MAX]; capz_t pc[CC_SHAPES_MAX]; uint8_t pok[CC_SHAPES_MAX];   /* this frame's projections */
    float sf[CC_SHAPES_MAX], kk[CC_SHAPES_MAX];  /* a blob's surface fraction of its field radius, and its square */
    int *wrow;                              /* per dot row: the body's work there (the split's balance)               */
    /* the floor layer, kept while the camera does not move along it: a colour (0: none) | FON, per dot */
    uint32_t *frgb; float fscroll; bool fvalid, floor_new;
    uint8_t *fcell; uint32_t *fccol;        /* the floor layer as cells (bits, colour), made with it                 */
    uint8_t *touch, *cat; int8_t *bp; uint16_t *bl;   /* per cell: reached by the creature or its halo; its dots; scratch */
    const float *world; const cc_shape_t *sh; int ns; uint32_t frame;
    uint32_t prof[CD_STAGES];
#ifdef ESP_PLATFORM
    TaskHandle_t task; SemaphoreHandle_t go, done; void (*job)(cd_t *, int); volatile bool quit;
#endif
};

static inline float minf(float a, float b) { return a < b ? a : b; }
static inline float maxf(float a, float b) { return a > b ? a : b; }
static inline float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static float sstep(float a, float b, float x) { const float t = clampf((x - a) / (b - a), 0.f, 1.f); return t * t * (3.f - 2.f * t); }
static inline int ffloor(float x) { const int i = (int)x; return i - (x < (float)i); }   /* floorf is a call on the S3 */
static inline uint16_t zq(float z) { const int v = (int)(z * ZS + 32768.5f); return (uint16_t)(v < 1 ? 1 : v > 65535 ? 65535 : v); }
static inline float zf(uint16_t q) { return (float)((int)q - 32768) * (1.f / ZS); }

static uint32_t hash3u(int i, int j, int k) {
    uint32_t h = (uint32_t)i * 374761393u + (uint32_t)j * 668265263u + (uint32_t)k * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
static inline float hash3f(int i, int j, int k) { return (float)hash3u(i, j, k) * 2.3283064e-10f; }   /* 0..1, no double */
static inline uint32_t prob_u(float p) { return p >= 1.f ? 0xffffffffu : p <= 0.f ? 0u : (uint32_t)(p * 4294967296.f); }
static inline unsigned lum(uint32_t v) { return (v >> 16 & 255) + (v >> 8 & 255) + (v & 255); }

static uint32_t pack(const float *c) {
    /* no lroundf (a library call on the S3): values are 0..1, so + 0.5 and truncate rounds */
    const int r = (int)(clampf(c[0], 0, 1) * 255.f + 0.5f), g = (int)(clampf(c[1], 0, 1) * 255.f + 0.5f), b = (int)(clampf(c[2], 0, 1) * 255.f + 0.5f);
    return (uint32_t)(r << 16 | g << 8 | b);
}
static void unpack(uint32_t v, float *c) { c[0] = (v >> 16 & 255) / 255.f; c[1] = (v >> 8 & 255) / 255.f; c[2] = (v & 255) / 255.f; }

/* ── the bands: part 0 here, part 1 on the other core, joined ─────────────────── */

typedef void (*band_fn)(cd_t *d, int part);
static void band(int y0, int y1, int split, int part, int *a, int *b) {
    split = split < y0 ? y0 : split > y1 ? y1 : split;
    *a = part ? split : y0; *b = part ? y1 : split;
}
#ifdef ESP_PLATFORM
static void band_task(void *arg) {
    cd_t *d = arg;
    for (;;) {
        xSemaphoreTake(d->go, portMAX_DELAY);
        if (d->quit) break;
        const uint32_t t = cycles();
        d->job(d, 1);
        d->prof[ST_BAND] += cycles() - t;
        xSemaphoreGive(d->done);
    }
    xSemaphoreGive(d->done);
    vTaskDelete(NULL);
}
static void split_run(cd_t *d, band_fn fn) {
    if (!d->task) { fn(d, 0); fn(d, 1); return; }
    d->job = fn;
    xSemaphoreGive(d->go);
    fn(d, 0);
    const uint32_t t = cycles();
    xSemaphoreTake(d->done, portMAX_DELAY);
    d->prof[ST_WAIT] += cycles() - t;
}
#else
static void split_run(cd_t *d, band_fn fn) { fn(d, 0); fn(d, 1); }   /* the same work, one band after the other */
#endif

cd_t *cd_create(const cc_t *c, int W, int H) {
    cd_t *d = CD_CALLOC(sizeof(cd_t));
    if (!d) return NULL;
    d->W = W; d->H = H; d->DW = 2 * W; d->DH = 4 * H; d->N = d->DW * d->DH;
    d->dsx = CD_VIEW_SIZE / d->DW; d->dsy = CD_VIEW_SIZE / d->DH; d->floor_row = CD_FLOOR_AT * d->DH;
    const size_t N = (size_t)d->N, NC = (size_t)W * H;
    const int DW = d->DW, DH = d->DH;
    d->dot = CD_CALLOC(sizeof(dot_t) * N);
    d->cdots = CD_CALLOC(NC); d->ccol = CD_CALLOC(sizeof(uint32_t) * NC);
    d->frgb = CD_CALLOC(sizeof(uint32_t) * N);
    d->fcell = CD_CALLOC(NC); d->fccol = CD_CALLOC(sizeof(uint32_t) * NC);
    d->touch = CD_CALLOC(NC); d->cat = CD_CALLOC(NC); d->bp = CD_CALLOC(NC); d->bl = CD_CALLOC(sizeof(uint16_t) * NC);
    d->vig = CD_CALLOC(sizeof(float) * N); d->row_zz = CD_CALLOC(sizeof(float) * (size_t)DH); d->row_near = CD_CALLOC(sizeof(float) * (size_t)DH);
    d->wrow = CD_CALLOC(sizeof(int) * (size_t)DH); d->mote_row = CD_CALLOC(sizeof(int) * ((size_t)H + 1));
    if (!d->dot || !d->cdots || !d->ccol || !d->frgb || !d->fcell || !d->fccol || !d->touch || !d->cat || !d->bp || !d->bl ||
        !d->vig || !d->row_zz || !d->row_near || !d->wrow || !d->mote_row) { cd_destroy(d); return NULL; }
    {   /* the fixed colours; the outline's shades are made each frame (their depth range) */
        float iris[3], wh[3];
        unpack(cc_color(c, "iris"), iris); unpack(cc_color(c, "whisker"), wh); unpack(cc_color(c, "body"), d->body);
        const float pup[3] = { iris[0] * 0.75f + 0.25f, iris[1] * 0.75f + 0.25f, iris[2] * 0.75f + 0.25f };
        for (int q = 0; q < 3; q++) wh[q] = minf(1.f, wh[q] * 1.3f);
        d->pal[C_IRIS] = cc_color(c, "iris"); d->pal[C_PUPIL] = pack(pup); d->pal[C_SHINE] = cc_color(c, "shine");
        d->pal[C_NOSE] = cc_color(c, "nose"); d->pal[C_EAR] = cc_color(c, "ear"); d->pal[C_BELLY] = cc_color(c, "belly");
        d->pal[C_WHISKER] = pack(wh); d->pal[C_BOWL] = cc_color(c, "bowl");
    }
    d->thigh[0] = cc_node_find(c, "thigh.L"); d->thigh[1] = cc_node_find(c, "thigh.R");
    {
        const cc_shape_t *sh; const int ns = cc_shapes(c, &sh);
        for (int q = 0; q < ns; q++) { d->sf[q] = sh[q].kind == CC_BLOB ? sqrtf(1.f - cbrtf(THRESH / sh[q].st)) : 1.f; d->kk[q] = d->sf[q] * d->sf[q]; }
    }
    /* the world's constants */
    d->horizon = d->floor_row - 16.f * DH / 128.f;
    const float R = DW / 2.f;
    int nm = 0;
    for (int pass = 0; pass < 2; pass++) {      /* count the motes, then keep them (in dot order: by row) */
        if (pass) {
            d->mote_i = CD_CALLOC(sizeof(uint32_t) * (size_t)(nm + 1)); d->mote_band = CD_CALLOC(sizeof(float) * (size_t)(nm + 1));
            d->mote_ph = CD_CALLOC(sizeof(int32_t) * (size_t)(nm + 1)); d->mote_rgb = CD_CALLOC(sizeof(uint32_t) * (size_t)(nm + 1));
            if (!d->mote_i || !d->mote_band || !d->mote_ph || !d->mote_rgb) { cd_destroy(d); return NULL; }
        }
        for (int Y = 0; Y < DH; Y++) {
            if (!pass && Y > d->horizon) { d->row_zz[Y] = (d->floor_row - d->horizon) / (Y + 0.5f - d->horizon); d->row_near[Y] = sstep(d->horizon, d->horizon + 14.f * DH / 128.f, (float)Y); }
            if (pass && !(Y & 3)) d->mote_row[Y >> 2] = d->nmote;
            const float band = expf(-fabsf(Y - d->horizon) / 14.f);
            for (int X = 0; X < DW; X++) {
                const int i = Y * DW + X;
                if (!pass) {
                    const float ex = (X + 0.5f - DW / 2.f) / R, ey = (Y + 0.5f - DH / 2.f) / (DH / 2.f), rr = sqrtf(ex * ex + ey * ey);
                    const float v = 1.f - sstep(0.9f, 0.99f, rr);   /* the world fills the disc, fading only at its rim */
                    d->vig[i] = v > 0.01f ? v : 0.f;
                }
                const double seed = (double)hash3u(X, Y, 11) / 4294967296.0;
                if (d->vig[i] <= 0.f || seed >= 0.02 + 0.09 * band) continue;
                if (!pass) { nm++; continue; }
                d->mote_i[d->nmote] = (uint32_t)i; d->mote_band[d->nmote] = band; d->mote_ph[d->nmote++] = -1;
            }
        }
    }
    d->mote_row[H] = d->nmote;
#ifdef ESP_PLATFORM
    d->go = xSemaphoreCreateBinary(); d->done = xSemaphoreCreateBinary();
    if (d->go && d->done && xTaskCreatePinnedToCore(band_task, "cd_band", 4096, d, CD_BAND_PRIO, &d->task, CD_BAND_CORE) != pdPASS) d->task = NULL;
#endif
    return d;
}

void cd_destroy(cd_t *d) {
    if (!d) return;
#ifdef ESP_PLATFORM
    if (d->task) { d->quit = true; xSemaphoreGive(d->go); xSemaphoreTake(d->done, portMAX_DELAY); }
    if (d->go) vSemaphoreDelete(d->go);
    if (d->done) vSemaphoreDelete(d->done);
#endif
    CD_FREE(d->dot); CD_FREE(d->cdots); CD_FREE(d->ccol); CD_FREE(d->frgb); CD_FREE(d->fcell); CD_FREE(d->fccol);
    CD_FREE(d->touch); CD_FREE(d->cat); CD_FREE(d->bp); CD_FREE(d->bl);
    CD_FREE(d->vig); CD_FREE(d->row_zz); CD_FREE(d->row_near); CD_FREE(d->wrow); CD_FREE(d->mote_row);
    CD_FREE(d->mote_i); CD_FREE(d->mote_band); CD_FREE(d->mote_ph); CD_FREE(d->mote_rgb);
    CD_FREE(d);
}

/* ── projection ────────────────────────────────────────────────────────── */

static void xf(const float *w, int n, const float *p, float *o) {
    const float *m = w + n * 12;
    o[0] = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
    o[1] = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7];
    o[2] = m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11];
}
static void xv(const float *w, int n, const float *v, float *o) {
    const float *m = w + n * 12;
    o[0] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2];
    o[1] = m[4] * v[0] + m[5] * v[1] + m[6] * v[2];
    o[2] = m[8] * v[0] + m[9] * v[1] + m[10] * v[2];
}
static void proj(const cd_t *d, const float *p, float *o) {
    const float q[3] = { p[0] - d->cam.c[0], p[1], p[2] - d->cam.c[2] };
    o[0] = q[0] * d->cam.r[0] + q[1] * d->cam.r[1] + q[2] * d->cam.r[2];
    o[1] = q[0] * d->cam.u[0] + q[1] * d->cam.u[1] + q[2] * d->cam.u[2];
    o[2] = q[0] * d->cam.d[0] + q[1] * d->cam.d[1] + q[2] * d->cam.d[2];
}
static float dotv(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline float dot_u(const cd_t *d, int X) { return (X + 0.5f - d->DW / 2) * d->dsx; }
static inline float dot_v(const cd_t *d, int Y) { return (d->floor_row - (Y + 0.5f)) * d->dsy; }

static bool ellipse(const cd_t *d, const float *world, const cc_shape_t *s, ell_t *e) {
    float c[3], cp[3];
    xf(world, s->node, s->p, c); proj(d, c, cp);
    float s00 = 0, s01 = 0, s11 = 0, hh = 0;
    for (int k = 0; k < 3; k++) {
        const float v[3] = { s->rq[k] * s->r[k], s->rq[3 + k] * s->r[k], s->rq[6 + k] * s->r[k] };
        xv(world, s->node, v, e->A[k]);
        const float au = dotv(e->A[k], d->cam.r), av = dotv(e->A[k], d->cam.u), ad = dotv(e->A[k], d->cam.d);
        s00 += au * au; s01 += au * av; s11 += av * av; hh += ad * ad;
    }
    const float det = s00 * s11 - s01 * s01;
    if (det < 1e-12f) return false;
    e->u = cp[0]; e->v = cp[1]; e->d = cp[2];
    const float idet = 1.f / det;
    e->i00 = s11 * idet; e->i01 = -s01 * idet; e->i11 = s00 * idet;
    e->eu = sqrtf(s00); e->ev = sqrtf(s11); e->h = sqrtf(hh);
    return true;
}
static inline float eq(const ell_t *e, float uu, float vv) { const float du = uu - e->u, dv = vv - e->v; return e->i00 * du * du + 2 * e->i01 * du * dv + e->i11 * dv * dv; }

static float node_scale(const float *w, int n) { return sqrtf(w[n * 12] * w[n * 12] + w[n * 12 + 4] * w[n * 12 + 4] + w[n * 12 + 8] * w[n * 12 + 8]); }
static void capsule(const cd_t *d, const float *world, const cc_shape_t *s, float r0, float r1, capz_t *c) {
    float a[3], b[3];
    xf(world, s->node, s->p, a); xf(world, s->node, s->p1, b);
    proj(d, a, c->a); proj(d, b, c->b);
    const float sc = node_scale(world, s->node);
    c->r0 = r0 * sc; c->r1 = r1 * sc; c->facing = 1.f;
    const float ex = c->b[0] - c->a[0], ey = c->b[1] - c->a[1], L = ex * ex + ey * ey;
    c->invL = L > 1e-12f ? 1.f / L : 0.f;
}
static bool in_capsule(const capz_t *c, float u, float v, float *zz) {
    const float ex = c->b[0] - c->a[0], ey = c->b[1] - c->a[1];
    float t = ((u - c->a[0]) * ex + (v - c->a[1]) * ey) * c->invL;
    t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
    const float r = c->r0 + (c->r1 - c->r0) * t, dx = u - c->a[0] - ex * t, dy = v - c->a[1] - ey * t, d2 = dx * dx + dy * dy;
    if (d2 >= r * r) return false;
    *zz = c->a[2] + (c->b[2] - c->a[2]) * t + sqrtf(r * r - d2);
    return true;
}
static void cone_capsule(const cd_t *d, const float *world, const cc_shape_t *s, capz_t *c) {
    capsule(d, world, s, 0.f, 0.f, c);
    float fa[3], pb[3], pt[3];
    xv(world, s->node, s->fa, fa); xf(world, s->node, s->p, pb); xf(world, s->node, s->p1, pt);
    const float ax[3] = { pt[0] - pb[0], pt[1] - pb[1], pt[2] - pb[2] };
    const float e2[3] = { ax[1] * fa[2] - ax[2] * fa[1], ax[2] * fa[0] - ax[0] * fa[2], ax[0] * fa[1] - ax[1] * fa[0] };
    float l2 = sqrtf(dotv(e2, e2)); if (l2 <= 0.f) l2 = 1.f;
    const float ex = c->b[0] - c->a[0], ey = c->b[1] - c->a[1];
    float l = sqrtf(ex * ex + ey * ey); if (l <= 0.f) l = 1.f;
    const float px = -ey / l, py = ex / l;
    const float fu = dotv(fa, d->cam.r), fv = dotv(fa, d->cam.u), gu = dotv(e2, d->cam.r) / l2, gv = dotv(e2, d->cam.u) / l2;
    const float sc = node_scale(world, s->node);
    const float p1 = s->r[0] * s->r[1] * (fu * px + fv * py), p2 = s->r[0] * (gu * px + gv * py);
    c->r0 = sqrtf(p1 * p1 + p2 * p2) * sc; c->r1 = 0.004f;
    c->facing = dotv(fa, d->cam.d);
}

/* the dots of a screen box (u, v) +- (eu, ev), clipped to the grid */
typedef struct { int x0, x1, y0, y1; } box_t;
static box_t sbox(const cd_t *d, float cu, float cv, float eu, float ev) {
    const float X0 = (cu - eu) / d->dsx + d->DW / 2 - 0.5f, X1 = (cu + eu) / d->dsx + d->DW / 2 - 0.5f;
    const float Y1 = d->floor_row - (cv - ev) / d->dsy - 0.5f, Y0 = d->floor_row - (cv + ev) / d->dsy - 0.5f;
    box_t b = { (int)floorf(X0), (int)ceilf(X1), (int)floorf(Y0), (int)ceilf(Y1) };
    if (b.x0 < 0) b.x0 = 0;
    if (b.y0 < 0) b.y0 = 0;
    if (b.x1 > d->DW - 1) b.x1 = d->DW - 1;
    if (b.y1 > d->DH - 1) b.y1 = d->DH - 1;
    return b;
}
static box_t cap_box(const cd_t *d, const capz_t *c) {
    const float m = maxf(c->r0, c->r1);
    return sbox(d, (c->a[0] + c->b[0]) / 2, (c->a[1] + c->b[1]) / 2, fabsf(c->b[0] - c->a[0]) / 2 + m, fabsf(c->b[1] - c->a[1]) / 2 + m);
}
static box_t blob_box(const cd_t *d, int q) { const ell_t *e = &d->pe[q]; return sbox(d, e->u, e->v, e->eu / d->sf[q], e->ev / d->sf[q]); }

static inline void set_dot(dot_t *p, int col, int pri) {
    if (pri < p->pri) return;
    p->pri = (uint8_t)pri; p->col = (uint8_t)col;
}
static void grow(box_t *acc, box_t b) {
    if (b.x0 < acc->x0) acc->x0 = b.x0;
    if (b.y0 < acc->y0) acc->y0 = b.y0;
    if (b.x1 > acc->x1) acc->x1 = b.x1;
    if (b.y1 > acc->y1) acc->y1 = b.y1;
}
/* a cell row (times 4) where the running sum of w over [y0, y1) reaches the first band's share */
static int balance(const int *w, int y0, int y1) {
    long tot = 0, run = 0;
    for (int Y = y0; Y < y1; Y++) tot += w[Y];
    for (int Y = y0; Y < y1; Y++) if ((run += w[Y]) * 100 >= tot * CD_SHARE) return (Y + 2) & ~3;
    return (y0 + y1) / 2 & ~3;
}

/* ── 1, 2: the body's field and the solid parts (band) ─────────────────────── */

static void ph_solid(cd_t *d, int part) {
    const int DW = d->DW;
    int a, b; band(d->uy0, d->uy1, d->split, part, &a, &b);
    if (d->ux1 > d->ux0) for (int Y = a; Y < b; Y++) memset(d->dot + Y * DW + d->ux0, 0, sizeof(dot_t) * (size_t)(d->ux1 - d->ux0));
    if (a < d->by0) a = d->by0;
    if (b > d->by1) b = d->by1;
    if (a >= b) return;
    const cc_shape_t *sh = d->sh;
    /* 1. the merged body: a metaball field, the nearest element surface per dot */
    for (int q = 0; q < d->ns; q++) {
        const cc_shape_t *s = &sh[q];
        if (s->kind != CC_BLOB || !d->pok[q]) continue;
        const ell_t e = d->pe[q];
        const box_t bx = blob_box(d, q);
        const int ya = bx.y0 > a ? bx.y0 : a, yb = bx.y1 < b - 1 ? bx.y1 : b - 1;
        const float kk = d->kk[q], stq = s->st * FS;
        const uint16_t topq = zq(e.d + e.h); const uint8_t g = (uint8_t)(s->group + 1);
        const float sx = d->dsx, a2 = e.i00 * sx * sx;          /* q along a row is a quadratic in X: stepped by adds */
        const float ikk = 1.f / kk, h2 = 0.5f / e.i00, isx = 1.f / sx;   /* the row's run: one divide a blob, not a row */
        for (int Y = ya; Y <= yb; Y++) {
            const float dv = dot_v(d, Y) - e.v, cdv = e.i11 * dv * dv, lin = 2.f * e.i01 * dv;
            dot_t *row = d->dot + Y * DW;
            /* only the run of the row inside the field: i00 du^2 + lin du + cdv < 1/kk, solved for du */
            const float disc = lin * lin - 4.f * e.i00 * (cdv - ikk);
            if (disc <= 0.f) continue;
            const float sq = sqrtf(disc), u0 = (-lin - sq) * h2 + e.u, u1 = (-lin + sq) * h2 + e.u;
            int xa = ffloor(u0 * isx + DW / 2 - 0.5f), xb = -ffloor(-(u1 * isx + DW / 2 - 0.5f));
            if (xa < bx.x0) xa = bx.x0;
            if (xb > bx.x1) xb = bx.x1;
            float du = dot_u(d, xa) - e.u;
            float qv = (e.i00 * du + lin) * du + cdv, dq = (2.f * e.i00 * du + lin) * sx + a2;
            for (int X = xa; X <= xb; X++, qv += dq, dq += 2.f * a2) {
                const float qf = qv * kk;
                if (qf >= 1.f) continue;
                dot_t *p = row + X;
                if (p->f < TQ) { const float o = 1.f - qf; p->f = (uint16_t)(p->f + (int)(stq * o * o * o)); }   /* past the threshold it is in */
                if (topq <= p->z) continue;                 /* cannot be nearer than what is there */
                const uint16_t z = zq(e.d + (qv < 1.f ? e.h * sqrtf(1.f - qv) : 0.f));
                if (z > p->z) { p->z = z; p->g = g; }
            }
        }
    }
    for (int Y = a; Y < b; Y++) {
        dot_t *p = d->dot + Y * DW;
        for (int X = d->bx0; X < d->bx1; X++) if (p[X].f < TQ) { p[X].z = 0; p[X].g = 0; }
    }
    /* 2. the solid parts, nearest wins */
    for (int q = 0; q < d->ns; q++) {
        const cc_shape_t *s = &sh[q];
        if (s->kind == CC_BLOB || s->kind == CC_PATCH || s->group >= CG_NSTRUCT || !d->pok[q]) continue;
        const uint8_t g = (uint8_t)(s->group + 1);
        if (s->kind == CC_ELL) {
            const ell_t e = d->pe[q];
            const box_t bx = sbox(d, e.u, e.v, e.eu, e.ev);
            for (int Y = bx.y0 > a ? bx.y0 : a; Y <= bx.y1 && Y < b; Y++) for (int X = bx.x0; X <= bx.x1; X++) {
                const float qv = eq(&e, dot_u(d, X), dot_v(d, Y));
                if (qv >= 1.f) continue;
                dot_t *p = d->dot + Y * DW + X; const uint16_t z = zq(e.d + e.h * sqrtf(1.f - qv));
                if (z > p->z) { p->z = z; p->g = g; }
            }
        } else {
            const capz_t cp = d->pc[q];
            const box_t bx = cap_box(d, &cp);
            for (int Y = bx.y0 > a ? bx.y0 : a; Y <= bx.y1 && Y < b; Y++) for (int X = bx.x0; X <= bx.x1; X++) {
                float zz;
                if (!in_capsule(&cp, dot_u(d, X), dot_v(d, Y), &zz)) continue;
                dot_t *p = d->dot + Y * DW + X; const uint16_t z = zq(zz);
                if (z > p->z) { p->z = z; p->g = g; }
            }
        }
    }
}

/* ── 3. lines (band): an outline, and an inner line where a nearer part crosses ─ */

static void ph_lines(cd_t *d, int part) {
    const int DW = d->DW, DH = d->DH;
    int a, b; band(d->by0, d->by1, d->split, part, &a, &b);
    int zlo = 65536, zhi = -1;
    for (int Y = a; Y < b; Y++) for (int X = d->bx0; X < d->bx1; X++) {
        dot_t *p = d->dot + Y * DW + X;
        const int g = p->g;
        if (!g) continue;
        if (g <= CG_BOWL) { if (p->z < zlo) zlo = p->z; if (p->z > zhi) zhi = p->z; }
        bool line = false;
        static const int DX[4] = { 1, -1, 0, 0 }, DY[4] = { 0, 0, 1, -1 };
        for (int n = 0; n < 4; n++) {
            const int x = X + DX[n], y = Y + DY[n];
            if (x < 0 || y < 0 || x >= DW || y >= DH) { line = true; break; }
            const dot_t *o = d->dot + y * DW + x; const int h = o->g;
            if (!h) { line = true; break; }
            if (h == g || g == CG_TORSO + 1) continue;
            if (p->z <= o->z + Z01) continue;
            if ((g == CG_HINDL + 1 || g == CG_HINDR + 1) && h == CG_TORSO + 1 && dot_v(d, Y) > d->hip[g == CG_HINDL + 1 ? 0 : 1]) continue;
            line = true; break;
        }
        if (line) set_dot(p, g == CG_BOWL + 1 || g == CG_YARN + 1 ? C_BOWL : C_LINE, 2);
    }
    d->zlo[part] = zlo; d->zhi[part] = zhi;
}

/* ── 4. features (one core): regions on the body's surface ─────────────────── */

static void region_edge(cd_t *d, int col, int p, bool keep_lines) {
    const int DW = d->DW;
    for (int Y = d->ry0; Y <= d->ry1; Y++) for (int X = d->rx0; X <= d->rx1; X++) {
        const int i = Y * DW + X;
        dot_t *q = d->dot + i;
        if (!(q->fl & FR)) continue;
        const bool edge = X == 0 || Y == 0 || X == DW - 1 || Y == d->DH - 1 || !(q[-1].fl & FR) || !(q[1].fl & FR) || !(q[-DW].fl & FR) || !(q[DW].fl & FR);
        if (edge && !(keep_lines && q->pri >= 2)) set_dot(q, col, p);
    }
}
/* clear the last region (its box), and make b the next one's */
static void clear_region(cd_t *d) {
    for (int Y = d->ry0; Y <= d->ry1; Y++) for (int X = d->rx0; X <= d->rx1; X++) d->dot[Y * d->DW + X].fl &= (uint8_t)~FR;
    d->rx0 = d->ry0 = 0; d->rx1 = d->ry1 = -1;
}
static void region_box(cd_t *d, int x0, int y0, int x1, int y1) { d->rx0 = x0; d->ry0 = y0; d->rx1 = x1; d->ry1 = y1; }
static void disc_region(cd_t *d, const cc_shape_t *s, bool facing) {
    clear_region(d);
    ell_t e;
    if (!ellipse(d, d->world, s, &e)) return;
    if (facing) {
        float l = sqrtf(dotv(e.A[2], e.A[2])); if (l <= 0.f) l = 1.f;
        if (dotv(e.A[2], d->cam.d) / l < 0.15f) return;
    }
    const box_t b = sbox(d, e.u, e.v, e.eu, e.ev);
    region_box(d, b.x0, b.y0, b.x1, b.y1);
    for (int Y = b.y0; Y <= b.y1; Y++) for (int X = b.x0; X <= b.x1; X++) {
        dot_t *p = d->dot + Y * d->DW + X; const float q = eq(&e, dot_u(d, X), dot_v(d, Y));
        if (q < 1.f && zq(e.d + e.h * sqrtf(1.f - q)) + Z02 >= p->z) p->fl |= FR;
    }
}
static void inv3(float A[3][3], float *o, bool *ok) {
    const float m[9] = { A[0][0], A[1][0], A[2][0], A[0][1], A[1][1], A[2][1], A[0][2], A[1][2], A[2][2] };
    const float dt = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
    *ok = fabsf(dt) >= 1e-12f;
    if (!*ok) return;
    o[0] = (m[4] * m[8] - m[5] * m[7]) / dt; o[1] = (m[2] * m[7] - m[1] * m[8]) / dt; o[2] = (m[1] * m[5] - m[2] * m[4]) / dt;
    o[3] = (m[5] * m[6] - m[3] * m[8]) / dt; o[4] = (m[0] * m[8] - m[2] * m[6]) / dt; o[5] = (m[2] * m[3] - m[0] * m[5]) / dt;
    o[6] = (m[3] * m[7] - m[4] * m[6]) / dt; o[7] = (m[1] * m[6] - m[0] * m[7]) / dt; o[8] = (m[0] * m[4] - m[1] * m[3]) / dt;
}

static void features(cd_t *d) {
    const int DW = d->DW, ns = d->ns;
    const cc_shape_t *sh = d->sh; const float *world = d->world; const cam_t *k = &d->cam;
    for (int q = 0; q < ns; q++) {
        const cc_shape_t *s = &sh[q];
        if (s->group != CG_EYE || s->eye != CC_IRIS) continue;
        disc_region(d, s, true);
        const int ex0 = d->rx0, ey0 = d->ry0, ex1 = d->rx1, ey1 = d->ry1;
        for (int Y = ey0; Y <= ey1; Y++) for (int X = ex0; X <= ex1; X++) { dot_t *p = d->dot + Y * DW + X; if (p->fl & FR) p->fl |= FE; }
        region_edge(d, C_IRIS, 4, false);
        for (int r = 0; r < ns; r++) {
            const cc_shape_t *t = &sh[r];
            if (t->group != CG_EYE || t->node != s->node || t->eye == CC_IRIS) continue;
            disc_region(d, t, true);
            const int col = t->eye == CC_SHINE ? C_SHINE : C_PUPIL, pri = t->eye == CC_SHINE ? 6 : 5;
            for (int Y = d->ry0; Y <= d->ry1; Y++) for (int X = d->rx0; X <= d->rx1; X++) {
                dot_t *p = d->dot + Y * DW + X;
                if ((p->fl & (FR | FE)) == (FR | FE)) set_dot(p, col, pri);
            }
        }
        for (int Y = ey0; Y <= ey1; Y++) for (int X = ex0; X <= ex1; X++) d->dot[Y * DW + X].fl &= (uint8_t)~FE;
    }
    for (int q = 0; q < ns; q++) {
        const cc_shape_t *s = &sh[q];
        if (s->group == CG_NOSE || s->group == CG_TONGUE) {
            disc_region(d, s, false);
            for (int Y = d->ry0; Y <= d->ry1; Y++) for (int X = d->rx0; X <= d->rx1; X++) { dot_t *p = d->dot + Y * DW + X; if (p->fl & FR) set_dot(p, C_NOSE, 5); }
        } else if (s->group == CG_MAW) { disc_region(d, s, false); region_edge(d, C_NOSE, 4, false); }
        else if (s->group == CG_MILK) { disc_region(d, s, false); region_edge(d, C_BELLY, 3, false); }
        else if (s->group == CG_EARIN) {
            clear_region(d);
            capz_t cp; cone_capsule(d, world, s, &cp);
            if (cp.facing < 0.1f) continue;
            const box_t b = cap_box(d, &cp);
            region_box(d, b.x0, b.y0, b.x1, b.y1);
            for (int Y = b.y0; Y <= b.y1; Y++) for (int X = b.x0; X <= b.x1; X++) {
                dot_t *p = d->dot + Y * DW + X; float zz;
                if (in_capsule(&cp, dot_u(d, X), dot_v(d, Y), &zz) && zq(zz) + Z02 >= p->z) p->fl |= FR;
            }
            region_edge(d, C_EAR, 4, false);
        } else if (s->kind == CC_PATCH) {          /* the white chest: a region of the body's own surface */
            clear_region(d);
            float C[3], A[3][3], inv[9]; bool ok;
            xf(world, s->node, s->p, C);
            for (int kq = 0; kq < 3; kq++) { const float v[3] = { s->rq[kq] * s->r[kq], s->rq[3 + kq] * s->r[kq], s->rq[6 + kq] * s->r[kq] }; xv(world, s->node, v, A[kq]); }
            inv3(A, inv, &ok);
            if (!ok) continue;
            /* the region is where the torso's surface point is inside the patch's ellipsoid: only the patch's
             * screen box (its ellipsoid's projected bounds) can hold one */
            if (!d->pok[q]) continue;
            box_t b = sbox(d, d->pe[q].u, d->pe[q].v, d->pe[q].eu, d->pe[q].ev);
            if (b.x0 < d->bx0) b.x0 = d->bx0;
            if (b.y0 < d->by0) b.y0 = d->by0;
            if (b.x1 > d->bx1 - 1) b.x1 = d->bx1 - 1;
            if (b.y1 > d->by1 - 1) b.y1 = d->by1 - 1;
            region_box(d, b.x0, b.y0, b.x1, b.y1);
            for (int Y = b.y0; Y <= b.y1; Y++) for (int X = b.x0; X <= b.x1; X++) {
                dot_t *p = d->dot + Y * DW + X;
                if (p->g != CG_TORSO + 1) continue;
                const float uu = dot_u(d, X), vv = dot_v(d, Y), zz = zf(p->z);
                const float P[3] = { k->c[0] + uu * k->r[0] + vv * k->u[0] + zz * k->d[0], uu * k->r[1] + vv * k->u[1] + zz * k->d[1], k->c[2] + uu * k->r[2] + vv * k->u[2] + zz * k->d[2] };
                const float qv[3] = { P[0] - C[0], P[1] - C[1], P[2] - C[2] };
                float l2 = 0; for (int r = 0; r < 3; r++) { const float l = inv[r * 3] * qv[0] + inv[r * 3 + 1] * qv[1] + inv[r * 3 + 2] * qv[2]; l2 += l * l; }
                if (l2 < 1.f) p->fl |= FR;
            }
            region_edge(d, C_BELLY, 3, true);
        }
    }
    for (int q = 0; q < ns; q++) {                 /* whiskers last: fine lines where nothing else is drawn */
        const cc_shape_t *s = &sh[q];
        if (s->group != CG_WHISKER) continue;
        capz_t cp; capsule(d, world, s, 0, 0, &cp); cp.r0 = cp.r1 = 0.55f * d->dsx;
        const box_t b = cap_box(d, &cp);
        for (int Y = b.y0; Y <= b.y1; Y++) for (int X = b.x0; X <= b.x1; X++) {
            dot_t *p = d->dot + Y * DW + X; float zz;
            if (p->pri) continue;
            if (in_capsule(&cp, dot_u(d, X), dot_v(d, Y), &zz) && (!p->g || zq(zz) + Z05 >= p->z)) set_dot(p, C_WHISKER, 1);
        }
    }
}

/* ── 5. the halo (band): the world kept off the creature by a disc of radius sqrt 5 (two dots across within a row of
 *    it, one dot across two rows away), as two separable passes; the box's 4-dot margin keeps them inside it ───── */

static void ph_halo_rows(cd_t *d, int part) {
    int a, b; band(d->by0, d->by1, d->split, part, &a, &b);
    const int x0 = d->bx0, x1 = d->bx1;
    for (int Y = a; Y < b; Y++) {
        dot_t *p = d->dot + Y * d->DW;
        if (x1 - x0 < 5) continue;
        int m0 = (p[x0].g | p[x0].pri) != 0, m1 = (p[x0 + 1].g | p[x0 + 1].pri) != 0, m2 = (p[x0 + 2].g | p[x0 + 2].pri) != 0, m3 = (p[x0 + 3].g | p[x0 + 3].pri) != 0;
        for (int X = x0 + 2; X < x1 - 2; X++) {
            const int m4 = (p[X + 2].g | p[X + 2].pri) != 0, c = m1 | m2 | m3;
            if (c | m0 | m4) p[X].fl |= (uint8_t)((c ? FB : 0) | FA);
            m0 = m1; m1 = m2; m2 = m3; m3 = m4;
        }
    }
}

/* ── 6, 7: the halo's columns, the world, the cells (band of cell rows) ──────── */

static inline uint32_t dot_rgb(const cd_t *d, const dot_t *p) {
    if (p->col != C_LINE) return d->pal[p->col];
    int l = NLINE - 1;
    if (d->lscale > 0.f) { l = (int)((float)((int)p->z - d->zlo_all) * d->lscale + 0.5f); l = l < 0 ? 0 : l >= NLINE ? NLINE - 1 : l; }
    return d->pal[l];
}

static const uint8_t BIT[8] = { 0, 1, 2, 6, 3, 4, 5, 7 };   /* dot (col << 2 | row) -> braille bit */

static void ph_cells(cd_t *d, int part) {
    const int DW = d->DW, DH = d->DH, W = d->W;
    int ja, jb; band(0, d->H, d->csplit, part, &ja, &jb);
    const int ya = ja * 4, yb = jb * 4;
    {   /* the halo's columns over this band's rows of the box */
        const int y0 = d->by0 + 2 > ya ? d->by0 + 2 : ya, y1 = d->by1 - 2 < yb ? d->by1 - 2 : yb;
        for (int Y = y0; Y < y1; Y++) {
            dot_t *p = d->dot + Y * DW; uint8_t *tc = d->touch + (Y >> 2) * W;
            for (int X = d->bx0 + 2; X < d->bx1 - 2; X++) {
                const uint8_t v = (uint8_t)((p[X - DW].fl | p[X].fl | p[X + DW].fl) & FA) | ((p[X - 2 * DW].fl | p[X + 2 * DW].fl) & FB);
                if (v) { p[X].fl |= FH; tc[X >> 1] = 1; }
            }
        }
    }
    static const float FLR[3] = { 0x8f / 255.f, 0x87 / 255.f, 0xc4 / 255.f }, TEX[3] = { 0x6c / 255.f, 0x65 / 255.f, 0x98 / 255.f },
                       DIM[3] = { 0x5c / 255.f, 0x56 / 255.f, 0x88 / 255.f }, MOTE[3] = { 0xb4 / 255.f, 0xac / 255.f, 0xec / 255.f };
    const float horizon = d->horizon;
    if (d->floor_new) {     /* the floor layer and its own cells: redrawn only when the camera moved along it (walking) */
        const float scroll = d->fscroll;
        memset(d->fcell + ja * W, 0, (size_t)((jb - ja) * W)); memset(d->fccol + ja * W, 0, sizeof(uint32_t) * (size_t)((jb - ja) * W));
        for (int Y = (int)horizon + 1 > ya ? (int)horizon + 1 : ya; Y < yb; Y++) {
            if (Y <= horizon) continue;
            const float zz = d->row_zz[Y], near = d->row_near[Y], lim = 13.f - 0.55f * zz;
            const uint32_t thr = prob_u(0.16f + 0.1f * (1.f - near));
            const int kk = (int)floorf(14.f / zz + 0.5f);
            const float x0 = (0.5f - DW / 2.f) * zz + scroll;
            uint32_t *fr = d->frgb + Y * DW; const float *vg = d->vig + Y * DW;
            uint8_t *fc = d->fcell + (Y >> 2) * W; uint32_t *fcc = d->fccol + (Y >> 2) * W; const int r = Y & 3;
            for (int X = 0; X < DW; X++) {
                fr[X] = 0;
                const float vig = vg[X];
                if (vig <= 0.f) continue;
                const float uw = x0 + X * zz;
                const float ul = uw + 13.f;              /* the lines either side of the middle, none under the cat */
                const float lane = fabsf(ul - 26.f * (float)ffloor(ul * (1.f / 26.f)) - 13.f);
                float al = 0.f; const float *col = NULL;
                if (lane > lim) { al = 0.85f * near; col = FLR; }
                else if (hash3u(ffloor(uw * 0.625f), kk, 7) < thr) { al = (0.5f + 0.25f * near) * (0.6f + 0.4f * hash3f(ffloor(uw), Y, 3)); col = TEX; }
                if (al <= 0.f) continue;
                const float kv = minf(1.f, al * vig), cc3[3] = { col[0] * kv, col[1] * kv, col[2] * kv };
                const uint32_t v = pack(cc3);
                fr[X] = v | (kv > 0.12f ? FON : 0);
                if (kv <= 0.12f) continue;
                fc[X >> 1] |= (uint8_t)(1 << BIT[(X & 1) << 2 | r]);
                if (lum(v) > lum(fcc[X >> 1])) fcc[X >> 1] = v;
            }
        }
    }
    /* cells: the floor's cells as they are, then the cells under the creature's box made again (its dots own their cell;
     * the floor there only outside the halo); a cell's colour is its highest priority dot's, the brightest among those */
    memcpy(d->cdots + ja * W, d->fcell + ja * W, (size_t)((jb - ja) * W)); memcpy(d->ccol + ja * W, d->fccol + ja * W, sizeof(uint32_t) * (size_t)((jb - ja) * W));
    const int cx0 = d->bx0 >> 1, cx1 = (d->bx1 + 1) >> 1 < W ? (d->bx1 + 1) >> 1 : W;
    const int cy0 = d->by0 >> 2 > ja ? d->by0 >> 2 : ja, cy1 = (d->by1 + 3) >> 2 < jb ? (d->by1 + 3) >> 2 : jb;
    for (int j = cy0; j < cy1; j++) for (int i = cx0; i < cx1; i++) { const int k = j * W + i; if (d->touch[k]) { d->cdots[k] = 0; d->ccol[k] = 0; d->bp[k] = -1; } }
    for (int Y = cy0 * 4; Y < cy1 * 4 && Y < DH; Y++) {
        const int j = Y >> 2, r = Y & 3;
        const dot_t *p = d->dot + Y * DW; const uint32_t *fr = d->frgb + Y * DW; const uint8_t *tc = d->touch + j * W;
        for (int X = cx0 * 2; X < cx1 * 2; X++) {
            if (!tc[X >> 1]) { X |= 1; continue; }               /* nothing of the creature in this cell: the floor's cell as made */
            int pr; uint32_t v;
            if (p[X].pri) { pr = p[X].pri; v = dot_rgb(d, p + X); }
            else if (!(p[X].fl & FH) && (fr[X] & FON)) { pr = 0; v = fr[X] & 0xffffff; }
            else continue;
            const int k = j * W + (X >> 1), bit = 1 << BIT[(X & 1) << 2 | r];
            d->cdots[k] |= (uint8_t)bit;
            if (pr >= 1) d->cat[k] |= (uint8_t)bit;
            const uint16_t l = (uint16_t)lum(v);
            if (pr > d->bp[k] || (pr == d->bp[k] && l > d->bl[k])) { d->bp[k] = (int8_t)pr; d->bl[k] = l; d->ccol[k] = v; }
        }
    }
    /* the dust: only its motes (made once), where neither the creature's halo nor the floor is; a cell with the creature
     * in it keeps only the creature's dots */
    for (int m = d->mote_row[ja]; m < d->mote_row[jb]; m++) {
        const int i = (int)d->mote_i[m];
        const dot_t *p = d->dot + i;
        if ((p->fl & FH) || d->frgb[i] || p->pri) continue;
        const int X = i % DW, Y = i / DW, k = (Y >> 2) * W + (X >> 1);
        if (d->cat[k]) continue;
        const int ph = (int)((d->frame + (uint32_t)((((Y >> 2) * 11) % d->H) * TWINKLE / d->H)) / TWINKLE);
        if (ph != d->mote_ph[m]) {                                /* its twinkle moved on: every third frame or so */
            d->mote_ph[m] = ph;
            const float tw = hash3f(X, Y, ph);
            const float al = (0.3f + 0.55f * tw) * (0.55f + 0.45f * d->mote_band[m]), kv = minf(1.f, al * d->vig[i]);
            const float *col = tw > 0.8f ? MOTE : DIM, cc3[3] = { col[0] * kv, col[1] * kv, col[2] * kv };
            d->mote_rgb[m] = kv > 0.12f ? pack(cc3) : 0;
        }
        const uint32_t v = d->mote_rgb[m], w = d->ccol[k];
        if (!v) continue;
        d->cdots[k] |= (uint8_t)(1 << BIT[(X & 1) << 2 | (Y & 3)]);
        if (!w || lum(v) > lum(w)) d->ccol[k] = v;
    }
    for (int j = cy0; j < cy1; j++) for (int i = cx0; i < cx1; i++) {
        const int k = j * W + i;
        if (d->cat[k]) d->cdots[k] = d->cat[k];                   /* a cell with the creature in it shows only its dots */
        if (!d->cdots[k]) d->ccol[k] = 0;
        d->cat[k] = 0; d->bp[k] = 0; d->bl[k] = 0; d->touch[k] = 0;   /* the scratch back to clear */
    }
}

/* ── the frame ─────────────────────────────────────────────────────────── */

void cd_draw(cd_t *d, const cc_t *c, uint32_t frame) {
    uint32_t _pc = cycles();
#define PROF_STAGE(n) do { const uint32_t _t = cycles(); d->prof[n] += _t - _pc; _pc = _t; } while (0)
    const float *world = d->world = cc_world(c);
    const cc_shape_t *sh; const int ns = d->ns = cc_shapes(c, &sh);
    d->sh = sh; d->frame = frame;
    const int DW = d->DW, DH = d->DH;
    /* the camera */
    float cx, cz, yawd; cc_cam(c, &cx, &cz, &yawd);
    const float yaw = yawd * 3.14159265358979f / 180.f, cy = cosf(yaw), el = (6.f * cy * cy) * 3.14159265358979f / 180.f;
    cam_t *k = &d->cam;
    k->d[0] = sinf(yaw) * cosf(el); k->d[1] = sinf(el); k->d[2] = cosf(yaw) * cosf(el);
    k->r[0] = k->d[2]; k->r[1] = 0.f; k->r[2] = -k->d[0];
    const float rl = sqrtf(k->r[0] * k->r[0] + k->r[2] * k->r[2]); k->r[0] /= rl; k->r[2] /= rl;
    k->u[0] = k->d[1] * k->r[2] - k->d[2] * k->r[1]; k->u[1] = k->d[2] * k->r[0] - k->d[0] * k->r[2]; k->u[2] = k->d[0] * k->r[1] - k->d[1] * k->r[0];
    k->c[0] = cx; k->c[1] = 0.f; k->c[2] = cz;
    {   /* the camera in whole dots across the screen: the creature and the floor then move together a dot at a time, a
         * planted paw stays on its floor dot, and the floor is redrawn only when it moved a dot (clip-dots.js the same) */
        const float u = (k->c[0] * k->r[0] + k->c[2] * k->r[2]) / d->dsx, fu = u - floorf(u + 0.5f);
        k->c[0] -= fu * d->dsx * k->r[0]; k->c[2] -= fu * d->dsx * k->r[2];
    }

    /* the creature's box this frame (every primitive's bounds, plus room for lines and the halo), and each row's work */
    box_t bb = { DW, -1, DH, -1 };
    memset(d->wrow, 0, sizeof(int) * (size_t)DH);
    for (int q = 0; q < ns; q++) {
        const cc_shape_t *s = &sh[q];
        d->pok[q] = 0;
        box_t b;
        if (s->kind == CC_BLOB || s->kind == CC_ELL || s->kind == CC_PATCH) {
            if (!ellipse(d, world, s, &d->pe[q])) continue;
            b = s->kind == CC_BLOB ? blob_box(d, q) : sbox(d, d->pe[q].u, d->pe[q].v, d->pe[q].eu, d->pe[q].ev);
        } else {
            capz_t *cp = &d->pc[q];
            if (s->kind == CC_CONE) cone_capsule(d, world, s, cp); else capsule(d, world, s, s->r[0], s->r[1], cp);
            capz_t w = *cp;
            if (s->group == CG_WHISKER) w.r0 = w.r1 = 0.55f * d->dsx;
            b = cap_box(d, &w);
        }
        d->pok[q] = 1;
        grow(&bb, b);
        if (s->kind != CC_PATCH && s->group < CG_NSTRUCT) for (int Y = b.y0; Y <= b.y1; Y++) d->wrow[Y] += b.x1 - b.x0 + 1;
    }
    const int M = 4;
    box_t nb = { bb.x0 - M, bb.x1 + 1 + M, bb.y0 - M, bb.y1 + 1 + M };
    if (nb.x0 < 0) nb.x0 = 0;
    if (nb.y0 < 0) nb.y0 = 0;
    if (nb.x1 > DW) nb.x1 = DW;
    if (nb.y1 > DH) nb.y1 = DH;
    if (bb.x1 < 0) nb = (box_t){ 0, 0, 0, 0 };
    {   /* what to clear: the union of last frame's box and this one's */
        const bool last = d->bx1 > d->bx0 && d->by1 > d->by0;
        d->ux0 = last && d->bx0 < nb.x0 ? d->bx0 : nb.x0; d->ux1 = last && d->bx1 > nb.x1 ? d->bx1 : nb.x1;
        d->uy0 = last && d->by0 < nb.y0 ? d->by0 : nb.y0; d->uy1 = last && d->by1 > nb.y1 ? d->by1 : nb.y1;
    }
    d->bx0 = nb.x0; d->bx1 = nb.x1; d->by0 = nb.y0; d->by1 = nb.y1;
    d->rx0 = d->ry0 = 0; d->rx1 = d->ry1 = -1;
    d->split = balance(d->wrow, d->by0, d->by1);
    for (int s = 0; s < 2; s++) {
        d->hip[s] = 0.f;
        if (d->thigh[s] < 0) continue;
        const float o[3] = { world[d->thigh[s] * 12 + 3], world[d->thigh[s] * 12 + 7], world[d->thigh[s] * 12 + 11] }; float p[3];
        proj(d, o, p); d->hip[s] = p[1];
    }
    PROF_STAGE(ST_PROJ);
    split_run(d, ph_solid);
    PROF_STAGE(ST_SOLID);
    split_run(d, ph_lines);
    {   /* the outline's shades over the body's depth range */
        const int zlo = d->zlo[0] < d->zlo[1] ? d->zlo[0] : d->zlo[1], zhi = d->zhi[0] > d->zhi[1] ? d->zhi[0] : d->zhi[1];
        d->zlo_all = zlo; d->lscale = zhi > zlo ? (float)(NLINE - 1) / (float)(zhi - zlo) : 0.f;
        for (int l = 0; l < NLINE; l++) {
            const float t = (float)l / (NLINE - 1); float col[3];
            for (int q = 0; q < 3; q++) col[q] = minf(1.f, d->body[q] * (0.5f + 0.5f * t) + (1.f - d->body[q]) * 0.45f * t * t);
            d->pal[l] = pack(col);
        }
    }
    PROF_STAGE(ST_LINES);
    features(d);
    PROF_STAGE(ST_FEAT);
    split_run(d, ph_halo_rows);
    PROF_STAGE(ST_HALO);
    {   /* the world: the floor moves when the camera did; the cells' split balances the box's rows and the floor's */
        const float scroll = (k->c[0] * k->r[0] + k->c[2] * k->r[2]) / d->dsx;
        d->floor_new = !d->fvalid || fabsf(scroll - d->fscroll) > 1e-4f;
        if (d->floor_new) { d->fvalid = true; d->fscroll = scroll; }
        for (int Y = 0; Y < DH; Y++) d->wrow[Y] = (Y >= d->by0 && Y < d->by1 ? 2 * (d->bx1 - d->bx0) : 0) + (d->floor_new && Y > d->horizon ? DW : 0) + 8;
        d->csplit = balance(d->wrow, 0, DH) >> 2;
    }
    split_run(d, ph_cells);
    PROF_STAGE(ST_CELLS);
}

void cd_cells(const cd_t *d, const uint8_t **dots, const uint32_t **rgb) { *dots = d->cdots; *rgb = d->ccol; }
void cd_prof(cd_t *d, uint32_t *out) { for (int i = 0; i < CD_STAGES; i++) { out[i] += d->prof[i]; d->prof[i] = 0; } }
