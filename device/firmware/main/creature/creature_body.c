/*
 * SPDX-License-Identifier: MIT
 *
 * creature_body.c: a creature package's body drawn into the fluid (creature_body.h).
 * The rig is molty2-runtime.mjs build(); the camera is renderer.mjs frame() and lens();
 * the raster is js/fluid/targets.js CreatureTarget.update; the cell style is renderer.mjs
 * style('dither') with its TUNE; the stamp is renderer.mjs stamp(); the fill is
 * CreatureTarget.fill plus holes().
 */
#include "creature_body.h"
#include "rig_geo.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define CB_CALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM)
#define CB_FREE(p)   heap_caps_free(p)
/* the vertex buffer (a few KB, rewritten for every part) in internal DRAM while the fluid's
 * 32 KB reserve stays free, else PSRAM */
static void *calloc_fast(size_t n) {
    void *p = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= n + 32 * 1024 ? heap_caps_calloc(1, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) : NULL;
    return p ? p : CB_CALLOC(n);
}
#else
#define CB_CALLOC(n) calloc(1, (n))
#define CB_FREE(p)   free(p)
#define calloc_fast(n) CB_CALLOC(n)
#endif

#define PI_F 3.14159265f
#define FIT  0.8f                 /* the body fills 80 % of the glass's radius            */
#define AMB  0.4f                 /* CreatureTarget's ambient                             */
#define KEY  1.15f                /* the lab's key (renderer.mjs enter, a soft surface)   */
static const float VIEW[3] = { 0.16f, 0.2f, 1.f };        /* TUNE.yaw, TUNE.pitch         */
static const float LIGHT[3] = { -0.75f, 1.f, 0.55f };     /* TUNE.light (dither)          */
/* style('dither'): TUNE floor low span sat shadow hot edge glare */
#define S_FLOOR 0.32f
#define S_LOW   0.1f
#define S_SPAN  0.68f
#define S_SAT   1.5f
#define S_SHADOW 0.55f
#define S_HOT   0.55f
#define S_EDGE  0.7f
#define S_GLARE 0.74f

static inline float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
/* on the S3, fminf / fmaxf are newlib calls (with NaN checks) and a float divide is a libgcc
 * call, ~10x a multiply: the per-triangle and per-cell loops use these and reciprocals */
static inline float minf(float a, float b) { return a < b ? a : b; }
static inline float maxf(float a, float b) { return a > b ? a : b; }

/* lod: an ellipsoid small enough that its facets are under a sample also gets a coarser
 * mesh with facets of about LOD_PX samples, drawn while the camera is not zoomed in: the
 * same picture for fewer triangles (an eye's glint is 250 facets in a few samples) */
#define LOD_PX 1.5f
typedef struct { geo_t g, lod; int16_t bone; int16_t pal; uint8_t rgb[3]; m34_t local; } mesh_t;

/* the meshes' arrays, each kept once: parts of one shape share their vertices and indices
 * (an ellipsoid is a unit sphere scaled by its part's matrix; the eight tentacles' tubes
 * share their indices), so a frame streams far fewer bytes through the cache */
#define POOL_MAX (CP_PARTS_MAX * 4)
typedef struct { void *p[POOL_MAX]; size_t n[POOL_MAX]; int np; } pool_t;

struct cb {
    int nb, nm, focus_bone;
    m34_t world[CP_BONES_MAX + 1];      /* bones, then the root                         */
    mesh_t mesh[CP_PARTS_MAX];
    pool_t pool, lods;
    float cam_c[3], cam_x[3], cam_y[3], cam_z[3], half;
    int W, H, tris, lod_w;
    float *zbuf; uint32_t *cbuf; float *vbuf; int vcap; uint8_t tag;
    float *cov, *lum, *cr, *cg, *cb; uint8_t *hole;
    int d0x, d0y, d1x, d1y;             /* the cells written last frame (body + marks)  */
};

/* ── the rig: rest + outputs, parent first ─────────────────────────────────── */

static m34_t trs(const float *p, const float *r, const float *s) {
    m34_t m; float R[9];
    rg_euler(R, r, 0);   /* three's default 'XYZ' */
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) m.m[i * 4 + j] = R[i * 3 + j] * (s ? s[j] : 1.f);
        m.m[i * 4 + 3] = p[i];
    }
    return m;
}

