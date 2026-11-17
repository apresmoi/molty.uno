/*
 * SPDX-License-Identifier: MIT
 *
 * creature_dots.c: a rigged creature on the braille lattice (creature_dots.h). Port of device/creatures/js/clip-dots.js;
 * the steps are numbered as there. The heavy per-dot work (the field, the depths, the regions) runs only over the
 * creature's box; the world (floor, dust, vignette) is a hash per dot.
 */
#include "creature_dots.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_cpu.h"
static inline uint32_t cycles(void) { return (uint32_t)esp_cpu_get_cycle_count(); }
#include "esp_heap_caps.h"
#define CD_CALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define CD_FREE(p)   heap_caps_free(p)
#else
static inline uint32_t cycles(void) { return 0; }
#define CD_CALLOC(n) calloc(1, (n))
#define CD_FREE(p)   free(p)
#endif

#define THRESH 0.6f
#define CD_STAGES 8   /* box, body, parts, lines, features, halo, world, cells (creature_dots.h cd_prof) */
#define ZNONE  (-1e9f)

typedef struct { float d[3], r[3], u[3], c[3]; } cam_t;
typedef struct { float u, v, d, i00, i01, i11, eu, ev, h, A[3][3]; } ell_t;
typedef struct { float a[3], b[3], r0, r1, facing, invL; } capz_t;

struct cd {
    int W, H, DW, DH, N;
    float dsx, dsy, floor_row;
    float *field, *z;
    int8_t *own;
    uint8_t *region, *eye, *on, *pri, *halo;
    uint32_t *rgb;
    uint8_t *cdots; uint32_t *ccol;
    int bx0, by0, bx1, by1;
    cam_t cam;
    uint32_t col_body, col_iris, col_shine, col_nose, col_ear, col_belly, col_whisker, col_bowl;
    int thigh[2];
    /* the world, what does not change (made once): each dot's vignette (0: outside, never drawn), each floor row's depth
     * and nearness, and the dust's motes (a few percent of the dots: index, seed, how much they lean to the horizon) */
    float *vig, *row_zz, *row_near; float horizon;
    int nmote; uint32_t *mote_i; float *mote_seed, *mote_band;
    int ry0, ry1, rx0, rx1;             /* the last feature region's box (dots, inclusive)              */
    ell_t pe[CC_SHAPES_MAX]; capz_t pc[CC_SHAPES_MAX]; uint8_t pok[CC_SHAPES_MAX];   /* this frame's projections */
    float sf[CC_SHAPES_MAX], kk[CC_SHAPES_MAX];  /* a blob's surface fraction of its field radius, and its square */
    /* the floor layer, kept while the camera does not move along it: a colour (0: none) and on, per dot */
    uint32_t *frgb; uint8_t *fon; float fscroll; bool fvalid;
    uint8_t *fcell; uint32_t *fccol;    /* the floor layer as cells (bits, colour), made with it               */
    uint8_t *touch;                     /* per cell: the creature or its halo reaches it this frame           */
    float *mote_s97;                    /* a mote's seed x 97: its twinkle's phase                          */
    int32_t *mote_ph; uint32_t *mote_rgb;   /* each mote's current twinkle (its phase, its colour; 0: not shown) */
    uint32_t prof[CD_STAGES];
};

static inline float minf(float a, float b) { return a < b ? a : b; }
static inline float maxf(float a, float b) { return a > b ? a : b; }
static inline float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static float sstep(float a, float b, float x) { const float t = clampf((x - a) / (b - a), 0.f, 1.f); return t * t * (3.f - 2.f * t); }

