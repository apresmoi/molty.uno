/*
 * SPDX-License-Identifier: MIT
 *
 * rig_geo.c: the small linear algebra and the three.js geometry generators both creature
 * runtimes build their meshes with (rig_geo.h). Moved from cute_rig.c.
 */
#include "rig_geo.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define CUTE_CALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM)
#else
#define CUTE_CALLOC(n) calloc(1, (n))
#endif

#define PI_F  3.14159265f
#define TAU_F 6.28318531f

/* ── small linear algebra: 3×4 affine, row-major [r00 r01 r02 tx | r10 … ty | r20 … tz] ── */



m34_t rg_mul(const m34_t *a, const m34_t *b) {
    m34_t r;
    for (int i = 0; i < 3; i++) {
        const float *A = a->m + i * 4;
        for (int j = 0; j < 3; j++) r.m[i * 4 + j] = A[0] * b->m[j] + A[1] * b->m[4 + j] + A[2] * b->m[8 + j];
        r.m[i * 4 + 3] = A[0] * b->m[3] + A[1] * b->m[7] + A[2] * b->m[11] + A[3];
    }
    return r;
}
float rg_det(const m34_t *a) {
    const float *m = a->m;
    return m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
}
/* 3×3 rotation for one axis */
void rg_rot_axis(float *R, int axis, float a) {
    float c = cosf(a), s = sinf(a);
    memset(R, 0, 9 * sizeof(float));
    if (axis == 0) { R[0] = 1; R[4] = c; R[5] = -s; R[7] = s; R[8] = c; }
    else if (axis == 1) { R[0] = c; R[2] = s; R[4] = 1; R[6] = -s; R[8] = c; }
    else { R[0] = c; R[1] = -s; R[3] = s; R[4] = c; R[8] = 1; }
}
void rg_mat3_mul(float *o, const float *a, const float *b) {
    float t[9];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) t[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
    memcpy(o, t, sizeof(t));
}
/* three.js Euler orders: 'XYZ' → Rx·Ry·Rz (the order string read left to right) */
static const uint8_t ORDERS[6][3] = { { 0, 1, 2 }, { 1, 0, 2 }, { 2, 0, 1 }, { 0, 2, 1 }, { 1, 2, 0 }, { 2, 1, 0 } };
void rg_euler(float *R, const float *e, int order) {
    float A[9], B[9], C[9];
    const uint8_t *o = ORDERS[order];
    rg_rot_axis(A, o[0], e[o[0]]); rg_rot_axis(B, o[1], e[o[1]]); rg_rot_axis(C, o[2], e[o[2]]);
    rg_mat3_mul(R, A, B); rg_mat3_mul(R, R, C);
}
/* quaternion.setFromUnitVectors(+z, n) → rotation matrix (shortest arc) */
void rg_rot_from_z(float *R, v3_t n) {
    float r = n.z + 1.f, qx, qy, qz, qw;
    if (r < 1e-6f) { qx = 0; qy = -1; qz = 0; qw = 0; }      /* n = −z: 180° about y (three picks −from.z,0,from.x) */
    else { qx = -n.y; qy = n.x; qz = 0; qw = r; }           /* cross(z, n) = (−n.y, n.x, 0) */
    float l = sqrtf(qx * qx + qy * qy + qz * qz + qw * qw);
    qx /= l; qy /= l; qz /= l; qw /= l;
    R[0] = 1 - 2 * (qy * qy + qz * qz); R[1] = 2 * (qx * qy - qz * qw); R[2] = 2 * (qx * qz + qy * qw);
    R[3] = 2 * (qx * qy + qz * qw); R[4] = 1 - 2 * (qx * qx + qz * qz); R[5] = 2 * (qy * qz - qx * qw);
    R[6] = 2 * (qx * qz - qy * qw); R[7] = 2 * (qy * qz + qx * qw); R[8] = 1 - 2 * (qx * qx + qy * qy);
}

/* ── geometry (three.js generators, same vertex/index order and winding) ─────────────── */


typedef struct { geo_t *g; int nv, nt, cv, ct; } gbuild_t;