static void pose(cb_t *b, const cp_t *c, bool rest) {
    const cp_body_t *B = cp_body(c);
    static const float ZERO[9] = { 0, 0, 0, 0, 0, 0, 1, 1, 1 };
    const float *o = rest ? ZERO : cp_bone_out(c, B->nbones);
    const float zp[3] = { o[3], o[4], o[5] }, zr[3] = { o[0], o[1], o[2] };
    b->world[b->nb] = trs(zp, zr, o + 6);
    for (int i = 0; i < B->nbones; i++) {
        const cp_bone_t *bn = &B->bones[i];
        const float *a = rest ? ZERO : cp_bone_out(c, i);
        const float p[3] = { bn->pos[0] + a[3], bn->pos[1] + a[4], bn->pos[2] + a[5] }, r[3] = { bn->rot[0] + a[0], bn->rot[1] + a[1], bn->rot[2] + a[2] };
        m34_t l = trs(p, r, a + 6);
        b->world[i] = rg_mul(&b->world[bn->parent < 0 ? b->nb : bn->parent], &l);
    }
}

static inline float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void norm3(float *v) { float l = sqrtf(dot3(v, v)); if (l > 0) { v[0] /= l; v[1] /= l; v[2] /= l; } }
/* 1/sqrt(x) to ~1e-5 without a libm call (the hot loop's): the bit trick + two Newton steps */
static inline float rsqrt(float x) {
    union { float f; uint32_t i; } u = { x };
    u.i = 0x5f3759dfu - (u.i >> 1);
    float y = u.f;
    y *= 1.5f - 0.5f * x * y * y;
    return y * (1.5f - 0.5f * x * y * y);
}
static inline float rcp(float x) { const float r = rsqrt(x); return r * r; }   /* x > 0 */
static void cross3(float *o, const float *a, const float *b) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }

/* renderer.mjs frame(): looking from VIEW, centred on the projected bounds of the body at
 * rest (every other vertex, as the lab samples them), the furthest one at FIT of the glass */
static void fit(cb_t *b) {
    float *z = b->cam_z, *x = b->cam_x, *y = b->cam_y;
    memcpy(z, VIEW, sizeof(VIEW)); norm3(z);
    const float up[3] = { 0, 1, 0 };
    cross3(x, up, z); norm3(x); cross3(y, z, x);
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (int pass = 0; pass < 2; pass++) {
        float h = 0;
        for (int m = 0; m < b->nm; m++) {
            const mesh_t *me = &b->mesh[m];
            const m34_t W = rg_mul(&b->world[me->bone < 0 ? b->nb : me->bone], &me->local);
            for (int i = 0; i < me->g.nv; i += 2) {
                const float *v = me->g.v + i * 3;
                const v3_t p = rg_apply(&W, v[0], v[1], v[2]);
                const float q[3] = { p.x, p.y, p.z }, px = dot3(q, x) - b->cam_c[0], py = dot3(q, y) - b->cam_c[1];
                if (!pass) { x0 = minf(x0, px); x1 = maxf(x1, px); y0 = minf(y0, py); y1 = maxf(y1, py); }
                else h = maxf(h, hypotf(px, py));
            }
        }
        if (!pass) { b->cam_c[0] = (x0 + x1) / 2; b->cam_c[1] = (y0 + y1) / 2; }
        else b->half = maxf(h, 1e-3f) / FIT;
    }
}

/* *a (n bytes) as the pool's copy: an equal one already there replaces it */
static void *pooled(pool_t *pl, void *a, size_t n) {
    for (int i = 0; i < pl->np; i++) if (pl->n[i] == n && !memcmp(pl->p[i], a, n)) { CB_FREE(a); return pl->p[i]; }
    pl->p[pl->np] = a; pl->n[pl->np++] = n;
    return a;
}
static void pool_geo(pool_t *pl, geo_t *g) {
    g->v = pooled(pl, g->v, (size_t)g->nv * 3 * sizeof(float));
    g->t = pooled(pl, g->t, (size_t)g->nt * 3 * sizeof(uint16_t));
}
static void pool_free(pool_t *pl) { for (int i = 0; i < pl->np; i++) CB_FREE(pl->p[i]); pl->np = 0; }