static uint32_t hash3u(int i, int j, int k) {
    uint32_t h = (uint32_t)i * 374761393u + (uint32_t)j * 668265263u + (uint32_t)k * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
static inline float hash3f(int i, int j, int k) { return (float)hash3u(i, j, k) * 2.3283064e-10f; }   /* 0..1, no double */
static inline uint32_t prob_u(float p) { return p >= 1.f ? 0xffffffffu : p <= 0.f ? 0u : (uint32_t)(p * 4294967296.f); }
static double hash3(int i, int j, int k) {
    uint32_t h = (uint32_t)i * 374761393u + (uint32_t)j * 668265263u + (uint32_t)k * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (double)(h ^ (h >> 16)) / 4294967296.0;
}

static uint32_t pack(const float *c) {
    /* no lroundf (a library call on the S3): values are 0..1, so + 0.5 and truncate rounds */
    const int r = (int)(clampf(c[0], 0, 1) * 255.f + 0.5f), g = (int)(clampf(c[1], 0, 1) * 255.f + 0.5f), b = (int)(clampf(c[2], 0, 1) * 255.f + 0.5f);
    return (uint32_t)(r << 16 | g << 8 | b);
}
static void unpack(uint32_t v, float *c) { c[0] = (v >> 16 & 255) / 255.f; c[1] = (v >> 8 & 255) / 255.f; c[2] = (v & 255) / 255.f; }

cd_t *cd_create(const cc_t *c, int W, int H) {
    cd_t *d = CD_CALLOC(sizeof(cd_t));
    if (!d) return NULL;
    d->W = W; d->H = H; d->DW = 2 * W; d->DH = 4 * H; d->N = d->DW * d->DH;
    d->dsx = CD_VIEW_SIZE / d->DW; d->dsy = CD_VIEW_SIZE / d->DH; d->floor_row = CD_FLOOR_AT * d->DH;
    const size_t N = (size_t)d->N;
    d->field = CD_CALLOC(sizeof(float) * N); d->z = CD_CALLOC(sizeof(float) * N); d->own = CD_CALLOC(N);
    d->region = CD_CALLOC(N); d->eye = CD_CALLOC(N); d->on = CD_CALLOC(N); d->pri = CD_CALLOC(N); d->halo = CD_CALLOC(N);
    d->rgb = CD_CALLOC(sizeof(uint32_t) * N);
    d->cdots = CD_CALLOC((size_t)W * H); d->ccol = CD_CALLOC(sizeof(uint32_t) * (size_t)W * H);
    if (!d->field || !d->z || !d->own || !d->region || !d->eye || !d->on || !d->pri || !d->halo || !d->rgb || !d->cdots || !d->ccol) { cd_destroy(d); return NULL; }
    for (size_t i = 0; i < N; i++) { d->z[i] = ZNONE; d->own[i] = -1; }
    d->col_body = cc_color(c, "body"); d->col_iris = cc_color(c, "iris"); d->col_shine = cc_color(c, "shine");
    d->col_nose = cc_color(c, "nose"); d->col_ear = cc_color(c, "ear"); d->col_belly = cc_color(c, "belly");
    d->col_whisker = cc_color(c, "whisker"); d->col_bowl = cc_color(c, "bowl");
    d->thigh[0] = cc_node_find(c, "thigh.L"); d->thigh[1] = cc_node_find(c, "thigh.R");
    {
        const cc_shape_t *sh; const int ns = cc_shapes(c, &sh);
        for (int q = 0; q < ns; q++) { d->sf[q] = sh[q].kind == CC_BLOB ? sqrtf(1.f - cbrtf(THRESH / sh[q].st)) : 1.f; d->kk[q] = d->sf[q] * d->sf[q]; }
    }
    d->frgb = CD_CALLOC(sizeof(uint32_t) * N); d->fon = CD_CALLOC(N);
    d->fcell = CD_CALLOC((size_t)W * H); d->fccol = CD_CALLOC(sizeof(uint32_t) * (size_t)W * H); d->touch = CD_CALLOC((size_t)W * H);
    if (!d->frgb || !d->fon || !d->fcell || !d->fccol || !d->touch) { cd_destroy(d); return NULL; }
    d->bx0 = d->by0 = 0; d->bx1 = d->by1 = 0;
    /* the world's constants */
    const int DW = d->DW, DH = d->DH;
    d->vig = CD_CALLOC(sizeof(float) * N); d->row_zz = CD_CALLOC(sizeof(float) * (size_t)DH); d->row_near = CD_CALLOC(sizeof(float) * (size_t)DH);
    if (!d->vig || !d->row_zz || !d->row_near) { cd_destroy(d); return NULL; }
    d->horizon = d->floor_row - 16.f * DH / 128.f;
    const float R = DW / 2.f;
    int nm = 0;
    for (int Y = 0; Y < DH; Y++) {
        if (Y > d->horizon) { d->row_zz[Y] = (d->floor_row - d->horizon) / (Y + 0.5f - d->horizon); d->row_near[Y] = sstep(d->horizon, d->horizon + 14.f * DH / 128.f, (float)Y); }
        for (int X = 0; X < DW; X++) {
            const float ex = (X + 0.5f - DW / 2.f) / R, ey = (Y + 0.5f - DH / 2.f) / (DH / 2.f), rr = sqrtf(ex * ex + ey * ey);
            const float v = (1.f - sstep(0.62f, 0.97f, rr)) * (1.f - sstep(0.87f, 1.f, (float)Y / DH));
            d->vig[Y * DW + X] = v > 0.01f ? v : 0.f;
            const float band = expf(-fabsf(Y - d->horizon) / 14.f);
            if (v > 0.01f && hash3(X, Y, 11) < 0.02 + 0.09 * band) nm++;
        }
    }
    d->mote_i = CD_CALLOC(sizeof(uint32_t) * (size_t)(nm + 1)); d->mote_seed = CD_CALLOC(sizeof(float) * (size_t)(nm + 1)); d->mote_band = CD_CALLOC(sizeof(float) * (size_t)(nm + 1));
    d->mote_s97 = CD_CALLOC(sizeof(float) * (size_t)(nm + 1));
    d->mote_ph = CD_CALLOC(sizeof(int32_t) * (size_t)(nm + 1)); d->mote_rgb = CD_CALLOC(sizeof(uint32_t) * (size_t)(nm + 1));
    if (!d->mote_i || !d->mote_seed || !d->mote_band || !d->mote_s97 || !d->mote_ph || !d->mote_rgb) { cd_destroy(d); return NULL; }
    for (int m = 0; m < nm; m++) d->mote_ph[m] = -1;
    for (int Y = 0; Y < DH; Y++) for (int X = 0; X < DW; X++) {
        const int i = Y * DW + X; const float band = expf(-fabsf(Y - d->horizon) / 14.f); const double seed = hash3(X, Y, 11);
        if (d->vig[i] > 0.f && seed < 0.02 + 0.09 * band) { d->mote_i[d->nmote] = (uint32_t)i; d->mote_seed[d->nmote] = (float)seed; d->mote_s97[d->nmote] = (float)(seed * 97.0); d->mote_band[d->nmote++] = band; }
    }
    return d;
}

void cd_destroy(cd_t *d) {
    if (!d) return;
    CD_FREE(d->field); CD_FREE(d->z); CD_FREE(d->own); CD_FREE(d->region); CD_FREE(d->eye); CD_FREE(d->on); CD_FREE(d->pri);
    CD_FREE(d->halo); CD_FREE(d->rgb); CD_FREE(d->cdots); CD_FREE(d->ccol);
    CD_FREE(d->vig); CD_FREE(d->row_zz); CD_FREE(d->row_near); CD_FREE(d->mote_i); CD_FREE(d->mote_seed); CD_FREE(d->mote_band);
    CD_FREE(d->mote_s97); CD_FREE(d->mote_ph); CD_FREE(d->mote_rgb); CD_FREE(d->frgb); CD_FREE(d->fon); CD_FREE(d->fcell); CD_FREE(d->fccol); CD_FREE(d->touch);
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
    e->i00 = s11 / det; e->i01 = -s01 / det; e->i11 = s00 / det;
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

static void set_dot(cd_t *d, int i, uint32_t col, int p) {
    if (p < d->pri[i]) return;
    d->on[i] = 1; d->pri[i] = (uint8_t)p; d->rgb[i] = col;
}

/* ── the frame ─────────────────────────────────────────────────────────── */

static void grow(box_t *acc, box_t b) {
    if (b.x0 < acc->x0) acc->x0 = b.x0;
    if (b.y0 < acc->y0) acc->y0 = b.y0;
    if (b.x1 > acc->x1) acc->x1 = b.x1;
    if (b.y1 > acc->y1) acc->y1 = b.y1;
}

static void region_edge(cd_t *d, uint32_t col, int p, bool keep_lines) {
    for (int Y = d->ry0; Y <= d->ry1; Y++) for (int X = d->rx0; X <= d->rx1; X++) {
        const int i = Y * d->DW + X;
        if (!d->region[i]) continue;
        bool edge = X == 0 || Y == 0 || X == d->DW - 1 || Y == d->DH - 1 || !d->region[i - 1] || !d->region[i + 1] || !d->region[i - d->DW] || !d->region[i + d->DW];
        if (edge && !(keep_lines && d->pri[i] >= 2)) set_dot(d, i, col, p);
    }
}
/* clear the last region (its box), and make b the next one's */
static void clear_region(cd_t *d) {
    for (int Y = d->ry0; Y <= d->ry1 && d->rx1 >= d->rx0; Y++) memset(d->region + Y * d->DW + d->rx0, 0, (size_t)(d->rx1 - d->rx0 + 1));
    d->rx0 = d->ry0 = 0; d->rx1 = d->ry1 = -1;
}
static void region_box(cd_t *d, int x0, int y0, int x1, int y1) { d->rx0 = x0; d->ry0 = y0; d->rx1 = x1; d->ry1 = y1; }
static void disc_region(cd_t *d, const float *world, const cc_shape_t *s, bool facing) {
    clear_region(d);
    ell_t e;
    if (!ellipse(d, world, s, &e)) return;
    if (facing) {
        float l = sqrtf(dotv(e.A[2], e.A[2])); if (l <= 0.f) l = 1.f;
        if (dotv(e.A[2], d->cam.d) / l < 0.15f) return;
    }
    const box_t b = sbox(d, e.u, e.v, e.eu, e.ev);
    region_box(d, b.x0, b.y0, b.x1, b.y1);
    for (int Y = b.y0; Y <= b.y1; Y++) for (int X = b.x0; X <= b.x1; X++) {
        const int i = Y * d->DW + X; const float q = eq(&e, dot_u(d, X), dot_v(d, Y));
        if (q < 1.f && e.d + e.h * sqrtf(1.f - q) >= d->z[i] - 0.02f) d->region[i] = 1;
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

void cd_draw(cd_t *d, const cc_t *c, uint32_t frame) {
    uint32_t _pc = cycles();
#define PROF_STAGE(n) do { const uint32_t _t = cycles(); d->prof[n] += _t - _pc; _pc = _t; } while (0)
    const float *world = cc_world(c);
    const cc_shape_t *sh; const int ns = cc_shapes(c, &sh);
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

    /* the creature's box this frame (every primitive's bounds, plus room for lines and the halo); clear it and last frame's */
    box_t bb = { DW, DW, DH, DH }; bb.x0 = DW; bb.x1 = -1; bb.y0 = DH; bb.y1 = -1;
    for (int q = 0; q < ns; q++) {
        const cc_shape_t *s = &sh[q];
        d->pok[q] = 0;
        if (s->kind == CC_BLOB || s->kind == CC_ELL || s->kind == CC_PATCH) {
            ell_t *e = &d->pe[q]; if (!ellipse(d, world, s, e)) continue;
            d->pok[q] = 1;
            const float sf = d->sf[q];
            grow(&bb, sbox(d, e->u, e->v, e->eu / sf, e->ev / sf));
        } else {
            capz_t *cp = &d->pc[q];
            if (s->kind == CC_CONE) cone_capsule(d, world, s, cp); else capsule(d, world, s, s->r[0], s->r[1], cp);
            d->pok[q] = 1;
            capz_t w = *cp;
            if (s->group == CG_WHISKER) w.r0 = w.r1 = 0.55f * d->dsx;
            grow(&bb, cap_box(d, &w));
        }
    }
    const int M = 4;
    box_t nb = { bb.x0 - M, bb.x1 + 1 + M, bb.y0 - M, bb.y1 + 1 + M };
    if (nb.x0 < 0) nb.x0 = 0;
    if (nb.y0 < 0) nb.y0 = 0;
    if (nb.x1 > DW) nb.x1 = DW;
    if (nb.y1 > DH) nb.y1 = DH;
    if (bb.x1 < 0) nb = (box_t){ 0, 0, 0, 0 };
    {   /* clear the union of last frame's box and this one's */
        const int x0 = nb.x0 < d->bx0 || d->bx1 <= d->bx0 ? nb.x0 : d->bx0, x1 = nb.x1 > d->bx1 ? nb.x1 : d->bx1;
        const int y0 = nb.y0 < d->by0 || d->by1 <= d->by0 ? nb.y0 : d->by0, y1 = nb.y1 > d->by1 ? nb.y1 : d->by1;
        const size_t w = (size_t)(x1 > x0 ? x1 - x0 : 0);
        for (int Y = y0; Y < y1 && w; Y++) {
            const int i = Y * DW + x0;
            memset(d->field + i, 0, sizeof(float) * w); memset(d->own + i, -1, w); memset(d->region + i, 0, w); memset(d->eye + i, 0, w);
            memset(d->on + i, 0, w); memset(d->pri + i, 0, w); memset(d->halo + i, 0, w); memset(d->rgb + i, 0, sizeof(uint32_t) * w);
            for (size_t q = 0; q < w; q++) d->z[i + q] = ZNONE;
        }
    }
    d->bx0 = nb.x0; d->bx1 = nb.x1; d->by0 = nb.y0; d->by1 = nb.y1;
    d->rx0 = d->ry0 = 0; d->rx1 = d->ry1 = -1;

    PROF_STAGE(0);
    /* 1. the merged body: a metaball field, the nearest element surface per dot */
    for (int q = 0; q < ns; q++) {
        const cc_shape_t *s = &sh[q];
        if (s->kind != CC_BLOB || !d->pok[q]) continue;
        const ell_t e = d->pe[q];
        const float sf = d->sf[q], kk = d->kk[q];
        const box_t b = sbox(d, e.u, e.v, e.eu / sf, e.ev / sf);
        const float top = e.d + e.h;
        const float sx = d->dsx, a2 = e.i00 * sx * sx;          /* q along a row is a quadratic in X: stepped by adds */
        const float ikk = 1.f / kk, h2 = 0.5f / e.i00, isx = 1.f / sx;   /* the row's run: one divide a blob, not a row */
        for (int Y = b.y0; Y <= b.y1; Y++) {
            const float dv = dot_v(d, Y) - e.v, cdv = e.i11 * dv * dv, lin = 2.f * e.i01 * dv;
            float *fr = d->field + Y * DW, *zr = d->z + Y * DW; int8_t *orow = d->own + Y * DW;
            /* only the run of the row inside the field: i00 du^2 + lin du + cdv < 1/kk, solved for du */
            const float disc = lin * lin - 4.f * e.i00 * (cdv - ikk);
            if (disc <= 0.f) continue;
            const float sq = sqrtf(disc), u0 = (-lin - sq) * h2 + e.u, u1 = (-lin + sq) * h2 + e.u;
            int xa = (int)floorf(u0 * isx + DW / 2 - 0.5f), xb = (int)ceilf(u1 * isx + DW / 2 - 0.5f);
            if (xa < b.x0) xa = b.x0;
            if (xb > b.x1) xb = b.x1;
            float du = dot_u(d, xa) - e.u;
            float qv = (e.i00 * du + lin) * du + cdv, dq = (2.f * e.i00 * du + lin) * sx + a2;
            for (int X = xa; X <= xb; X++, qv += dq, dq += 2.f * a2) {
                const float qf = qv * kk;
                if (qf >= 1.f) continue;
                const float o = 1.f - qf;
                fr[X] += s->st * o * o * o;
                if (top <= zr[X]) continue;                 /* cannot be nearer than what is there */
                const float zz = e.d + (qv < 1.f ? e.h * sqrtf(1.f - qv) : 0.f);
                if (zz > zr[X]) { zr[X] = zz; orow[X] = (int8_t)s->group; }
            }
        }
    }
    for (int Y = d->by0; Y < d->by1; Y++) for (int X = d->bx0; X < d->bx1; X++) {
        const int i = Y * DW + X;
        if (d->field[i] < THRESH) { d->z[i] = ZNONE; d->own[i] = -1; }
    }
    PROF_STAGE(1);
    /* 2. the solid parts, nearest wins */
    for (int q = 0; q < ns; q++) {
        const cc_shape_t *s = &sh[q];
        if (s->kind == CC_BLOB || s->kind == CC_PATCH || s->group >= CG_NSTRUCT) continue;
        if (!d->pok[q]) continue;
        if (s->kind == CC_ELL) {
            const ell_t e = d->pe[q];
            const box_t b = sbox(d, e.u, e.v, e.eu, e.ev);
            for (int Y = b.y0; Y <= b.y1; Y++) for (int X = b.x0; X <= b.x1; X++) {
                const int i = Y * DW + X; const float qv = eq(&e, dot_u(d, X), dot_v(d, Y));
                if (qv < 1.f) { const float zz = e.d + e.h * sqrtf(1.f - qv); if (zz > d->z[i]) { d->z[i] = zz; d->own[i] = (int8_t)s->group; } }
            }
        } else {
            const capz_t cp = d->pc[q];
            const box_t b = cap_box(d, &cp);
            for (int Y = b.y0; Y <= b.y1; Y++) for (int X = b.x0; X <= b.x1; X++) {
                const int i = Y * DW + X; float zz;
                if (in_capsule(&cp, dot_u(d, X), dot_v(d, Y), &zz) && zz > d->z[i]) { d->z[i] = zz; d->own[i] = (int8_t)s->group; }
            }
        }
    }
    PROF_STAGE(2);
    /* 3. lines */
    float zlo = 1e30f, zhi = -1e30f;
    for (int Y = d->by0; Y < d->by1; Y++) for (int X = d->bx0; X < d->bx1; X++) {
        const int i = Y * DW + X;
        if (d->own[i] >= 0 && d->own[i] < CG_BOWL) { zlo = minf(zlo, d->z[i]); zhi = maxf(zhi, d->z[i]); }
    }
    float hip[2] = { 0, 0 };
    for (int s = 0; s < 2; s++) if (d->thigh[s] >= 0) {
        const float o[3] = { world[d->thigh[s] * 12 + 3], world[d->thigh[s] * 12 + 7], world[d->thigh[s] * 12 + 11] }; float p[3];
        proj(d, o, p); hip[s] = p[1];
    }
    float body[3]; unpack(d->col_body, body);
    const float izr = zhi > zlo ? 1.f / (zhi - zlo) : 0.f;
    for (int Y = d->by0; Y < d->by1; Y++) for (int X = d->bx0; X < d->bx1; X++) {
        const int i = Y * DW + X, g = d->own[i];
        if (g < 0) continue;
        bool line = false;
        static const int DX[4] = { 1, -1, 0, 0 }, DY[4] = { 0, 0, 1, -1 };
        for (int n = 0; n < 4 && !line; n++) {
            const int x = X + DX[n], y = Y + DY[n];
            if (x < 0 || y < 0 || x >= DW || y >= DH) { line = true; break; }
            const int j = y * DW + x, h = d->own[j];
            if (h < 0) { line = true; break; }
            if (h == g || g == CG_TORSO) continue;
            if (d->z[i] <= d->z[j] + 0.01f) continue;
            if ((g == CG_HINDL || g == CG_HINDR) && h == CG_TORSO && dot_v(d, Y) > hip[g == CG_HINDL ? 0 : 1]) continue;
            line = true;
        }
        if (!line) continue;
        if (g == CG_BOWL || g == CG_YARN) { set_dot(d, i, d->col_bowl, 2); continue; }
        const float t = zhi > zlo ? clampf((d->z[i] - zlo) * izr, 0.f, 1.f) : 1.f;
        float col[3];
        for (int q = 0; q < 3; q++) col[q] = minf(1.f, body[q] * (0.5f + 0.5f * t) + (1.f - body[q]) * 0.45f * t * t);
        set_dot(d, i, pack(col), 2);
    }
    PROF_STAGE(3);
    /* 4. features */
    float iris[3]; unpack(d->col_iris, iris);
    const float pup[3] = { iris[0] * 0.75f + 0.25f, iris[1] * 0.75f + 0.25f, iris[2] * 0.75f + 0.25f };
    const uint32_t col_pupil = pack(pup);
    for (int q = 0; q < ns; q++) {
        const cc_shape_t *s = &sh[q];
        if (s->group != CG_EYE || s->eye != CC_IRIS) continue;
        disc_region(d, world, s, true);
        const int ex0 = d->rx0, ey0 = d->ry0, ex1 = d->rx1, ey1 = d->ry1;
        for (int Y = ey0; Y <= ey1; Y++) memcpy(d->eye + Y * DW + ex0, d->region + Y * DW + ex0, (size_t)(ex1 - ex0 + 1));
        region_edge(d, d->col_iris, 4, false);
        for (int p = 0; p < ns; p++) {
            const cc_shape_t *t = &sh[p];
            if (t->group != CG_EYE || t->node != s->node || t->eye == CC_IRIS) continue;
            disc_region(d, world, t, true);
            for (int Y = d->ry0; Y <= d->ry1; Y++) for (int X = d->rx0; X <= d->rx1; X++) {
                const int i = Y * DW + X;
                if (d->region[i] && d->eye[i]) set_dot(d, i, t->eye == CC_SHINE ? d->col_shine : col_pupil, t->eye == CC_SHINE ? 6 : 5);
            }
        }
        for (int Y = ey0; Y <= ey1; Y++) memset(d->eye + Y * DW + ex0, 0, (size_t)(ex1 - ex0 + 1));
    }
    for (int q = 0; q < ns; q++) {
        const cc_shape_t *s = &sh[q];
        if (s->group == CG_NOSE || s->group == CG_TONGUE) {
            disc_region(d, world, s, false);
            for (int Y = d->ry0; Y <= d->ry1; Y++) for (int X = d->rx0; X <= d->rx1; X++) { const int i = Y * DW + X; if (d->region[i]) set_dot(d, i, d->col_nose, 5); }
        } else if (s->group == CG_MAW) { disc_region(d, world, s, false); region_edge(d, d->col_nose, 4, false); }
        else if (s->group == CG_MILK) { disc_region(d, world, s, false); region_edge(d, d->col_belly, 3, false); }
        else if (s->group == CG_EARIN) {
            clear_region(d);
            capz_t cp; cone_capsule(d, world, s, &cp);
            if (cp.facing < 0.1f) continue;
            const box_t b = cap_box(d, &cp);
            region_box(d, b.x0, b.y0, b.x1, b.y1);
            for (int Y = b.y0; Y <= b.y1; Y++) for (int X = b.x0; X <= b.x1; X++) {
                const int i = Y * DW + X; float zz;
                if (in_capsule(&cp, dot_u(d, X), dot_v(d, Y), &zz) && zz >= d->z[i] - 0.02f) d->region[i] = 1;
            }
            region_edge(d, d->col_ear, 4, false);
        } else if (s->kind == CC_PATCH) {          /* the white chest: a region of the body's own surface */
            clear_region(d);
            float C[3], A[3][3], inv[9]; bool ok;
            xf(world, s->node, s->p, C);
            for (int kq = 0; kq < 3; kq++) { const float v[3] = { s->rq[kq] * s->r[kq], s->rq[3 + kq] * s->r[kq], s->rq[6 + kq] * s->r[kq] }; xv(world, s->node, v, A[kq]); }
            inv3(A, inv, &ok);
            if (!ok) continue;
            region_box(d, d->bx0, d->by0, d->bx1 - 1, d->by1 - 1);
            for (int Y = d->by0; Y < d->by1; Y++) for (int X = d->bx0; X < d->bx1; X++) {
                const int i = Y * DW + X;
                if (d->own[i] != CG_TORSO) continue;
                const float uu = dot_u(d, X), vv = dot_v(d, Y), zz = d->z[i];
                const float P[3] = { k->c[0] + uu * k->r[0] + vv * k->u[0] + zz * k->d[0], uu * k->r[1] + vv * k->u[1] + zz * k->d[1], k->c[2] + uu * k->r[2] + vv * k->u[2] + zz * k->d[2] };
                const float qv[3] = { P[0] - C[0], P[1] - C[1], P[2] - C[2] };
                float l2 = 0; for (int r = 0; r < 3; r++) { const float l = inv[r * 3] * qv[0] + inv[r * 3 + 1] * qv[1] + inv[r * 3 + 2] * qv[2]; l2 += l * l; }
                if (l2 < 1.f) d->region[i] = 1;
            }
            region_edge(d, d->col_belly, 3, true);
        }
    }
    float wh[3]; unpack(d->col_whisker, wh);
    for (int q = 0; q < 3; q++) wh[q] = minf(1.f, wh[q] * 1.3f);
    const uint32_t col_wh = pack(wh);
    for (int q = 0; q < ns; q++) {                 /* whiskers last: fine lines where nothing else is drawn */
        const cc_shape_t *s = &sh[q];
        if (s->group != CG_WHISKER) continue;
        capz_t cp; capsule(d, world, s, 0, 0, &cp); cp.r0 = cp.r1 = 0.55f * d->dsx;
        const box_t b = cap_box(d, &cp);
        for (int Y = b.y0; Y <= b.y1; Y++) for (int X = b.x0; X <= b.x1; X++) {
            const int i = Y * DW + X; float zz;
            if (d->on[i]) continue;
            if (in_capsule(&cp, dot_u(d, X), dot_v(d, Y), &zz) && (d->own[i] < 0 || zz >= d->z[i] - 0.05f)) set_dot(d, i, col_wh, 1);
        }
    }
    PROF_STAGE(4);
    /* 5. the world: kept off the creature by a halo (a disc of radius sqrt 5: two dots across within a row of it,
     * one dot across two rows away), as two separable passes over the box */
    {
        /* the box keeps 4 dots of margin round the creature (cleared), so the passes read inside it without bounds checks */
        const int x0 = d->bx0, x1 = d->bx1, y0 = d->by0, y1 = d->by1;
        uint8_t *A = d->eye, *B = d->region;                /* scratch: free again by now (cleared with the box) */
        for (int Y = y0; Y < y1; Y++) {
            const int8_t *o = d->own + Y * DW; const uint8_t *n = d->on + Y * DW; uint8_t *m = d->halo + Y * DW;
            for (int X = x0; X < x1; X++) m[X] = (uint8_t)((o[X] >= 0) | n[X]);
        }
        for (int Y = y0; Y < y1; Y++) {
            const uint8_t *m = d->halo + Y * DW; uint8_t *a2 = A + Y * DW, *a1 = B + Y * DW;
            for (int X = x0 + 2; X < x1 - 2; X++) { const uint8_t c = m[X - 1] | m[X] | m[X + 1]; a1[X] = c; a2[X] = c | m[X - 2] | m[X + 2]; }
        }
        for (int Y = y0 + 2; Y < y1 - 2; Y++) {
            uint8_t *h = d->halo + Y * DW; const uint8_t *a = A + Y * DW, *b2 = B + Y * DW;
            uint8_t *tc = d->touch + (Y >> 2) * d->W;
            for (int X = x0 + 2; X < x1 - 2; X++) { const uint8_t v = a[X - DW] | a[X] | a[X + DW] | b2[X - 2 * DW] | b2[X + 2 * DW]; h[X] = v; tc[X >> 1] |= v; }
        }
        for (int Y = y0; Y < y1; Y++) { memset(A + Y * DW + x0, 0, (size_t)(x1 - x0)); memset(B + Y * DW + x0, 0, (size_t)(x1 - x0)); }
    }
    PROF_STAGE(5);
    const float scroll = (k->c[0] * k->r[0] + k->c[2] * k->r[2]) / d->dsx, horizon = d->horizon;
    static const float FLR[3] = { 0x8f / 255.f, 0x87 / 255.f, 0xc4 / 255.f }, TEX[3] = { 0x6c / 255.f, 0x65 / 255.f, 0x98 / 255.f },
                       DIM[3] = { 0x5c / 255.f, 0x56 / 255.f, 0x88 / 255.f }, MOTE[3] = { 0xb4 / 255.f, 0xac / 255.f, 0xec / 255.f };
    /* the floor layer: redrawn only when the camera moved along it (walking), else kept */
    const bool floor_new = !d->fvalid || fabsf(scroll - d->fscroll) > 1e-4f;
    if (floor_new) {
        d->fvalid = true; d->fscroll = scroll;
        for (int Y = (int)horizon + 1; Y < DH; Y++) {
            if (Y <= horizon) continue;
            const float zz = d->row_zz[Y], near = d->row_near[Y], lim = 13.f - 0.55f * zz;
            const uint32_t thr = prob_u(0.16f + 0.1f * (1.f - near));
            const int kk = (int)floorf(14.f / zz + 0.5f);
            const float x0 = (0.5f - DW / 2.f) * zz + scroll;
            uint32_t *fr = d->frgb + Y * DW; uint8_t *fo = d->fon + Y * DW; const float *vg = d->vig + Y * DW;
            for (int X = 0; X < DW; X++) {
                fr[X] = 0; fo[X] = 0;
                const float vig = vg[X];
                if (vig <= 0.f) continue;
                const float uw = x0 + X * zz;
                const float lane = fabsf(uw - 26.f * floorf(uw * (1.f / 26.f)) - 13.f);
                float a = 0.f; const float *col = NULL;
                if (lane > lim) { a = 0.85f * near; col = FLR; }
                else if (hash3u((int)floorf(uw * 0.625f), kk, 7) < thr) { a = (0.5f + 0.25f * near) * (0.6f + 0.4f * hash3f((int)floorf(uw), Y, 3)); col = TEX; }
                if (a > 0.f) {
                    const float kv = minf(1.f, a * vig), cc3[3] = { col[0] * kv, col[1] * kv, col[2] * kv };
                    fr[X] = pack(cc3); fo[X] = kv > 0.12f;
                }
            }
        }
    }
    static const uint8_t BIT[8] = { 0, 1, 2, 6, 3, 4, 5, 7 };   /* dot (col << 2 | row) -> braille bit */
    const int NC = d->W * d->H;
    if (floor_new) {                                              /* the floor layer's own cells, made with it */
        memset(d->fcell, 0, (size_t)NC); memset(d->fccol, 0, sizeof(uint32_t) * (size_t)NC);
        for (int Y = (int)horizon + 1; Y < DH; Y++) {
            const int j = Y >> 2, r = Y & 3; const uint8_t *fo = d->fon + Y * DW; const uint32_t *fr = d->frgb + Y * DW;
            for (int X = 0; X < DW; X++) {
                if (!fo[X]) continue;
                const int k = j * d->W + (X >> 1);
                d->fcell[k] |= (uint8_t)(1 << BIT[(X & 1) << 2 | r]);
                const uint32_t v = fr[X], w = d->fccol[k];
                if ((v >> 16 & 255) + (v >> 8 & 255) + (v & 255) > (w >> 16 & 255) + (w >> 8 & 255) + (w & 255)) d->fccol[k] = v;
            }
        }
    }
    PROF_STAGE(6);
    /* cells: the floor's cells as they are, then the cells under the creature's box made again (its dots own their cell;
     * the floor there only outside the halo); a cell's colour is its highest priority dot's, the brightest among those */
    uint8_t *cat = d->eye;                                        /* scratch: W*H bytes (free by now) */
    int8_t *bp = (int8_t *)d->region; uint16_t *bl = (uint16_t *)d->field;   /* scratch: best priority, its brightness */
    memcpy(d->cdots, d->fcell, (size_t)NC); memcpy(d->ccol, d->fccol, sizeof(uint32_t) * (size_t)NC);
    const int cx0 = d->bx0 >> 1, cx1 = (d->bx1 + 1) >> 1, cy0 = d->by0 >> 2, cy1 = (d->by1 + 3) >> 2;
    for (int j = cy0; j < cy1; j++) for (int i = cx0; i < cx1 && i < d->W; i++) { const int k = j * d->W + i; d->cdots[k] = 0; d->ccol[k] = 0; bp[k] = -1; }
    for (int Y = cy0 * 4; Y < cy1 * 4 && Y < DH; Y++) {
        const int j = Y >> 2, r = Y & 3;
        const uint8_t *on = d->on + Y * DW, *pr = d->pri + Y * DW, *ha = d->halo + Y * DW, *fo = d->fon + Y * DW, *tc = d->touch + j * d->W;
        const uint32_t *rgb = d->rgb + Y * DW, *fr = d->frgb + Y * DW;
        for (int X = cx0 * 2; X < cx1 * 2 && X < DW; X++) {
            if (!tc[X >> 1]) {                                    /* nothing of the creature in this cell: the floor's cell as made */
                if (!(X & 1)) { const int k = j * d->W + (X >> 1); d->cdots[k] = d->fcell[k]; d->ccol[k] = d->fccol[k]; }
                X++; continue;
            }
            int p; uint32_t v;
            if (on[X]) { p = pr[X]; v = rgb[X]; }
            else if (!ha[X] && fo[X]) { p = 0; v = fr[X]; }
            else continue;
            const int k = j * d->W + (X >> 1), bit = 1 << BIT[(X & 1) << 2 | r];
            d->cdots[k] |= (uint8_t)bit;
            if (p >= 1) cat[k] |= (uint8_t)bit;
            const uint16_t l = (uint16_t)((v >> 16 & 255) + (v >> 8 & 255) + (v & 255));
            if (p > bp[k] || (p == bp[k] && l > bl[k])) { bp[k] = (int8_t)p; bl[k] = l; d->ccol[k] = v; }
        }
    }
    /* the dust: only its motes (made once), where neither the creature's halo nor the floor is; a cell with the creature
     * in it keeps only the creature's dots */
    const float f3 = (float)(frame % 3000000u) * (1.f / 3.f);
    for (int m = 0; m < d->nmote; m++) {
        const int i = (int)d->mote_i[m];
        if (d->halo[i] || d->frgb[i] || d->on[i]) continue;
        const int X = i % DW, Y = i / DW, k = (Y >> 2) * d->W + (X >> 1);
        if (cat[k]) continue;
        const int ph = (int)floorf(f3 + d->mote_s97[m]);
        if (ph != d->mote_ph[m]) {                                /* its twinkle moved on: every third frame or so */
            d->mote_ph[m] = ph;
            const float tw = hash3f(X, Y, ph);
            const float a = (0.3f + 0.55f * tw) * (0.55f + 0.45f * d->mote_band[m]), kv = minf(1.f, a * d->vig[i]);
            const float *col = tw > 0.8f ? MOTE : DIM, cc3[3] = { col[0] * kv, col[1] * kv, col[2] * kv };
            d->mote_rgb[m] = kv > 0.12f ? pack(cc3) : 0;
        }
        if (!d->mote_rgb[m]) continue;
        const uint32_t v = d->mote_rgb[m], w = d->ccol[k];
        d->cdots[k] |= (uint8_t)(1 << BIT[(X & 1) << 2 | (Y & 3)]);
        if (!w || (v >> 16 & 255) + (v >> 8 & 255) + (v & 255) > (w >> 16 & 255) + (w >> 8 & 255) + (w & 255)) d->ccol[k] = v;
    }
    for (int j = cy0; j < cy1; j++) for (int i = cx0; i < cx1 && i < d->W; i++) {
        const int k = j * d->W + i;
        if (cat[k]) d->cdots[k] = cat[k];                         /* a cell with the creature in it shows only its dots */
        if (!d->cdots[k]) d->ccol[k] = 0;
        cat[k] = 0; bp[k] = 0; bl[k] = 0; d->touch[k] = 0;       /* the scratch back to clear */
    }
    PROF_STAGE(7);
}

void cd_cells(const cd_t *d, const uint8_t **dots, const uint32_t **rgb) { *dots = d->cdots; *rgb = d->ccol; }
void cd_dots(const cd_t *d, const uint8_t **on, const uint32_t **rgb, const uint8_t **pri) { *on = d->on; *rgb = d->rgb; *pri = d->pri; }
void cd_prof(cd_t *d, uint32_t *out) { for (int i = 0; i < CD_STAGES; i++) { out[i] += d->prof[i]; d->prof[i] = 0; } }
void cd_box(const cd_t *d, int *x0, int *y0, int *x1, int *y1) { *x0 = d->bx0; *y0 = d->by0; *x1 = d->bx1; *y1 = d->by1; }