static bool gb_init(gbuild_t *b, geo_t *g, int nv, int nt) {
    b->g = g; b->nv = b->nt = 0; b->cv = nv; b->ct = nt;
    g->v = CUTE_CALLOC((size_t)nv * 3 * sizeof(float));
    g->t = CUTE_CALLOC((size_t)nt * 3 * sizeof(uint16_t));
    return g->v && g->t;
}
static int gb_v(gbuild_t *b, float x, float y, float z) {
    if (b->nv >= b->cv) return b->nv - 1;
    float *p = b->g->v + b->nv * 3; p[0] = x; p[1] = y; p[2] = z;
    return b->nv++;
}
static void gb_t(gbuild_t *b, int a, int c, int d) {
    if (b->nt >= b->ct) return;
    uint16_t *p = b->g->t + b->nt * 3; p[0] = (uint16_t)a; p[1] = (uint16_t)c; p[2] = (uint16_t)d; b->nt++;
}
static void gb_done(gbuild_t *b) {
    geo_t *g = b->g; g->nv = b->nv; g->nt = b->nt;
    for (int k = 0; k < 3; k++) { g->bmin[k] = 1e9f; g->bmax[k] = -1e9f; }
    for (int i = 0; i < g->nv; i++) for (int k = 0; k < 3; k++) {
        float v = g->v[i * 3 + k];
        if (v < g->bmin[k]) g->bmin[k] = v;
        if (v > g->bmax[k]) g->bmax[k] = v;
    }
}

/* SphereGeometry(1, w, h) scaled, optional teardrop pinch toward +y */
bool rg_sphere(geo_t *g, int w, int h, const float *s, float pinch, bool tear) {
    gbuild_t b;
    if (!gb_init(&b, g, (w + 1) * (h + 1), 2 * w * h)) return false;
    for (int iy = 0; iy <= h; iy++) {
        float v = (float)iy / h, st = sinf(v * PI_F), ct = cosf(v * PI_F);
        for (int ix = 0; ix <= w; ix++) {
            float u = (float)ix / w;
            float x = -cosf(u * TAU_F) * st, y = ct, z = sinf(u * TAU_F) * st;
            if (tear) { float k = 1.f - pinch * powf((y + 1.f) * 0.5f, 1.6f); x *= k; z *= k; }
            gb_v(&b, x * s[0], y * s[1], z * s[2]);
        }
    }
    for (int iy = 0; iy < h; iy++) for (int ix = 0; ix < w; ix++) {
        int a = iy * (w + 1) + ix + 1, bb = iy * (w + 1) + ix, c = (iy + 1) * (w + 1) + ix, d = (iy + 1) * (w + 1) + ix + 1;
        if (iy != 0) gb_t(&b, a, bb, d);
        if (iy != h - 1) gb_t(&b, bb, c, d);
    }
    gb_done(&b);
    return true;
}

/* CylinderGeometry(rTop, rBottom, h, radial) — also ConeGeometry (rTop = 0) */
bool rg_cylinder(geo_t *g, float rt, float rb, float h, int rs) {
    gbuild_t b;
    if (!gb_init(&b, g, 2 * (rs + 1) + 2 * (2 * rs + 1), 4 * rs)) return false;
    float hh = h * 0.5f;
    int idx[2][33];
    for (int y = 0; y <= 1; y++) {
        float r = y * (rb - rt) + rt;
        for (int x = 0; x <= rs; x++) { float th = (float)x / rs * TAU_F; idx[y][x] = gb_v(&b, r * sinf(th), -y * h + hh, r * cosf(th)); }
    }
    for (int x = 0; x < rs; x++) {
        int a = idx[0][x], bb = idx[1][x], c = idx[1][x + 1], d = idx[0][x + 1];
        if (rt > 0.f) gb_t(&b, a, bb, d);
        if (rb > 0.f) gb_t(&b, bb, c, d);
    }
    for (int top = 1; top >= 0; top--) {
        float r = top ? rt : rb, sign = top ? 1.f : -1.f;
        if (r <= 0.f) continue;
        int c0 = b.nv;
        for (int x = 0; x < rs; x++) gb_v(&b, 0, hh * sign, 0);
        int r0 = b.nv;
        for (int x = 0; x <= rs; x++) { float th = (float)x / rs * TAU_F; gb_v(&b, r * sinf(th), hh * sign, r * cosf(th)); }
        for (int x = 0; x < rs; x++) {
            int c = c0 + x, i = r0 + x;
            if (top) gb_t(&b, i, i + 1, c); else gb_t(&b, i + 1, i, c);
        }
    }
    gb_done(&b);
    return true;
}