cb_t *cb_create(const cp_t *c) {
    cb_t *b = CB_CALLOC(sizeof(cb_t));
    if (!b) return NULL;
    const cp_body_t *B = cp_body(c);
    b->nb = B->nbones;
    b->focus_bone = cp_bone_find(c, "eyeL");
    if (b->focus_bone < 0 || b->focus_bone >= B->nbones) b->focus_bone = cp_bone_find(c, "head");
    for (int i = 0; i < B->nparts; i++) {
        const cp_part_t *p = &B->parts[i];
        mesh_t *me = &b->mesh[b->nm];
        static const float ONE[3] = { 1, 1, 1 };
        const bool unit = p->type == CPS_ELLIPSOID && p->size[0] > 0 && p->size[1] > 0 && p->size[2] > 0;   /* a flat one keeps its size */
        bool ok;
        switch (p->type) {
        case CPS_ELLIPSOID: ok = rg_sphere(&me->g, p->seg[0], p->seg[1], unit ? ONE : p->size, 0, false); break;
        case CPS_BOX:       ok = rg_box(&me->g, p->size); break;
        case CPS_CONE:      ok = rg_cylinder(&me->g, 0, p->r, p->h, p->seg[0]); break;
        case CPS_CYLINDER:  ok = rg_cylinder(&me->g, p->r, p->r2, p->h, p->seg[0]); break;
        default:           ok = rg_tube(&me->g, (const float (*)[3])p->path, p->npath, p->seg[0], p->r); break;
        }
        if (!ok) { cb_destroy(b); return NULL; }
        pool_geo(&b->pool, &me->g);
        me->bone = p->bone; me->pal = p->pal; memcpy(me->rgb, p->rgb, 3);
        me->local = trs(p->pos, p->rot, unit ? p->size : NULL);
        if (me->g.nv > b->vcap) b->vcap = me->g.nv;
        b->nm++;
    }
    b->vbuf = calloc_fast((size_t)b->vcap * 3 * sizeof(float) + 16);
    if (!b->vbuf) { cb_destroy(b); return NULL; }
    pose(b, c, true);
    fit(b);
    return b;
}

void cb_destroy(cb_t *b) {
    if (!b) return;
    pool_free(&b->pool); pool_free(&b->lods);
    CB_FREE(b->vbuf); CB_FREE(b->zbuf); CB_FREE(b->cbuf);
    CB_FREE(b->cov); CB_FREE(b->lum); CB_FREE(b->cr); CB_FREE(b->cg); CB_FREE(b->cb); CB_FREE(b->hole);
    CB_FREE(b);
}

static bool alloc_cells(cb_t *b, int W, int H) {
    if (b->W == W && b->H == H && b->cov) return true;
    CB_FREE(b->zbuf); CB_FREE(b->cbuf); CB_FREE(b->cov); CB_FREE(b->lum); CB_FREE(b->cr); CB_FREE(b->cg); CB_FREE(b->cb); CB_FREE(b->hole);
    const size_t S = (size_t)W * 2 * H * 2, N = (size_t)W * H;
    b->zbuf = CB_CALLOC(S * sizeof(float)); b->cbuf = CB_CALLOC(S * sizeof(uint32_t));
    b->cov = CB_CALLOC(N * sizeof(float)); b->lum = CB_CALLOC(N * sizeof(float));
    b->cr = CB_CALLOC(N * sizeof(float)); b->cg = CB_CALLOC(N * sizeof(float)); b->cb = CB_CALLOC(N * sizeof(float)); b->hole = CB_CALLOC(N);
    b->W = W; b->H = H; b->tag = 0; b->d0x = b->d0y = b->d1x = b->d1y = 0;
    return b->zbuf && b->cbuf && b->cov && b->lum && b->cr && b->cg && b->cb && b->hole;
}

/* the coarser meshes, once the grid is known (cb_raster's first frame on it) */
static void build_lods(cb_t *b, const cp_body_t *B, int W) {
    if (b->lod_w == W) return;
    b->lod_w = W;
    pool_free(&b->lods);
    const float px = W / b->half;                 /* samples per world unit (2W across 2 half) */
    for (int m = 0; m < b->nm; m++) {
        mesh_t *me = &b->mesh[m];
        const cp_part_t *p = &B->parts[m];
        memset(&me->lod, 0, sizeof(me->lod));
        if (p->type != CPS_ELLIPSOID) continue;
        const float r = maxf(p->size[0], maxf(p->size[1], p->size[2])) * px;
        const int w = (int)ceilf(6.2831853f * r / LOD_PX), h = (int)ceilf(3.1415927f * r / LOD_PX);
        const int lw = w < 6 ? 6 : w, lh = h < 4 ? 4 : h;
        static const float ONE[3] = { 1, 1, 1 };
        const bool unit = p->size[0] > 0 && p->size[1] > 0 && p->size[2] > 0;   /* as cb_create meshed it */
        if (lw < p->seg[0] || lh < p->seg[1]) {
            if (rg_sphere(&me->lod, lw < p->seg[0] ? lw : p->seg[0], lh < p->seg[1] ? lh : p->seg[1], unit ? ONE : p->size, 0, false)) pool_geo(&b->lods, &me->lod);
            else memset(&me->lod, 0, sizeof(me->lod));
        }
    }
}

/* ── raster: flat Lambert, 2×2 samples a cell (CreatureTarget.update) ──────── */
/*
 * Memory, not arithmetic, is the cost on the device: everything here is in PSRAM behind a
 * 32 KB data cache that core 0's frame buffer shares, so a cache line touched costs ~1 µs.
 * Hence: no per-face tables (a face is culled by its screen winding and lit from its own
 * vertices, which the transform has just read), the samples stored a cell at a time (a
 * cell's 2×2 in one run), and only the cells the body or the marks wrote are visited.
 */

/* sample (x, y) in the cell-major buffers: a cell's 2×2 samples are consecutive */
static inline int sample_row(int y, int W) { return (y >> 1) * W * 4 + (y & 1) * 2; }
static inline int sample_at(int row, int x) { return row + 2 * x - (x & 1); }

static void style(cb_t *b, int x0, int y0, int x1, int y1, float lo, float span);

void cb_raster(cb_t *b, const cp_t *c, int W, int H, float zoom, float focus) {
    if (!b || !alloc_cells(b, W, H)) return;
    FLUID_PROF_BEGIN(c0);
    build_lods(b, cp_body(c), W);
    pose(b, c, false);
    const int SW = W * 2, SH = H * 2, NS = SW * SH;
    if (++b->tag == 0) { memset(b->cbuf, 0, (size_t)NS * sizeof(uint32_t)); b->tag = 1; }
    const uint32_t tagbits = (uint32_t)b->tag << 24;
    /* the key comes round to the front as the body turns side on (renderer.mjs fill) */
    const float side = fabsf(sinf(cp_bone_out(c, b->nb)[1]));
    float L[3] = { LIGHT[0] * (1 - 0.8f * side), LIGHT[1], LIGHT[2] + 0.5f * side };
    norm3(L);
    /* the palette this frame, per depth down a chain (tints that ripple) */
    const cp_body_t *B = cp_body(c);
    uint32_t pal[4][CP_PALETTE_MAX]; float pd[4]; int npd = 0;
    const float inv_h = (1.f + zoom) / b->half, hx = 0.5f * SW * inv_h, hy = 0.5f * SH * inv_h;
    float cx = b->cam_c[0], cy = b->cam_c[1];
    if (focus > 0.f && b->focus_bone >= 0) {
        const float e[3] = { b->world[b->focus_bone].m[3], b->world[b->focus_bone].m[7], b->world[b->focus_bone].m[11] };
        cx += focus * (dot3(e, b->cam_x) - cx); cy += focus * (dot3(e, b->cam_y) - cy);
    }
    int tris = 0, sx0 = SW, sy0 = SH, sx1 = 0, sy1 = 0;     /* the samples written */
    for (int m = 0; m < b->nm; m++) {
        const mesh_t *me = &b->mesh[m];
        const geo_t *ge = zoom < 0.05f && me->lod.nv ? &me->lod : &me->g;
        const m34_t Wm = rg_mul(&b->world[me->bone < 0 ? b->nb : me->bone], &me->local);
        const float det = rg_det(&Wm);
        if (fabsf(det) < 1e-12f) continue;                        /* collapsed (scale 0) */
        uint8_t rgb[3] = { me->rgb[0], me->rgb[1], me->rgb[2] };
        if (me->pal >= 0 && cp_tinted(c)) {
            const float d = me->bone >= 0 ? B->bones[me->bone].d : 0.f;
            int k = 0;
            while (k < npd && pd[k] != d) k++;
            if (k == npd && npd < 4) { pd[npd] = d; cp_tints(c, d, pal[npd++]); }
            if (k < npd) { uint32_t v = pal[k][me->pal]; rgb[0] = (uint8_t)(v >> 16); rgb[1] = (uint8_t)(v >> 8); rgb[2] = (uint8_t)v; }
        }
        /* the camera folded into the mesh's matrix: one affine per vertex to screen x, y, depth */
        m34_t V;
        for (int k = 0; k < 3; k++) {
            const float *ax = k == 0 ? b->cam_x : k == 1 ? b->cam_y : b->cam_z, sc = k == 0 ? hx : k == 1 ? -hy : 1.f;
            const float off = k == 0 ? 0.5f * SW - cx * hx : k == 1 ? 0.5f * SH + cy * hy : 0.f;
            for (int j = 0; j < 4; j++) V.m[k * 4 + j] = sc * (ax[0] * Wm.m[j] + ax[1] * Wm.m[4 + j] + ax[2] * Wm.m[8 + j]) + (j == 3 ? off : 0.f);
        }
        /* a face's world normal is cof(M)·n (the rows' cross products), n its local one; the
         * screen area is -hx·hy·(cof(M)·n)·cam_z, so it faces the camera when sd·area < 0 */
        const float r0[3] = { Wm.m[0], Wm.m[1], Wm.m[2] }, r1[3] = { Wm.m[4], Wm.m[5], Wm.m[6] }, r2[3] = { Wm.m[8], Wm.m[9], Wm.m[10] };
        float c0[3], c1[3], c2[3];
        cross3(c0, r1, r2); cross3(c1, r2, r0); cross3(c2, r0, r1);
        const float sd = det < 0.f ? -1.f : 1.f;                  /* a mirrored mesh turns its faces inside out */
        for (int k = 0; k < 3; k++) { c0[k] *= sd; c1[k] *= sd; c2[k] *= sd; }
        float *vb = b->vbuf;
        FLUID_PROF_BEGIN(cv);
        for (int i = 0; i < ge->nv; i++) {
            const float *v = ge->v + i * 3;
            const v3_t p = rg_apply(&V, v[0], v[1], v[2]);
            vb[i * 3] = p.x; vb[i * 3 + 1] = p.y; vb[i * 3 + 2] = p.z;
        }
        FLUID_PROF_END(FP_C_VERT, cv);
        cv = fluid_cycles();
        for (int t = 0; t < ge->nt; t++) {
            const uint16_t *ix = ge->t + t * 3;
            const float *A = vb + ix[0] * 3, *Bv = vb + ix[1] * 3, *C = vb + ix[2] * 3;
            const float area = (Bv[0] - A[0]) * (C[1] - A[1]) - (C[0] - A[0]) * (Bv[1] - A[1]);
            if (area * sd >= 0.f) continue;                       /* faces away, or edge on */
            /* the sample centres (x + 0.5) inside the bounds: a facet between two is skipped here */
            const float fx0 = maxf(0.f, minf(A[0], minf(Bv[0], C[0])) - 0.5f), fx1 = minf(SW - 1.f, maxf(A[0], maxf(Bv[0], C[0])) - 0.5f);
            const float fy0 = maxf(0.f, minf(A[1], minf(Bv[1], C[1])) - 0.5f), fy1 = minf(SH - 1.f, maxf(A[1], maxf(Bv[1], C[1])) - 0.5f);
            if (fx0 > fx1 || fy0 > fy1) continue;
            int x0 = (int)fx0, y0 = (int)fy0;
            const int x1 = (int)fx1 + 1, y1 = (int)fy1 + 1;
            if ((float)x0 < fx0) x0++;
            if ((float)y0 < fy0) y0++;
            if (x0 >= x1 || y0 >= y1) continue;
            /* edge functions at the sample centres, unnormalised (signed as the area): most
             * facets are a sample or two, so the divide and the shading wait for a first hit */
            const float sg = -sd;
            const float a0x = (Bv[1] - C[1]) * sg, a0y = (C[0] - Bv[0]) * sg, a1x = (C[1] - A[1]) * sg, a1y = (A[0] - C[0]) * sg, sa = area * sg;
            const float px0 = x0 + 0.5f, py0 = y0 + 0.5f;
            float w0r = ((Bv[0] - px0) * (C[1] - py0) - (C[0] - px0) * (Bv[1] - py0)) * sg;
            float w1r = ((C[0] - px0) * (A[1] - py0) - (A[0] - px0) * (C[1] - py0)) * sg;
            float ia = 0.f; uint32_t col = 0; bool lit = false;
            for (int y = y0; y < y1; y++, w0r += a0y, w1r += a1y) {
                float w0 = w0r, w1 = w1r;
                const int row = sample_row(y, W);
                for (int x = x0; x < x1; x++, w0 += a0x, w1 += a1x) {
                    const float w2 = sa - w0 - w1;
                    if (w0 < 0.f || w1 < 0.f || w2 < 0.f) continue;
                    if (!lit) {   /* the face's world normal, toward the camera; flat Lambert, key + ambient */
                        const float *la = ge->v + ix[0] * 3, *lb = ge->v + ix[1] * 3, *lc = ge->v + ix[2] * 3;
                        const float e1[3] = { lb[0] - la[0], lb[1] - la[1], lb[2] - la[2] }, e2[3] = { lc[0] - la[0], lc[1] - la[1], lc[2] - la[2] };
                        float nl[3]; cross3(nl, e1, e2);
                        const float n[3] = { dot3(c0, nl), dot3(c1, nl), dot3(c2, nl) };
                        const float nd = dot3(n, L) * rsqrt(dot3(n, n));
                        const float shade = minf(1.f, AMB + KEY * maxf(0.f, nd));
                        col = (uint32_t)(rgb[0] * shade + 0.5f) | (uint32_t)(rgb[1] * shade + 0.5f) << 8 | (uint32_t)(rgb[2] * shade + 0.5f) << 16 | tagbits;
                        ia = 1.f / sa; lit = true; tris++;
                    }
                    const float z = (w0 * A[2] + w1 * Bv[2] + w2 * C[2]) * ia;
                    const int q = sample_at(row, x);
                    if ((b->cbuf[q] & 0xff000000u) == tagbits && z <= b->zbuf[q]) continue;
                    b->zbuf[q] = z; b->cbuf[q] = col;
                }
            }
            if (lit) { sx0 = x0 < sx0 ? x0 : sx0; sy0 = y0 < sy0 ? y0 : sy0; sx1 = x1 > sx1 ? x1 : sx1; sy1 = y1 > sy1 ? y1 : sy1; }
        }
        FLUID_PROF_END(FP_C_SAMPLES, cv);
    }
    b->tris = tris;
    FLUID_PROF_BEGIN(ca);
    /* the cells the samples fall in; last frame's other cells (body or marks) back to empty */
    const int x0 = sx0 >> 1, y0 = sy0 >> 1, x1 = sx1 > sx0 ? (sx1 + 1) >> 1 : x0, y1 = sy1 > sy0 ? (sy1 + 1) >> 1 : y0;
    for (int j = b->d0y; j < b->d1y; j++) for (int i = b->d0x; i < b->d1x; i++) {
        if (j >= y0 && j < y1 && i >= x0 && i < x1) continue;
        const int k = j * W + i;
        b->cov[k] = b->lum[k] = b->cr[k] = b->cg[k] = b->cb[k] = 0.f; b->hole[k] = 0;
    }
    b->d0x = x0; b->d0y = y0; b->d1x = x1; b->d1y = y1;
    /* per cell: coverage, mean colour, luminance; auto-exposure over the covered cells */
    static const float INV255[5] = { 0.f, 1.f / 255, 1.f / 510, 1.f / 765, 1.f / 1020 };
    float lo = 1, hi = 0;
    for (int j = y0; j < y1; j++) for (int i = x0; i < x1; i++) {
        const int k = j * W + i;
        const uint32_t *v = b->cbuf + k * 4;
        unsigned n = 0, r = 0, g = 0, bl = 0;
        for (int s = 0; s < 4; s++) {
            if ((v[s] & 0xff000000u) != tagbits) continue;
            n++; r += v[s] & 255; g += v[s] >> 8 & 255; bl += v[s] >> 16 & 255;
        }
        const float inv = INV255[n], fr = r * inv, fg = g * inv, fb = bl * inv, l = 0.2126f * fr + 0.7152f * fg + 0.0722f * fb;
        b->cov[k] = n * 0.25f; b->cr[k] = fr; b->cg[k] = fg; b->cb[k] = fb; b->lum[k] = l;
        if (n > 2) { if (l < lo) lo = l; if (l > hi) hi = l; }
    }
    style(b, x0, y0, x1, y1, lo, 1.f / maxf(0.15f, hi - lo));
    FLUID_PROF_END(FP_C_AGG, ca);
    FLUID_PROF_END(FP_C_RASTER, c0);
}