/* CapsuleGeometry(r, len, capSeg, radial): LatheGeometry of two arcs (Path.getPoints) */
bool rg_capsule(geo_t *g, float r, float len, int cap, int rs) {
    float px[80], py[80]; int np = 0;
    int div = cap * 2;                                         /* EllipseCurve resolution = divisions·2 */
    for (int i = 0; i <= div && np < 80; i++) { float a = 1.5f * PI_F + (float)i / div * (0.5f * PI_F); px[np] = r * cosf(a); py[np] = -len * 0.5f + r * sinf(a); np++; }
    for (int i = 0; i <= div && np < 80; i++) {
        float a = (float)i / div * (0.5f * PI_F), x = r * cosf(a), y = len * 0.5f + r * sinf(a);
        if (fabsf(x - px[np - 1]) < 1e-6f && fabsf(y - py[np - 1]) < 1e-6f) continue;
        px[np] = x; py[np] = y; np++;
    }
    gbuild_t b;
    if (!gb_init(&b, g, (rs + 1) * np, 2 * rs * (np - 1))) return false;
    for (int i = 0; i <= rs; i++) {
        float phi = (float)i / rs * TAU_F, s = sinf(phi), c = cosf(phi);
        for (int j = 0; j < np; j++) gb_v(&b, px[j] * s, py[j], px[j] * c);
    }
    for (int i = 0; i < rs; i++) for (int j = 0; j < np - 1; j++) {
        int base = j + i * np, a = base, bb = base + np, c = base + np + 1, d = base + 1;
        gb_t(&b, a, bb, d); gb_t(&b, c, d, bb);
    }
    gb_done(&b);
    return true;
}

/* TorusGeometry(R, tube, radial, tubular, arc) */
bool rg_torus(geo_t *g, float R, float tube, int radial, int tub, float arc) {
    gbuild_t b;
    if (!gb_init(&b, g, (radial + 1) * (tub + 1), 2 * radial * tub)) return false;
    for (int j = 0; j <= radial; j++) for (int i = 0; i <= tub; i++) {
        float u = (float)i / tub * arc, v = (float)j / radial * TAU_F;
        gb_v(&b, (R + tube * cosf(v)) * cosf(u), (R + tube * cosf(v)) * sinf(u), tube * sinf(v));
    }
    for (int j = 1; j <= radial; j++) for (int i = 1; i <= tub; i++) {
        int a = (tub + 1) * j + i - 1, bb = (tub + 1) * (j - 1) + i - 1, c = (tub + 1) * (j - 1) + i, d = (tub + 1) * j + i;
        gb_t(&b, a, bb, d); gb_t(&b, bb, c, d);
    }
    gb_done(&b);
    return true;
}

/* CircleGeometry(r, seg, thetaStart, thetaLength), faces +z */
bool rg_circle(geo_t *g, float r, int seg, float t0, float tl) {
    gbuild_t b;
    if (!gb_init(&b, g, seg + 2, seg)) return false;
    gb_v(&b, 0, 0, 0);
    for (int s = 0; s <= seg; s++) { float th = t0 + (float)s / seg * tl; gb_v(&b, r * cosf(th), r * sinf(th), 0); }
    for (int i = 1; i <= seg; i++) gb_t(&b, i, i + 1, 0);
    gb_done(&b);
    return true;
}