/* renderer.mjs style('dither'): very dark covered cells are holes; the rest gets a lifted
 * floor, a softer rim, saturated hues, shadows that go deeper rather than grey, and warm
 * highlights; bright colours are calmed so the glyphs do not glare. Over the body's cells
 * (every other one is empty), the luminance auto-exposed first (lo, span) */
static void style(cb_t *b, int x0, int y0, int x1, int y1, float lo, float span) {
    const int W = b->W, N = W * b->H;
    float *C = b->cov, *L = b->lum, *R = b->cr, *G = b->cg, *B = b->cb;
    for (int j = y0; j < y1; j++) for (int i = j * W + x0; i < j * W + x1; i++) {
        const float mx = maxf(R[i], maxf(G[i], B[i]));
        b->hole[i] = C[i] > 0.5f && mx < 0.09f;
        if (C[i] <= 0) continue;
        const float m = maxf(mx, 1e-4f), v = clampf((L[i] - lo) * span, 0, 1), im = rcp(m), hr = R[i] * im, hg = G[i] * im, hb = B[i] * im;
        const bool edge = i < W || i >= N - W || !C[i - 1] || !C[i + 1] || !C[i - W] || !C[i + W];
        const float z = clampf((v - S_LOW) * (1.f / S_SPAN), 0, 1);
        float u = z * z * (3 - 2 * z);
        if (edge) u *= S_EDGE;
        const float y = 0.2126f * hr + 0.7152f * hg + 0.0722f * hb, calmer = minf(1.f, S_GLARE * rcp(maxf(y, 0.01f)));
        L[i] = b->hole[i] ? 0.f : (S_FLOOR + (1 - S_FLOOR) * u) * calmer;
        const float g = (hr + hg + hb) * (1.f / 3), sr = minf(1.f, g + (hr - g) * S_SAT), sg = minf(1.f, g + (hg - g) * S_SAT), sb = minf(1.f, g + (hb - g) * S_SAT);
        const float n = maxf(maxf(sr, sg), maxf(sb, 1e-4f)), in = rcp(n), br = sr * in, bg = sg * in, bb = sb * in;
        if (u < 0.5f) {
            const float q = u / 0.5f, k = S_SHADOW + (1 - S_SHADOW) * q;
            R[i] = br * k * (0.92f + 0.08f * q); G[i] = bg * k * (0.62f + 0.38f * q); B[i] = minf(1.f, bb * k * (0.8f + 0.2f * q) + (1 - q) * 0.12f);
        } else {
            const float q = maxf(0.f, (v - 0.62f) * (1.f / 0.38f)) * S_HOT;
            R[i] = br + (1 - br) * q; G[i] = bg + (0.97f - bg) * q; B[i] = bb + (0.88f - bb) * q;
        }
        R[i] *= calmer; G[i] *= calmer; B[i] *= calmer;
    }
}

const float *cb_cov(const cb_t *b) { return b->cov; }
int cb_tris(const cb_t *b) { return b ? b->tris : 0; }
void cb_cells(const cb_t *b, const float **cov, const float **lum, const float **r, const float **g, const float **bl, const uint8_t **hole) {
    *cov = b->cov; *lum = b->lum; *r = b->cr; *g = b->cg; *bl = b->cb; *hole = b->hole;
}

void cb_glass(const cb_t *b, int bone, const float at[3], float out[3]) {
    const v3_t p = rg_apply(&b->world[bone < 0 || bone > b->nb ? b->nb : bone], at[0], at[1], at[2]);
    const float q[3] = { p.x, p.y, p.z };
    out[0] = 0.5f + 0.5f * (dot3(q, b->cam_x) - b->cam_c[0]) / b->half;
    out[1] = 0.5f - 0.5f * (dot3(q, b->cam_y) - b->cam_c[1]) / b->half;
    out[2] = p.y;
}

/* ── the glass: marks over the cells (renderer.mjs stamp) ─────────────────── */

/* the radius a mark's outline stays within: [lo, hi] of its size */
static const float MARK_LO[3] = { 0.5f, 0.26f, 0.35f }, MARK_HI[3] = { 0.5f, 0.5f, 0.65f };