bool rg_box(geo_t *g, const float *s) {
    gbuild_t b;
    if (!gb_init(&b, g, 8, 12)) return false;
    float x = s[0] * 0.5f, y = s[1] * 0.5f, z = s[2] * 0.5f;
    for (int k = 0; k < 8; k++) gb_v(&b, (k & 1) ? x : -x, (k & 2) ? y : -y, (k & 4) ? z : -z);
    static const uint8_t T[12][3] = { { 1, 3, 7 }, { 1, 7, 5 }, { 0, 4, 6 }, { 0, 6, 2 }, { 2, 6, 7 }, { 2, 7, 3 },
                                      { 0, 1, 5 }, { 0, 5, 4 }, { 4, 5, 7 }, { 4, 7, 6 }, { 0, 2, 3 }, { 0, 3, 1 } };
    for (int k = 0; k < 12; k++) gb_t(&b, T[k][0], T[k][1], T[k][2]);
    gb_done(&b);
    return true;
}

/* ── TubeGeometry over a CatmullRomCurve3 (centripetal, tension 0.5), as three.js builds it ── */

typedef struct { const float (*p)[3]; int n; float len[201]; } crv_t;

static void crv_point(const crv_t *c, float t, float *out) {
    const int l = c->n;
    float p = (l - 1) * t; int ip = (int)floorf(p); float w = p - ip;
    if (w == 0.f && ip == l - 1) { ip = l - 2; w = 1.f; }
    float tmp[3], p0[3], p3[3];
    const float *p1 = c->p[ip], *p2 = c->p[ip + 1];
    bool p0tmp = ip == 0;
    if (p0tmp) for (int k = 0; k < 3; k++) tmp[k] = 2 * c->p[0][k] - c->p[1][k];
    else memcpy(tmp, c->p[ip - 1], sizeof(tmp));
    memcpy(p0, tmp, sizeof(p0));
    if (ip + 2 < l) memcpy(p3, c->p[ip + 2], sizeof(p3));
    else { for (int k = 0; k < 3; k++) p3[k] = 2 * c->p[l - 1][k] - c->p[l - 2][k]; if (p0tmp) memcpy(p0, p3, sizeof(p0)); }   /* three shares one tmp vector */
    float d0 = 0, d1 = 0, d2 = 0;
    for (int k = 0; k < 3; k++) { d0 += (p1[k] - p0[k]) * (p1[k] - p0[k]); d1 += (p2[k] - p1[k]) * (p2[k] - p1[k]); d2 += (p3[k] - p2[k]) * (p3[k] - p2[k]); }
    float dt0 = powf(d0, 0.25f), dt1 = powf(d1, 0.25f), dt2 = powf(d2, 0.25f);
    if (dt1 < 1e-4f) dt1 = 1.f;
    if (dt0 < 1e-4f) dt0 = dt1;
    if (dt2 < 1e-4f) dt2 = dt1;
    for (int k = 0; k < 3; k++) {
        float x0 = p0[k], x1 = p1[k], x2 = p2[k], x3 = p3[k];
        float t1 = ((x1 - x0) / dt0 - (x2 - x0) / (dt0 + dt1) + (x2 - x1) / dt1) * dt1;
        float t2 = ((x2 - x1) / dt1 - (x3 - x1) / (dt1 + dt2) + (x3 - x2) / dt2) * dt1;
        float c2 = -3 * x1 + 3 * x2 - 2 * t1 - t2, c3 = 2 * x1 - 2 * x2 + t1 + t2;
        out[k] = x1 + t1 * w + c2 * w * w + c3 * w * w * w;
    }
}
/* arc length to the curve's own parameter (Curve.getUtoTmapping, 200 divisions) */
static float crv_u2t(const crv_t *c, float u) {
    const int il = 201; const float target = u * c->len[il - 1];
    int lo = 0, hi = il - 1;
    while (lo <= hi) {
        int i = lo + (hi - lo) / 2; float d = c->len[i] - target;
        if (d < 0) lo = i + 1; else if (d > 0) hi = i - 1; else { hi = i; break; }
    }
    int i = hi < 0 ? 0 : hi;
    if (c->len[i] == target || i >= il - 1) return (float)i / (il - 1);
    return (i + (target - c->len[i]) / (c->len[i + 1] - c->len[i])) / (il - 1);
}
static void norm3(float *v) { float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 0) { v[0] /= l; v[1] /= l; v[2] /= l; } }
static void cross3(float *o, const float *a, const float *b) { float r[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] }; memcpy(o, r, sizeof(r)); }