static bool inside(int kind, float u, float v) {
    const float r = hypotf(u, v);
    if (kind == CP_MARK_DOT) return r < 0.5f;
    const float a = atan2f(v, u);
    if (kind == CP_MARK_STAR) { const float f = fmodf(fmodf((a - PI_F / 2) / (2 * PI_F / 5), 1.f) + 1.5f, 1.f); return r < 0.26f + 0.24f * fabsf(f - 0.5f) * 2; }
    return r < 0.5f * (1 + 0.3f * (0.6f * sinf(3 * a + 0.4f) + 0.4f * sinf(5 * a + 1.3f)));
}

/* a mark's cell joins the cells written this frame (cleared on the next) */
static void grow(cb_t *b, int i, int j) {
    if (b->d1x <= b->d0x || b->d1y <= b->d0y) { b->d0x = i; b->d0y = j; b->d1x = i + 1; b->d1y = j + 1; return; }
    if (i < b->d0x) b->d0x = i;
    if (i >= b->d1x) b->d1x = i + 1;
    if (j < b->d0y) b->d0y = j;
    if (j >= b->d1y) b->d1y = j + 1;
}

void cb_stamp(cb_t *b, const cp_mark_t *ms, int n) {
    if (!b || !b->cov || n <= 0) return;
    const int W = b->W, H = b->H;
    for (int d = 0; d < 256; d++) {          /* shallowest first; a stable order within a depth */
        int next = 256;
        for (int q = 0; q < n; q++) {
            const cp_mark_t *m = &ms[q];
            if (m->d > d && m->d < next) next = m->d;
            if (m->d != d || !(m->size > 0)) continue;
            const bool dark = m->rgb[0] < 23 && m->rgb[1] < 23 && m->rgb[2] < 23;
            if (!m->rgb[0] && !m->rgb[1] && !m->rgb[2] && m->size <= 0.05f) continue;
            const float co = cosf(m->rot), si = sinf(m->rot), reach = 0.7f * m->size;
            const int i0 = (int)floorf(((m->x - reach) / CB_WINDOW + 1) / 2 * W - 0.5f), i1 = (int)ceilf(((m->x + reach) / CB_WINDOW + 1) / 2 * W - 0.5f);
            const int j0 = (int)floorf((1 - (m->y + reach) / CB_WINDOW) / 2 * H - 0.5f), j1 = (int)ceilf((1 - (m->y - reach) / CB_WINDOW) / 2 * H - 0.5f);
            /* well inside or outside the outline's band needs no trig (the margins cover the
             * reciprocal's rounding); the band is decided exactly as before */
            const float is = 1.f / m->size, lo2 = MARK_LO[m->kind] * MARK_LO[m->kind] * 0.99f, hi2 = MARK_HI[m->kind] * MARK_HI[m->kind] * 1.01f;
            float fxs[FLUID_GRID_MAX_W];
            for (int i = i0 < 0 ? 0 : i0; i <= i1 && i < W; i++) fxs[i] = ((i + 0.5f) / W * 2 - 1) * CB_WINDOW - m->x;
            for (int j = j0 < 0 ? 0 : j0; j <= j1 && j < H; j++) {
                const float fy = (1 - (j + 0.5f) / H * 2) * CB_WINDOW - m->y;
                for (int i = i0 < 0 ? 0 : i0; i <= i1 && i < W; i++) {
                    const float fx = fxs[i], u = (fx * co + fy * si) * is, v = (-fx * si + fy * co) * is, r2 = u * u + v * v;
                    if (r2 > hi2 || (r2 >= lo2 && !inside(m->kind, (fx * co + fy * si) / m->size, (-fx * si + fy * co) / m->size))) continue;
                    const int k = j * W + i;
                    grow(b, i, j);
                    b->cov[k] = 1; b->lum[k] = dark ? 0.f : 0.85f; b->hole[k] = dark;
                    b->cr[k] = m->rgb[0] / 255.f; b->cg[k] = m->rgb[1] / 255.f; b->cb[k] = m->rgb[2] / 255.f;
                }
            }
        }
        if (next == 256) break;
        d = next - 1;
    }
}

/* ── the fluid's target (CreatureTarget.fill, then holes) ──────────────────── */

static float s_pow13[65];   /* l^1.3, 64 segments */
static inline float pow13(float l) {
    if (s_pow13[64] == 0.f) for (int i = 0; i <= 64; i++) s_pow13[i] = powf(i / 64.f, 1.3f);
    float f = clampf(l, 0, 1) * 64.f; int i = (int)f; if (i > 63) i = 63;
    return s_pow13[i] + (s_pow13[i + 1] - s_pow13[i]) * (f - i);
}

void cb_fill(cb_t *b, fluid_sim_t *s, float swell) {
    if (!b || !s || b->W != s->W || b->H != s->H) return;
    FLUID_PROF_BEGIN(c0);
    const int W = s->W, H = s->H;
    const float inv = 1.f / swell, xmax = W - 1.001f, ymax = H - 1.001f;
    for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) {
        const int k = j * W + i;
        s->tf[k] = 0;                        /* the eyes are holes in the colour, not blocks */
        if (!s->mask[k]) { s->T[k] = 0; continue; }
        float fx = ((s->xs[i] * inv + 1) * 0.5f) * W - 0.5f, fy = ((s->ys[j] * inv + 1) * 0.5f) * H - 0.5f;
        fx = clampf(fx, 0, xmax); fy = clampf(fy, 0, ymax);
        const int x0 = (int)fx, y0 = (int)fy, q = y0 * W + x0;
        if (x0 + 1 < b->d0x || x0 >= b->d1x || y0 + 1 < b->d0y || y0 >= b->d1y) { s->T[k] = 0; continue; }   /* all four empty */
        const float ax = fx - x0, ay = fy - y0;
#define BL(a) (((a)[q] * (1 - ax) + (a)[q + 1] * ax) * (1 - ay) + ((a)[q + W] * (1 - ax) + (a)[q + W + 1] * ax) * ay)
        const float c = BL(b->cov), l = BL(b->lum);
        const int qn = (int)(fy + 0.5f) * W + (int)(fx + 0.5f);
        s->T[k] = c > 0.f && !b->hole[qn] ? fq(c * (0.06f + 0.94f * pow13(l)), FQ_UNIT) : 0;
        if (c > 0.05f) {
            const float d = c >= 0.25f ? rcp(c) : 4.f;
            s->tcol[k] = fluid_pack_rgba8(BL(b->cr) * d, BL(b->cg) * d, BL(b->cb) * d, 0.f);
        }
#undef BL
    }
    FLUID_PROF_END(FP_C_FILL, c0);
}