bool rg_tube(geo_t *g, const float (*path)[3], int n, int seg, float r) {
    enum { RS = 6 };
    if (n < 2 || seg < 1 || seg > 64) return false;
    crv_t c = { path, n, { 0 } };
    float last[3], cur[3];
    crv_point(&c, 0, last);
    for (int p = 1; p <= 200; p++) {
        crv_point(&c, p / 200.f, cur);
        c.len[p] = c.len[p - 1] + sqrtf((cur[0] - last[0]) * (cur[0] - last[0]) + (cur[1] - last[1]) * (cur[1] - last[1]) + (cur[2] - last[2]) * (cur[2] - last[2]));
        memcpy(last, cur, sizeof(last));
    }
    float T[65][3], N[65][3], B[65][3];
    for (int i = 0; i <= seg; i++) {   /* getTangentAt: a finite difference at the mapped t */
        float t = crv_u2t(&c, (float)i / seg), t1 = fmaxf(0.f, t - 1e-4f), t2 = fminf(1.f, t + 1e-4f), a[3], b[3];
        crv_point(&c, t1, a); crv_point(&c, t2, b);
        for (int k = 0; k < 3; k++) T[i][k] = b[k] - a[k];
        norm3(T[i]);
    }
    {   /* computeFrenetFrames: the first normal off the tangent's smallest axis, then parallel transport */
        float mn = 1e9f, nv[3] = { 0, 0, 0 }, ax = fabsf(T[0][0]), ay = fabsf(T[0][1]), az = fabsf(T[0][2]), v[3];
        if (ax <= mn) { mn = ax; nv[0] = 1; nv[1] = nv[2] = 0; }
        if (ay <= mn) { mn = ay; nv[1] = 1; nv[0] = nv[2] = 0; }
        if (az <= mn) { nv[2] = 1; nv[0] = nv[1] = 0; }
        cross3(v, T[0], nv); norm3(v);
        cross3(N[0], T[0], v); cross3(B[0], T[0], N[0]);
    }
    for (int i = 1; i <= seg; i++) {
        float v[3];
        memcpy(N[i], N[i - 1], sizeof(N[i]));
        cross3(v, T[i - 1], T[i]);
        if (sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) > 1.1920929e-7f) {
            norm3(v);
            float d = T[i - 1][0] * T[i][0] + T[i - 1][1] * T[i][1] + T[i - 1][2] * T[i][2];
            float th = acosf(d < -1 ? -1 : d > 1 ? 1 : d), cs = cosf(th), sn = sinf(th), x = N[i][0], y = N[i][1], z = N[i][2];
            float kd = v[0] * x + v[1] * y + v[2] * z, kx[3];   /* Rodrigues: rotate N about v by th */
            cross3(kx, v, N[i]);
            N[i][0] = x * cs + kx[0] * sn + v[0] * kd * (1 - cs);
            N[i][1] = y * cs + kx[1] * sn + v[1] * kd * (1 - cs);
            N[i][2] = z * cs + kx[2] * sn + v[2] * kd * (1 - cs);
        }
        cross3(B[i], T[i], N[i]);
    }
    gbuild_t b;
    if (!gb_init(&b, g, (seg + 1) * (RS + 1), 2 * seg * RS)) return false;
    for (int i = 0; i <= seg; i++) {
        float P[3];
        crv_point(&c, crv_u2t(&c, (float)i / seg), P);
        for (int j = 0; j <= RS; j++) {
            float v = (float)j / RS * TAU_F, s = sinf(v), cs = -cosf(v), nn[3];
            for (int k = 0; k < 3; k++) nn[k] = cs * N[i][k] + s * B[i][k];
            norm3(nn);
            gb_v(&b, P[0] + r * nn[0], P[1] + r * nn[1], P[2] + r * nn[2]);
        }
    }
    for (int j = 1; j <= seg; j++) for (int i = 1; i <= RS; i++) {
        int a = (RS + 1) * (j - 1) + (i - 1), bb = (RS + 1) * j + (i - 1), cc = (RS + 1) * j + i, d = (RS + 1) * (j - 1) + i;
        gb_t(&b, a, bb, d); gb_t(&b, bb, cc, d);
    }
    gb_done(&b);
    return true;
}
