/*
 * cute_rig.c — cute-kit runtime: geometry, rig, mood engine, kit animation, behaviours,
 * raster into the fluid target. See cute_rig.h. Each section names the JS it ports.
 *
 * Target board: Waveshare ESP32-S3-Touch-LCD-1.46 (412x412 round LCD)
 * FQBN: esp32:esp32:esp32s3:FlashSize=16M,PSRAM=enabled,CDCOnBoot=cdc
 */
#include "cute_rig.h"
#include "cute_act.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define CUTE_CALLOC(n) heap_caps_calloc(1, (n), MALLOC_CAP_SPIRAM)
#define CUTE_FREE(p)   heap_caps_free(p)
#else
#define CUTE_CALLOC(n) calloc(1, (n))
#define CUTE_FREE(p)   free(p)
#endif

#define PI_F  3.14159265f
#define TAU_F 6.28318531f
#define MAX_NODES 192
#define MAX_MESH  128
#define MAX_GEO   64

static inline float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static inline float bump(float q, float a, float b) { return (q < a || q > b) ? 0.f : sinf(PI_F * (q - a) / (b - a)); }

/* ── small linear algebra: 3×4 affine, row-major [r00 r01 r02 tx | r10 … ty | r20 … tz] ── */

typedef struct { float m[12]; } m34_t;
typedef struct { float x, y, z; } v3_t;


static m34_t m_mul(const m34_t *a, const m34_t *b) {
    m34_t r;
    for (int i = 0; i < 3; i++) {
        const float *A = a->m + i * 4;
        for (int j = 0; j < 3; j++) r.m[i * 4 + j] = A[0] * b->m[j] + A[1] * b->m[4 + j] + A[2] * b->m[8 + j];
        r.m[i * 4 + 3] = A[0] * b->m[3] + A[1] * b->m[7] + A[2] * b->m[11] + A[3];
    }
    return r;
}
static inline v3_t m_apply(const m34_t *a, float x, float y, float z) {
    v3_t r = { a->m[0] * x + a->m[1] * y + a->m[2] * z + a->m[3],
               a->m[4] * x + a->m[5] * y + a->m[6] * z + a->m[7],
               a->m[8] * x + a->m[9] * y + a->m[10] * z + a->m[11] };
    return r;
}
static float m_det(const m34_t *a) {
    const float *m = a->m;
    return m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
}
/* 3×3 rotation for one axis */
static void rot_axis(float *R, int axis, float a) {
    float c = cosf(a), s = sinf(a);
    memset(R, 0, 9 * sizeof(float));
    if (axis == 0) { R[0] = 1; R[4] = c; R[5] = -s; R[7] = s; R[8] = c; }
    else if (axis == 1) { R[0] = c; R[2] = s; R[4] = 1; R[6] = -s; R[8] = c; }
    else { R[0] = c; R[1] = -s; R[3] = s; R[4] = c; R[8] = 1; }
}
static void mat3_mul(float *o, const float *a, const float *b) {
    float t[9];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) t[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
    memcpy(o, t, sizeof(t));
}
/* three.js Euler orders: 'XYZ' → Rx·Ry·Rz (the order string read left to right) */
static const uint8_t ORDERS[6][3] = { { 0, 1, 2 }, { 1, 0, 2 }, { 2, 0, 1 }, { 0, 2, 1 }, { 1, 2, 0 }, { 2, 1, 0 } };
static void euler_mat(float *R, const float *e, int order) {
    float A[9], B[9], C[9];
    const uint8_t *o = ORDERS[order];
    rot_axis(A, o[0], e[o[0]]); rot_axis(B, o[1], e[o[1]]); rot_axis(C, o[2], e[o[2]]);
    mat3_mul(R, A, B); mat3_mul(R, R, C);
}
/* quaternion.setFromUnitVectors(+z, n) → rotation matrix (shortest arc) */
static void rot_from_z(float *R, v3_t n) {
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

typedef struct { int nv, nt; float *v; uint16_t *t; float bmin[3], bmax[3]; } geo_t;

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
static bool geo_sphere(geo_t *g, int w, int h, const float *s, float pinch, bool tear) {
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
static bool geo_cylinder(geo_t *g, float rt, float rb, float h, int rs) {
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
static bool geo_capsule(geo_t *g, float r, float len, int cap, int rs) {
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
static bool geo_torus(geo_t *g, float R, float tube, int radial, int tub, float arc) {
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
static bool geo_circle(geo_t *g, float r, int seg, float t0, float tl) {
    gbuild_t b;
    if (!gb_init(&b, g, seg + 2, seg)) return false;
    gb_v(&b, 0, 0, 0);
    for (int s = 0; s <= seg; s++) { float th = t0 + (float)s / seg * tl; gb_v(&b, r * cosf(th), r * sinf(th), 0); }
    for (int i = 1; i <= seg; i++) gb_t(&b, i, i + 1, 0);
    gb_done(&b);
    return true;
}

static bool geo_box(geo_t *g, const float *s) {
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

/* ── rig nodes + meshes ─────────────────────────────────────────────────────────────── */

typedef struct {
    int16_t parent;
    uint8_t order;
    bool    use_rm;            /* rotation from rm (face placement) instead of rot */
    float   pos[3], rot[3], scale[3], rm[9];
    float   bpos[3], brot[3], bscale[3];   /* rest pose (behaviours, sockets) */
    int8_t  side;
    m34_t   world;
} node_t;

typedef struct {
    int16_t node;
    uint8_t geo, color, name;
    bool    visible, spec;     /* spec: a spec part (counts for the height normalisation) */
    int8_t  side;
} mesh_t;

typedef struct { int node; int side; } srec_t;

/* mood engine pose (mood.js zeroPose) */
typedef struct {
    float pos[3], rot[3], head[3], spine[3];
    float squash, lid, look[2], mouth;
    int   eye;                 /* 0 normal 1 wide 2 happy 3 sleepy */
    int   mood;
} pose_t;
enum { EYE_NORMAL, EYE_WIDE, EYE_HAPPY, EYE_SLEEPY };

typedef struct { float k, d, x, v; } spring_t;
typedef struct { float a, f; int side; float next_glance, glance_until; int glance_dir; } jit_t;

typedef struct { float t, phrase_end, pause_end, rate, phase, value, syll_amp; } speech_t;

typedef struct { float eased; float next, t0; float spin; bool init; } term_state_t;
typedef struct { int g, inner, scl, ball, half, hl1, hl2, arc; int side; } eye_t;
typedef struct { float next, phase, hold, value; bool dbl, second; } eblink_t;

struct cute_rt {
    cute_spec_t spec;
    uint32_t rng;
    /* rig */
    node_t  nodes[MAX_NODES]; int nn;
    mesh_t  meshes[MAX_MESH]; int nm;
    geo_t   geos[MAX_GEO];    int ng;
    srec_t  sock[CUTE_N_SOCKETS][2]; int nsock[CUTE_N_SOCKETS];
    int n_root, n_norm, n_lean, n_body, n_hpivot, n_head;
    float H[3], neck[3]; bool has_body;
    /* face */
    eye_t eyes[2];
    int mouth_g, mouth_closed[2], n_closed, mouth_open, mouth_openD, mouth_tongue;
    int cheeks[2]; float cheek_r;
    /* kit state */
    float t, w[CUTE_N_MOODS], lift, wag_ph;
    struct { float next, t; } blink;
    struct { float next, t; int dir; } tilt;
    struct { float next, t; } flick;
    struct { float x, vx, z, vz, last_y; } spring;
    struct { float sleep, since; } lid_state;
    float norm_k, env_min[3], env_max[3], rise;
    /* mood engine */
    int mood, prev; float et, prev_t, fade, fade_dur, anticip, time;
    jit_t j, pj; speech_t env; float env_value;
    eblink_t eblink;
    struct { float next, x, y; } saccade;
    spring_t sp_h[3], sp_b[3], sp_py, sp_sq;
    pose_t pose;
    /* one-shot action (cute_act.h): the one playing, and the one it replaced fading out */
    int act, pact; float act_t, pact_t, pact_w;
    /* behaviours */
    term_state_t bst[CUTE_MAX_BEHAV][CUTE_MAX_TERMS];
    float beh_lid;
    /* camera + raster */
    float cam_t[3], cam_x[3], cam_y[3], cam_z[3], cam_half;
    float cam_zoom, cam_focus;                     /* an action's camera (cute_act.h): this frame's */
    int   rW, rH, tris;
    float *zbuf; uint32_t *cbuf;                   /* (2W)×(4H) samples */
    float *vbuf; int vcap;                         /* per-mesh camera-space vertices      */
    uint8_t tag;                                   /* frame tag in the sample alpha byte  */
    float *cov, *lum, *cr, *cg, *cb; uint8_t *feat; /* W×H cells */
};

static float rnd(cute_rt_t *rt) {
    uint32_t x = rt->rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; rt->rng = x;
    return (float)(x >> 8) * (1.f / 16777216.f);
}
static float rand_ab(cute_rt_t *rt, float a, float b) { return a + rnd(rt) * (b - a); }

static int node_add(cute_rt_t *rt, int parent) {
    if (rt->nn >= MAX_NODES) return parent;          /* degrade: attach to the parent */
    node_t *n = &rt->nodes[rt->nn];
    memset(n, 0, sizeof(*n));
    n->parent = (int16_t)parent;
    n->scale[0] = n->scale[1] = n->scale[2] = 1.f;
    n->bscale[0] = n->bscale[1] = n->bscale[2] = 1.f;
    n->side = 1;
    return rt->nn++;
}
static void node_rest(node_t *n) {   /* remember the current local transform as the rest pose */
    memcpy(n->bpos, n->pos, sizeof(n->pos)); memcpy(n->brot, n->rot, sizeof(n->rot)); memcpy(n->bscale, n->scale, sizeof(n->scale));
}
static int mesh_add(cute_rt_t *rt, int node, int geo, int color, int name, bool spec, int side) {
    if (rt->nm >= MAX_MESH || geo < 0) return -1;
    mesh_t *m = &rt->meshes[rt->nm];
    m->node = (int16_t)node; m->geo = (uint8_t)geo; m->color = (uint8_t)color; m->name = (uint8_t)name;
    m->visible = true; m->spec = spec; m->side = (int8_t)side;
    return rt->nm++;
}
static int geo_new(cute_rt_t *rt) { return rt->ng < MAX_GEO ? rt->ng++ : -1; }

/* ── kit: surface placement (CuteCreature._surfPoint / _place) ─────────────────────── */

static v3_t surf_point(const cute_rt_t *rt, float u, float v, float lift, float *R) {
    const float *fr = rt->spec.face_r, *fo = rt->spec.face_off;
    float d = u * u + v * v;
    if (d > 0.96f) { float k = sqrtf(0.96f / d); u *= k; v *= k; d = 0.96f; }
    float x = u * fr[0], y = v * fr[1], z = fr[2] * sqrtf(1.f - d);
    v3_t n = { x / (fr[0] * fr[0]), y / (fr[1] * fr[1]), z / (fr[2] * fr[2]) };
    float nl = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
    n.x /= nl; n.y /= nl; n.z /= nl;
    if (R) rot_from_z(R, n);
    v3_t p = { x + n.x * lift + fo[0], y + n.y * lift + fo[1], z + n.z * lift + fo[2] };
    return p;
}
static void place(cute_rt_t *rt, int node, float u, float v, float lift) {
    node_t *n = &rt->nodes[node];
    v3_t p = surf_point(rt, u, v, lift, n->rm);
    n->pos[0] = p.x; n->pos[1] = p.y; n->pos[2] = p.z; n->use_rm = true;
}

/* ── build (CuteCreature._build / _addPart / _buildFace) ───────────────────────────── */

static int part_geometry(cute_rt_t *rt, const cute_part_t *p) {
    int gi = geo_new(rt);
    if (gi < 0) return -1;
    geo_t *g = &rt->geos[gi];
    bool ok = false;
    const float *a = p->params;
    switch (p->type) {
    case CP_ELLIPSOID: ok = geo_sphere(g, p->seg_a, p->seg_b, a, 0, false); break;
    case CP_TEARDROP:  ok = geo_sphere(g, p->seg_a, p->seg_b, a, a[3], true); break;
    case CP_CAPSULE:   ok = geo_capsule(g, a[0], a[1], p->seg_b, p->seg_a); break;
    case CP_CONE:      ok = geo_cylinder(g, 0, a[0], a[1], p->seg_a); break;
    case CP_CYLINDER:  ok = geo_cylinder(g, a[0], a[1], a[2], p->seg_a); break;
    case CP_TORUS:     ok = geo_torus(g, a[0], a[1], p->seg_a, p->seg_b, a[2]); break;
    case CP_BOX:       ok = geo_box(g, a); break;
    case CP_DISC:      ok = geo_circle(g, a[0], p->seg_a, 0, TAU_F); break;
    }
    if (!ok) { rt->ng--; return -1; }
    if (p->has_offset) for (int i = 0; i < g->nv; i++) for (int k = 0; k < 3; k++) g->v[i * 3 + k] += p->offset[k];
    if (p->has_offset) for (int k = 0; k < 3; k++) { g->bmin[k] += p->offset[k]; g->bmax[k] += p->offset[k]; }
    return gi;
}

static bool paired(int s) { return s == CS_EAR || s == CS_SIDE || s == CS_FOOT || s == CS_ANTENNA || s == CS_HORN; }

static void add_part(cute_rt_t *rt, const cute_part_t *p) {
    int gi = part_geometry(rt, p);
    if (gi < 0) return;
    int s = p->socket;
    for (int r = 0; r < rt->nsock[s]; r++) {
        bool pr = paired(s);
        int copies = pr ? 1 : (p->mirror ? 2 : 1);
        if (!pr && r > 0) break;
        for (int c = 0; c < copies; c++) {
            bool mirror = pr ? rt->sock[s][r].side < 0 : c == 1;
            int nd = node_add(rt, rt->sock[s][r].node);
            node_t *n = &rt->nodes[nd];
            n->pos[0] = mirror ? -p->pos[0] : p->pos[0]; n->pos[1] = p->pos[1]; n->pos[2] = p->pos[2];
            n->rot[0] = p->rot[0]; n->rot[1] = mirror ? -p->rot[1] : p->rot[1]; n->rot[2] = mirror ? -p->rot[2] : p->rot[2];
            n->order = p->rot_order;
            memcpy(n->scale, p->scale, sizeof(n->scale));
            n->side = (int8_t)(mirror ? -1 : (rt->sock[s][r].side ? rt->sock[s][r].side : 1));
            node_rest(n);
            int mi = mesh_add(rt, nd, gi, p->color, p->name, true, n->side);
            if (mi >= 0 && p->hidden) rt->meshes[mi].visible = false;
        }
    }
}

static int face_mesh(cute_rt_t *rt, int parent, int geo, int color, float z) {
    int nd = node_add(rt, parent);
    rt->nodes[nd].pos[2] = z;
    node_rest(&rt->nodes[nd]);
    mesh_add(rt, nd, geo, color, 255, false, 1);
    return nd;
}
static mesh_t *mesh_of(cute_rt_t *rt, int node) {
    for (int i = rt->nm - 1; i >= 0; i--) if (rt->meshes[i].node == node) return &rt->meshes[i];
    return NULL;
}
static void set_vis(cute_rt_t *rt, int node, bool v) { mesh_t *m = mesh_of(rt, node); if (m) m->visible = v; }

static void build_face(cute_rt_t *rt) {
    const cute_spec_t *S = &rt->spec;
    int g_disc = geo_new(rt), g_half = geo_new(rt), g_dot = geo_new(rt), g_arc = geo_new(rt);
    if (g_arc < 0) return;
    geo_circle(&rt->geos[g_disc], 1, 16, 0, TAU_F);
    geo_circle(&rt->geos[g_half], 1, 10, PI_F, PI_F);
    geo_circle(&rt->geos[g_dot], 1, 7, 0, TAU_F);
    geo_torus(&rt->geos[g_arc], 0.72f, 0.26f, 3, 6, PI_F);
    for (int e = 0; e < 2; e++) {
        int side = e == 0 ? 1 : -1;
        rt->eyes[e].side = side;
        rt->eyes[e].g = node_add(rt, rt->n_head);
        rt->eyes[e].inner = node_add(rt, rt->eyes[e].g);
        int in = rt->eyes[e].inner;
        rt->eyes[e].scl = face_mesh(rt, in, g_disc, S->col_white, 0);
        rt->eyes[e].ball = face_mesh(rt, in, g_disc, S->eye_color, 0.06f);
        rt->eyes[e].half = face_mesh(rt, in, g_half, S->eye_color, 0.06f);
        rt->eyes[e].hl1 = face_mesh(rt, in, g_dot, S->col_shine, 0.14f);
        rt->eyes[e].hl2 = face_mesh(rt, in, g_dot, S->col_shine, 0.14f);
        rt->eyes[e].arc = face_mesh(rt, in, g_arc, S->eye_color, 0.06f);
        rt->nodes[rt->eyes[e].arc].scale[2] = 0.35f;
    }
    rt->mouth_g = -1;
    if (S->mouth_type == CMT_SMILE || S->mouth_type == CMT_CAT) {
        int g_marc = geo_new(rt), g_open = geo_new(rt), g_openD = geo_new(rt), g_tongue = geo_new(rt);
        if (g_tongue >= 0) {
            geo_torus(&rt->geos[g_marc], 1, 0.3f, 3, 6, PI_F);
            geo_circle(&rt->geos[g_open], 1, 12, 0, TAU_F);
            geo_circle(&rt->geos[g_openD], 1, 10, PI_F, PI_F);
            geo_circle(&rt->geos[g_tongue], 1, 8, 0, TAU_F);
            rt->mouth_g = node_add(rt, rt->n_head);
            bool cat = S->mouth_type == CMT_CAT;
            rt->n_closed = cat ? 2 : 1;
            for (int c = 0; c < rt->n_closed; c++) {
                int nd = face_mesh(rt, rt->mouth_g, g_marc, S->col_mouth, 0.004f);
                node_t *n = &rt->nodes[nd];
                n->rot[2] = PI_F;
                n->scale[0] = S->mouth_w * (cat ? 0.55f : 1.f); n->scale[1] = S->mouth_w * (cat ? 0.55f : 0.8f); n->scale[2] = 0.01f;
                if (cat) n->pos[0] = (c == 0 ? -1.f : 1.f) * S->mouth_w * 0.55f;
                rt->mouth_closed[c] = nd;
            }
            rt->mouth_open = face_mesh(rt, rt->mouth_g, g_open, S->col_mouth, 0.003f);
            rt->mouth_openD = face_mesh(rt, rt->mouth_g, g_openD, S->col_mouth, 0.003f);
            rt->mouth_tongue = face_mesh(rt, rt->mouth_g, g_tongue, S->col_tongue, 0.006f);
            place(rt, rt->mouth_g, 0, S->mouth_y, 0.006f);
        }
    }
    rt->cheeks[0] = rt->cheeks[1] = -1;
    if (S->has_cheeks) {
        int g_ch = geo_new(rt);
        if (g_ch >= 0) {
            geo_circle(&rt->geos[g_ch], 1, 9, 0, TAU_F);
            for (int c = 0; c < 2; c++) {
                int nd = node_add(rt, rt->n_head);
                mesh_add(rt, nd, g_ch, S->col_cheek, 255, false, 1);
                place(rt, nd, (c == 0 ? 1.f : -1.f) * S->cheek_x, S->cheek_y, 0.004f);
                rt->nodes[nd].scale[0] = S->cheek_r * 1.3f; rt->nodes[nd].scale[1] = S->cheek_r; rt->nodes[nd].scale[2] = 1;
                rt->cheeks[c] = nd;
            }
            rt->cheek_r = S->cheek_r;
        }
    }
}

/* ── mood engine (js/mood.js) ───────────────────────────────────────────────────────── */

static float jit(cute_rt_t *rt, float p) { return 1.f + rand_ab(rt, -p, p); }
static jit_t new_jit(cute_rt_t *rt) {
    jit_t j = { jit(rt, 0.15f), jit(rt, 0.15f), rnd(rt) < 0.5f ? -1 : 1, rand_ab(rt, 3, 8), 0, 1 };
    return j;
}
static void zero_pose(pose_t *p) { memset(p, 0, sizeof(*p)); p->eye = EYE_NORMAL; }

static float speech_update(cute_rt_t *rt, speech_t *e, float dt) {
    e->t += dt;
    if (e->t > e->phrase_end && e->pause_end == 0.f) e->pause_end = e->t + rand_ab(rt, 0.25f, 0.7f);
    if (e->pause_end != 0.f && e->t > e->pause_end) { e->pause_end = 0; e->phrase_end = e->t + rand_ab(rt, 1.2f, 3); e->rate = rand_ab(rt, 4, 5); }
    bool speaking = e->pause_end == 0.f;
    float prev = e->phase;
    e->phase += dt * e->rate;
    if (floorf(e->phase) != floorf(prev)) e->syll_amp = rand_ab(rt, 0.45f, 1);
    float s = sinf(TAU_F * e->phase); if (s < 0) s = 0;
    float target = speaking ? powf(s, 0.7f) * e->syll_amp : 0.f;
    float k = target > e->value ? 1.f - expf(-dt * 40) : 1.f - expf(-dt * 14);
    e->value += (target - e->value) * k;
    return e->value;
}

static void gen(cute_rt_t *rt, int mood, pose_t *p, float t, jit_t *j, float env) {
    switch (mood) {
    case CM_IDLE:
        p->head[2] = 0.05f * j->a;
        p->head[1] = 0.08f * sinf(TAU_F * t / (9 * j->f));
        p->rot[2] = 0.015f * sinf(TAU_F * t / (6 * j->f));
        break;
    case CM_LISTENING:
        p->spine[0] = 0.14f * j->a; p->rot[0] = 0.05f; p->pos[2] = 0.05f;
        p->head[2] = 0.22f * j->a * j->side;
        p->head[0] = -0.06f + 0.05f * fmaxf(0.f, sinf(TAU_F * t / (2.6f * j->f)));
        p->eye = EYE_WIDE; p->look[1] = 0.1f;
        break;
    case CM_THINKING:
        p->rot[2] = 0.07f * sinf(TAU_F * t / (3.2f * j->f));
        p->pos[1] = 0.015f * sinf(TAU_F * t / (2.4f * j->f));
        p->head[0] = -0.28f * j->a; p->head[1] = 0.32f * j->side; p->head[2] = 0.1f * j->side;
        p->look[0] = 0.7f * j->side; p->look[1] = 0.7f;
        break;
    case CM_SPEAKING:
        p->squash = 0.06f * env; p->pos[1] = 0.018f * env;
        p->head[0] = -0.05f * env + 0.03f * sinf(TAU_F * t / 1.7f);
        p->head[2] = 0.06f * sinf(TAU_F * t / (3.3f * j->f));
        p->mouth = env;
        if (t > j->next_glance) { j->glance_until = t + rand_ab(rt, 0.5f, 0.9f); j->next_glance = t + rand_ab(rt, 3, 8); j->glance_dir = rnd(rt) < 0.5f ? -1 : 1; }
        if (t < j->glance_until) { p->look[0] = 0.8f * j->glance_dir; p->head[1] = 0.12f * j->glance_dir; }
        break;
    case CM_HAPPY: {
        float f = 2.1f * j->f, hop = fabsf(sinf(PI_F * f * t));
        p->pos[1] = 0.13f * j->a * hop;
        p->squash = hop < 0.25f ? -0.12f * (1 - hop / 0.25f) : 0.05f * hop;
        p->head[2] = 0.12f * sinf(TAU_F * f * 0.5f * t);
        p->rot[1] = 0.1f * sinf(TAU_F * f * 0.25f * t);
        p->eye = EYE_HAPPY; p->mouth = 0.35f;
        break;
    }
    case CM_SLEEPY: {
        /* asleep, not down: eyes shut and a slight nod; a low head and half-open eyes read as sad */
        p->head[0] = 0.12f * j->a; p->spine[0] = 0.04f; p->rot[0] = 0.03f; p->pos[1] = -0.015f;
        float per = 7 * j->f, c = fmodf(t, per) / per;
        if (c > 0.75f) { float q = (c - 0.75f) / 0.25f; p->head[0] += 0.25f * sinf(PI_F * q) * (q < 0.8f ? 1.f : -0.5f); }
        p->lid = 1.f;
        p->eye = EYE_SLEEPY;
        break;
    }
    case CM_SURPRISED:
        if (t < 0.12f) p->squash = -0.14f * sinf(PI_F * t / 0.12f);
        else if (t < 0.6f) { float q = (t - 0.12f) / 0.48f; p->pos[1] = 0.17f * sinf(PI_F * q); p->squash = 0.16f * (1 - q) - 0.04f; }
        else { float q = fminf(1.f, (t - 0.6f) / 0.6f); p->squash = -0.04f * (1 - q); }
        p->spine[0] = -0.1f; p->rot[0] = -0.05f; p->head[0] = -0.1f;
        p->eye = EYE_WIDE; p->mouth = 0.55f;
        break;
    }
}

static void lerp_pose(pose_t *o, const pose_t *a, const pose_t *b, float w) {
    for (int k = 0; k < 3; k++) {
        o->pos[k] = a->pos[k] + (b->pos[k] - a->pos[k]) * w; o->rot[k] = a->rot[k] + (b->rot[k] - a->rot[k]) * w;
        o->head[k] = a->head[k] + (b->head[k] - a->head[k]) * w; o->spine[k] = a->spine[k] + (b->spine[k] - a->spine[k]) * w;
    }
    o->squash = a->squash + (b->squash - a->squash) * w; o->lid = a->lid + (b->lid - a->lid) * w; o->mouth = a->mouth + (b->mouth - a->mouth) * w;
    o->look[0] = a->look[0] + (b->look[0] - a->look[0]) * w; o->look[1] = a->look[1] + (b->look[1] - a->look[1]) * w;
    o->eye = w < 0.5f ? a->eye : b->eye;
}
static float spring_step(spring_t *s, float target, float dt) {
    float a = s->k * (target - s->x) - s->d * s->v; s->v += a * dt; s->x += s->v * dt; return s->x;
}

static void engine_init(cute_rt_t *rt) {
    rt->mood = CM_IDLE; rt->prev = -1; rt->fade = 1; rt->fade_dur = 0.7f;
    rt->j = new_jit(rt); rt->pj = rt->j;
    rt->env.phrase_end = rand_ab(rt, 1.2f, 3); rt->env.rate = rand_ab(rt, 4, 5); rt->env.syll_amp = 1;
    rt->eblink.next = rand_ab(rt, 2, 6); rt->eblink.phase = -1;
    rt->saccade.next = rand_ab(rt, 0.2f, 0.6f);
    for (int k = 0; k < 3; k++) { rt->sp_h[k] = (spring_t){ 90, 9, 0, 0 }; rt->sp_b[k] = (spring_t){ 45, 7, 0, 0 }; }
    rt->sp_sq = (spring_t){ 160, 9, 0, 0 };
    rt->sp_py = (spring_t){ 220, 14, 0, 0 };
    rt->act = rt->pact = -1;
}

void cute_rt_set_mood(cute_rt_t *rt, int mood) {
    if (!rt || mood < 0 || mood >= CUTE_N_MOODS || mood == rt->mood) return;
    rt->prev = rt->mood; rt->pj = rt->j; rt->prev_t = rt->et;
    rt->mood = mood; rt->et = 0; rt->fade = 0; rt->fade_dur = rand_ab(rt, 0.5f, 1.0f);
    rt->j = new_jit(rt);
    rt->anticip = 0.0001f;
}
int cute_rt_mood(const cute_rt_t *rt) { return rt ? rt->mood : 0; }

void cute_rt_act(cute_rt_t *rt, int act) {
    if (!rt || act < 0 || act >= CUTE_N_ACTS) return;
    if (rt->act >= 0) { rt->pact = rt->act; rt->pact_t = rt->act_t; rt->pact_w = 1.f; }   /* fades out */
    rt->act = act; rt->act_t = 0.f;
}
int cute_rt_acting(const cute_rt_t *rt) { return rt ? rt->act : -1; }
float cute_rt_act_time(const cute_rt_t *rt) { return rt && rt->act >= 0 ? rt->act_t : 0.f; }

/* the action layer: offsets added after the springs (the curves are smooth already, and a
 * nod must not be swallowed by the head's 1.5 Hz spring); look before the saccade clamp,
 * lid after the blink (taken as max) */
static void act_add(pose_t *p, const cute_act_pose_t *a, float w) {
    for (int k = 0; k < 3; k++) {
        p->pos[k] += w * a->pos[k]; p->rot[k] += w * a->rot[k];
        p->head[k] += w * a->head[k]; p->spine[k] += w * a->spine[k];
    }
    p->squash += w * a->squash; p->mouth = fmaxf(p->mouth, w * a->mouth);
    p->look[0] += w * a->look[0]; p->look[1] += w * a->look[1];
}
static float act_layer(cute_rt_t *rt, float dt, pose_t *p) {
    float lid = 0.f;
    cute_act_pose_t a;
    rt->cam_zoom = rt->cam_focus = 0.f;
    if (rt->pact >= 0) {                               /* the replaced one, out over 0.25 s */
        rt->pact_t += dt; rt->pact_w -= dt / 0.25f;
        if (rt->pact_w <= 0.f || rt->pact_t > cute_act_dur(rt->pact)) rt->pact = -1;
        else { cute_act_pose(rt->pact, rt->pact_t, &a); act_add(p, &a, rt->pact_w); lid = rt->pact_w * a.lid; rt->cam_zoom = rt->pact_w * a.zoom; rt->cam_focus = rt->pact_w * a.focus; }
    }
    if (rt->act >= 0) {
        rt->act_t += dt;
        if (rt->act_t > cute_act_dur(rt->act)) rt->act = -1;
        else { cute_act_pose(rt->act, rt->act_t, &a); act_add(p, &a, 1.f); lid = fmaxf(lid, a.lid); rt->cam_zoom += a.zoom; rt->cam_focus += a.focus; }
    }
    return lid;
}
float cute_rt_env(const cute_rt_t *rt) { return rt ? rt->env_value : 0.f; }

static void engine_update(cute_rt_t *rt, float dt, pose_t *p) {
    dt = fminf(dt, 0.05f);
    rt->time += dt; rt->et += dt; rt->prev_t += dt;
    rt->env_value = (rt->mood == CM_SPEAKING || rt->prev == CM_SPEAKING) ? speech_update(rt, &rt->env, dt) : 0.f;
    pose_t cur; zero_pose(&cur); gen(rt, rt->mood, &cur, rt->et, &rt->j, rt->env_value);
    *p = cur;
    if (rt->fade < 1.f && rt->prev >= 0) {
        rt->fade = fminf(1.f, rt->fade + dt / rt->fade_dur);
        pose_t old; zero_pose(&old); gen(rt, rt->prev, &old, rt->prev_t, &rt->pj, rt->env_value);
        float w = rt->fade * rt->fade * (3 - 2 * rt->fade);
        lerp_pose(p, &old, &cur, w);
    }
    bool sleepy = rt->mood == CM_SLEEPY;
    float period = sleepy ? 5 * rt->j.f : 3.5f * rt->j.f, amp = sleepy ? 0.04f : 0.025f;
    p->squash += amp * sinf(TAU_F * rt->time / period);
    if (rt->anticip > 0) {
        rt->anticip += dt; float q = rt->anticip / 0.18f;
        if (q >= 1) rt->anticip = 0; else { p->squash -= 0.07f * sinf(PI_F * q); p->head[0] += 0.08f * sinf(PI_F * q); }
    }
    for (int k = 0; k < 3; k++) p->head[k] = spring_step(&rt->sp_h[k], p->head[k], dt);
    for (int k = 0; k < 3; k++) p->rot[k] = spring_step(&rt->sp_b[k], p->rot[k], dt);
    p->pos[1] = spring_step(&rt->sp_py, p->pos[1], dt);
    p->squash = spring_step(&rt->sp_sq, p->squash, dt);
    const float act_lid = act_layer(rt, dt, p);
    rt->saccade.next -= dt;
    if (rt->saccade.next <= 0) { rt->saccade.next = rand_ab(rt, 0.2f, 0.6f); rt->saccade.x = rand_ab(rt, -0.08f, 0.08f); rt->saccade.y = rand_ab(rt, -0.06f, 0.06f); }
    p->look[0] = clampf(p->look[0] + rt->saccade.x, -1, 1);
    p->look[1] = clampf(p->look[1] + rt->saccade.y, -1, 1);
    /* blinks: 2–6 s, 80 ms close / 50–100 hold / 150 open; 1 in 8 doubles */
    eblink_t *b = &rt->eblink;
    b->next -= dt;
    if (b->phase < 0 && b->next <= 0) { b->phase = 0; b->hold = rand_ab(rt, 0.05f, 0.1f); b->dbl = !b->second && rnd(rt) < 1.f / 8; b->second = false; }
    if (b->phase >= 0) {
        b->phase += dt; float c = 0.08f, h = c + b->hold, o = h + 0.15f;
        b->value = b->phase < c ? b->phase / c : b->phase < h ? 1.f : b->phase < o ? 1.f - (b->phase - h) / 0.15f : 0.f;
        if (b->phase >= o) {
            b->phase = -1; b->value = 0;
            bool engaged = rt->mood == CM_LISTENING || rt->mood == CM_SPEAKING;
            b->next = b->dbl ? rand_ab(rt, 0.2f, 0.4f) : engaged ? rand_ab(rt, 2, 4) : rand_ab(rt, 4, 7);
            if (b->dbl) { b->dbl = false; b->second = true; }
        }
    }
    p->lid = clampf(fmaxf(p->lid, 0) + b->value * (1 - p->lid), 0, 1);
    p->lid = fmaxf(p->lid, act_lid);
    p->mood = rt->mood;
}

/* ── kit animation (CuteCreature.update / MOTIONS / _face / _limbs) ─────────────────── */

typedef struct {
    float pos[3], rot[3], head[3], squash, lift, foot[2], side, ear, tail[3];
} layer_t;

static float gate(float t, float period, float duty) { return clampf((sinf(TAU_F * t / period) + 1 - 2 * duty) * 3, 0, 1); }

static void motion(const cute_rt_t *rt, layer_t *L, float t, float a, const float *w, int type, float amp, float rate, float height) {
    switch (type) {
    case CMO_WADDLE: {
        float g = fmaxf(fmaxf(gate(t, 7.3f / rate, 0.45f), w[CM_HAPPY]), 0.35f * w[CM_SPEAKING]);
        float k = a * g * amp, ph = TAU_F * 1.9f * rate * (1 + 0.5f * w[CM_HAPPY]) * t, s = sinf(ph);
        L->rot[2] += 0.09f * k * s; L->rot[1] += 0.06f * k * sinf(ph + 0.6f);
        L->pos[1] += 0.012f * k * fabsf(s);
        L->foot[0] += 0.045f * k * fmaxf(0, s); L->foot[1] += 0.045f * k * fmaxf(0, -s);
        L->tail[1] += 0.25f * k * sinf(ph + 1.2f);
        break;
    }
    case CMO_HOP: {
        float P = 2.4f / rate, q = fmodf(t, P) / P, k = a * amp * (1 - w[CM_HAPPY]) * (1 - w[CM_SLEEPY]);
        L->squash += k * (-0.12f * bump(q, 0, 0.07f) + 0.06f * bump(q, 0.07f, 0.2f) - 0.1f * bump(q, 0.2f, 0.27f));
        L->pos[1] += k * 0.07f * bump(q, 0.07f, 0.2f);
        L->foot[0] += k * 0.02f * bump(q, 0.07f, 0.2f); L->foot[1] = L->foot[0];
        L->ear += 0.25f * k * bump(q, 0.15f, 0.35f);
        break;
    }
    case CMO_SLITHER: {
        float ph = TAU_F * 0.55f * rate * (1 + w[CM_HAPPY]) * t, k = a * amp;
        L->pos[0] += 0.03f * k * sinf(ph); L->rot[2] += 0.07f * k * sinf(ph + 1); L->rot[1] += 0.14f * k * sinf(ph);
        L->tail[1] += 0.55f * k * sinf(ph - 1.2f); L->tail[0] += 0.12f * k * sinf(ph * 2);
        break;
    }
    case CMO_FLOAT: case CMO_FLUTTER: {
        float k = type == CMO_FLUTTER ? amp * 0.7f : amp;
        L->lift += height * (1 - 0.6f * w[CM_SLEEPY]);
        L->pos[1] += 0.03f * k * sinf(TAU_F * t / (2.6f / rate));
        L->rot[2] += 0.045f * k * sinf(TAU_F * t / (3.4f / rate)); L->rot[0] += 0.03f * k * sinf(TAU_F * t / (4.1f / rate));
        L->tail[1] += 0.3f * k * sinf(TAU_F * t / 2.2f); L->tail[0] += 0.2f * k * sinf(TAU_F * t / 1.6f);
        if (type == CMO_FLUTTER) {
            L->pos[1] += 0.012f * sinf(TAU_F * 4.5f * rate * t);
            L->side += (0.3f + 0.35f * sinf(TAU_F * 9 * rate * (1 + 0.3f * w[CM_HAPPY]) * t)) * (0.4f + 0.6f * a) * amp;
        }
        break;
    }
    case CMO_WIGGLE: {
        float g = fmaxf(gate(t, 6.1f / rate, 0.4f), 0.5f * w[CM_HAPPY]), k = a * g * amp;
        L->squash += 0.045f * k * sinf(TAU_F * 2.7f * rate * t);
        L->rot[2] += 0.06f * k * sinf(TAU_F * 1.35f * rate * t);
        L->head[2] += 0.05f * k * sinf(TAU_F * 1.35f * rate * t + 1.4f);
        break;
    }
    default: break;
    }
    (void)rt;
}

static void set3(float *d, float x, float y, float z) { d[0] = x; d[1] = y; d[2] = z; }

static void kit_face(cute_rt_t *rt, const pose_t *P, float dt) {
    const cute_spec_t *S = &rt->spec;
    const float t = rt->t, *w = rt->w;
    int mood = P->mood;
    float kb = 0;
    if (rt->blink.t < 0 && t > rt->blink.next) rt->blink.t = 0;
    if (rt->blink.t >= 0) {
        rt->blink.t += dt; kb = bump(rt->blink.t, 0, 0.17f);
        if (rt->blink.t > 0.17f) { rt->blink.t = -1; rt->blink.next = t + rand_ab(rt, 1.8f, 4); }
    }
    bool surprised = mood == CM_SURPRISED, happy = mood == CM_HAPPY, sleepy = mood == CM_SLEEPY, wide = mood == CM_LISTENING;
    float lid_base;
    if (sleepy) { rt->lid_state.sleep = P->lid; rt->lid_state.since = 0; lid_base = P->lid; }
    else {
        rt->lid_state.since += dt; rt->lid_state.sleep *= expf(-dt * (surprised ? 40 : 8));
        lid_base = fmaxf(rt->lid_state.sleep, rt->lid_state.since > 1.1f ? P->lid : 0);
    }
    float lid = clampf(fmaxf(fmaxf(lid_base, kb), rt->beh_lid), 0, 1);
    float shapeY = S->eye_shape == CE_TALL ? 1.2f : S->eye_shape == CE_WIDE ? 0.85f : 1.f;
    float shapeX = S->eye_shape == CE_WIDE ? 1.15f : 1.f;
    for (int e = 0; e < 2; e++) {
        eye_t *E = &rt->eyes[e];
        float u = E->side * S->eye_x + P->look[0] * 0.15f, v = S->eye_y + P->look[1] * 0.13f + (surprised ? 0.03f : 0);
        place(rt, E->g, u, v, 0.006f);
        float r = S->eye_r * (surprised ? 1.28f : wide ? 1.1f : 1.f);
        node_t *in = &rt->nodes[E->inner];
        set3(in->scale, r * shapeX, r * shapeY, r);
        in->rot[2] = S->eye_tilt * E->side;
        node_t *scl = &rt->nodes[E->scl], *ball = &rt->nodes[E->ball], *half = &rt->nodes[E->half];
        node_t *hl1 = &rt->nodes[E->hl1], *hl2 = &rt->nodes[E->hl2], *arc = &rt->nodes[E->arc];
        bool vs = false, vb = false, vh = false, v1 = false, v2 = false, va = false;
        if (happy && lid < 0.9f) {
            va = true; arc->rot[2] = 0; arc->pos[1] = -0.3f; set3(arc->scale, 1, 1.05f, 0.35f);
        } else if (lid > 0.8f) {
            va = true; arc->rot[2] = PI_F; arc->pos[1] = 0.05f; set3(arc->scale, 0.95f, sleepy ? 0.6f : 0.42f, 0.35f);
        } else if (sleepy && lid > 0.45f) {
            vh = v2 = true;
            float o = clampf((1 - lid) * 3.2f, 0.35f, 1);
            set3(half->scale, 1, o, 1); half->pos[1] = -0.05f;
            set3(hl2->pos, 0.25f, -0.05f - o * 0.45f, 0.14f); set3(hl2->scale, 0.14f, 0.14f, 0.14f);
        } else if (surprised) {
            vs = vb = v1 = v2 = true;
            float open = 1 - lid;
            set3(scl->scale, 1, open, 1); scl->pos[1] = -(1 - open);
            set3(ball->scale, 0.58f, 0.58f * open, 1); set3(ball->pos, P->look[0] * 0.12f, -(1 - open) * 0.6f + P->look[1] * 0.1f, 0.06f);
            set3(hl1->pos, ball->pos[0] + 0.2f, ball->pos[1] + 0.2f, 0.14f); set3(hl1->scale, 0.2f, 0.2f, 0.2f);
            set3(hl2->pos, ball->pos[0] - 0.2f, ball->pos[1] - 0.2f, 0.14f); set3(hl2->scale, 0.09f, 0.09f, 0.09f);
        } else {
            float open = 1 - lid; bool hl = S->eye_shine && open > 0.45f;
            vb = true; v1 = v2 = hl;
            set3(ball->scale, 1, open, 1); set3(ball->pos, 0, -(1 - open), 0.06f);
            set3(hl1->pos, 0.32f, 0.34f - (1 - open), 0.14f); set3(hl1->scale, 0.34f, 0.34f, 0.34f);
            set3(hl2->pos, -0.3f, -0.38f, 0.14f); set3(hl2->scale, 0.15f, 0.15f, 0.15f);
        }
        set_vis(rt, E->scl, vs); set_vis(rt, E->ball, vb); set_vis(rt, E->half, vh);
        set_vis(rt, E->hl1, v1); set_vis(rt, E->hl2, v2); set_vis(rt, E->arc, va);
    }
    if (rt->mouth_g >= 0) {
        bool think = w[CM_THINKING] > 0.5f, cat = S->mouth_type == CMT_CAT;
        float mw = S->mouth_w;
        place(rt, rt->mouth_g, think ? 0.12f * (P->look[0] > 0 ? 1.f : -1.f) : 0.f, S->mouth_y + (surprised ? -0.02f : 0), 0.006f);
        float open = P->mouth;
        for (int c = 0; c < rt->n_closed; c++) set_vis(rt, rt->mouth_closed[c], open < 0.07f);
        set_vis(rt, rt->mouth_open, false); set_vis(rt, rt->mouth_openD, false); set_vis(rt, rt->mouth_tongue, false);
        node_t *mo = &rt->nodes[rt->mouth_open], *md = &rt->nodes[rt->mouth_openD], *tg = &rt->nodes[rt->mouth_tongue];
        if (open < 0.07f) {
            float k = think ? 0.6f : 1.f;
            for (int c = 0; c < rt->n_closed; c++) rt->nodes[rt->mouth_closed[c]].scale[0] = mw * (cat ? 0.55f : 1.f) * k;
        } else if (happy) {
            set_vis(rt, rt->mouth_openD, true); set_vis(rt, rt->mouth_tongue, true);
            set3(md->scale, mw * 1.15f, mw * 1.15f, 1); md->pos[1] = mw * 0.35f;
            set3(tg->scale, mw * 0.6f, mw * 0.4f, 1); tg->pos[1] = -mw * 0.45f;
        } else {
            set_vis(rt, rt->mouth_open, true); set_vis(rt, rt->mouth_tongue, open > 0.3f);
            float ox = surprised ? 0.65f : 0.8f + 0.3f * open, oy = surprised ? 0.85f : 0.2f + 0.85f * open;
            set3(mo->scale, mw * ox, mw * oy, 1);
            set3(tg->scale, mw * ox * 0.6f, mw * oy * 0.35f, 1); tg->pos[1] = -mw * oy * 0.5f;
        }
    }
    if (S->mouth_type == CMT_JAW && rt->nsock[CS_JAW]) {
        node_t *j = &rt->nodes[rt->sock[CS_JAW][0].node];
        set3(j->rot, j->brot[0] + P->mouth * S->mouth_open, j->brot[1], j->brot[2]);
    }
    for (int c = 0; c < 2; c++) if (rt->cheeks[c] >= 0) {
        float k = 1 + 0.3f * w[CM_HAPPY] + 0.15f * w[CM_SURPRISED];
        set3(rt->nodes[rt->cheeks[c]].scale, rt->cheek_r * 1.3f * k, rt->cheek_r * k, 1);
    }
}

static void kit_limbs(cute_rt_t *rt, const pose_t *P, const layer_t *L, float dt, float lagX, float lagZ) {
    const cute_spec_t *S = &rt->spec;
    const float t = rt->t, *w = rt->w;
    float flap = 0.55f + 0.5f * sinf(TAU_F * 7 * t);
    float spread = (w[CM_HAPPY] * flap + w[CM_SURPRISED] * 0.95f + w[CM_SPEAKING] * (0.18f + 0.16f * sinf(TAU_F * 1.4f * t)) +
                    w[CM_LISTENING] * 0.12f - w[CM_SLEEPY] * 0.12f + L->side) * S->limb_side;
    for (int r = 0; r < rt->nsock[CS_SIDE]; r++) {
        node_t *n = &rt->nodes[rt->sock[CS_SIDE][r].node]; int sd = rt->sock[CS_SIDE][r].side;
        set3(n->rot, n->brot[0] - 0.15f * w[CM_HAPPY] * S->limb_side * sinf(TAU_F * 7 * t + 1), n->brot[1], n->brot[2] + sd * spread);
    }
    float droop = (w[CM_SLEEPY] * 0.2f - w[CM_LISTENING] * 0.18f - w[CM_SURPRISED] * 0.2f + w[CM_HAPPY] * 0.14f * sinf(TAU_F * 4 * t) + L->ear) * S->limb_ear;
    for (int r = 0; r < rt->nsock[CS_EAR]; r++) {
        node_t *n = &rt->nodes[rt->sock[CS_EAR][r].node]; int sd = rt->sock[CS_EAR][r].side;
        set3(n->rot, n->brot[0] + (w[CM_LISTENING] * -0.25f + lagX * 0.8f) * S->limb_ear, n->brot[1], n->brot[2] - sd * droop + lagZ * 1.3f * S->limb_ear);
    }
    for (int r = 0; r < rt->nsock[CS_ANTENNA]; r++) {
        node_t *n = &rt->nodes[rt->sock[CS_ANTENNA][r].node]; int sd = rt->sock[CS_ANTENNA][r].side;
        set3(n->rot, n->brot[0] + lagX * 1.6f * S->limb_antenna, n->brot[1], n->brot[2] + lagZ * 1.8f * S->limb_antenna - sd * 0.25f * w[CM_SLEEPY]);
    }
    float fk = 0;
    if (rt->flick.t < 0 && t > rt->flick.next) rt->flick.t = 0;
    if (rt->flick.t >= 0) {
        rt->flick.t += dt; fk = bump(rt->flick.t, 0, 0.28f);
        if (rt->flick.t > 0.28f) { rt->flick.t = -1; rt->flick.next = t + rand_ab(rt, 2.5f, 6); }
    }
    float wf = 1.1f + 3 * w[CM_HAPPY] + 1 * w[CM_SPEAKING] - 0.7f * w[CM_SLEEPY], wa = 0.18f + 0.3f * w[CM_HAPPY] - 0.14f * w[CM_SLEEPY];
    rt->wag_ph += TAU_F * wf * dt;
    for (int r = 0; r < rt->nsock[CS_TAIL]; r++) {
        node_t *n = &rt->nodes[rt->sock[CS_TAIL][r].node];
        set3(n->rot, n->brot[0] + (L->tail[0] - 0.55f * fk * (1 - w[CM_SLEEPY]) + 0.25f * w[CM_SLEEPY]) * S->limb_tail,
             n->brot[1] + (wa * sinf(rt->wag_ph) + L->tail[1]) * S->limb_tail, n->brot[2] + L->tail[2] * S->limb_tail);
    }
    float rooty = rt->nodes[rt->n_root].pos[1];
    for (int r = 0; r < rt->nsock[CS_FOOT]; r++) {
        node_t *n = &rt->nodes[rt->sock[CS_FOOT][r].node];
        float lift = (L->foot[r] + fmaxf(0, (rooty - rt->lift) * 0.15f)) * S->limb_foot;
        set3(n->pos, n->bpos[0], n->bpos[1] + lift, n->bpos[2]);
        set3(n->rot, n->brot[0] - lift * 6, n->brot[1], n->brot[2]);
    }
    (void)P;
}

/* ── behaviours (the declarative stand-in for build/update hooks) ──────────────────── */

static float gain(const float *g, const float *w) {
    float v = g[0];
    for (int m = 0; m < CUTE_N_MOODS; m++) v += g[1 + m] * w[m];
    return v;
}

static float term_value(cute_rt_t *rt, const cute_term_t *T, term_state_t *st, int side, float dt, float lagX, float lagZ) {
    float G = gain(T->gain, rt->w);
    if (T->flags & CT_GAIN_B) G *= gain(T->gain_b, rt->w);
    if (T->flags & CT_EASE) {
        if (!st->init) { st->eased = G; st->init = true; }
        st->eased += (G - st->eased) * (dt > 0 ? 1.f - expf(-dt * T->ease) : 1.f);
        G = st->eased;
    }
    float ph = TAU_F * T->freq * rt->t + T->phase + ((T->flags & CT_SIDE_PHASE) && side < 0 ? PI_F : 0.f);
    float d;
    switch (T->driver) {
    case CD_SIN:    d = sinf(ph); break;
    case CD_ABSSIN: d = fabsf(sinf(ph)); break;
    case CD_POSSIN: d = fmaxf(0.f, sinf(ph)); break;
    case CD_SQUARE: d = sinf(ph) > 0 ? 1.f : 0.f; break;
    case CD_MOUTH:  d = rt->pose.mouth; break;
    case CD_LAGX:   d = lagX; break;
    case CD_LAGZ:   d = lagZ; break;
    case CD_PULSE: {
        if (!st->init) { st->next = rt->t + rand_ab(rt, T->p0, T->p1); st->t0 = -1; st->init = true; }
        if (st->t0 < 0 && rt->t > st->next) st->t0 = rt->t;
        d = 0.f;
        if (st->t0 >= 0) {
            float q = (rt->t - st->t0) / fmaxf(T->freq, 0.01f);
            if (q >= 1) { st->t0 = -1; st->next = rt->t + rand_ab(rt, T->p0, T->p1); }
            else d = bump(q, 0, 1);
        }
        break;
    }
    case CD_SPIN: st->spin += dt * T->amp * G; return st->spin * ((T->flags & CT_SIDE_SIGN) ? side : 1);
    default:        d = 1.f; break;
    }
    return T->amp * G * d * ((T->flags & CT_SIDE_SIGN) ? (float)side : 1.f);
}

static void apply_channel(node_t *n, int ch, int op, float v) {
    switch (ch) {
    case CC_RX: case CC_RY: case CC_RZ: { int k = ch - CC_RX; n->rot[k] = (op == CO_SET ? n->brot[k] : n->rot[k]) + v; break; }
    case CC_PX: case CC_PY: case CC_PZ: { int k = ch - CC_PX; n->pos[k] = (op == CO_SET ? n->bpos[k] : n->pos[k]) + v; break; }
    default: {
        bool kx = ch == CC_S || ch == CC_SX || ch == CC_SXZ, ky = ch == CC_S || ch == CC_SY, kz = ch == CC_S || ch == CC_SZ || ch == CC_SXZ;
        float f = 1.f + v;
        if (kx) n->scale[0] = (op == CO_SET ? n->bscale[0] : n->scale[0]) * f;
        if (ky) n->scale[1] = (op == CO_SET ? n->bscale[1] : n->scale[1]) * f;
        if (kz) n->scale[2] = (op == CO_SET ? n->bscale[2] : n->scale[2]) * f;
        break;
    }
    }
}

static void behaviours(cute_rt_t *rt, float dt, float lagX, float lagZ, bool lid_pass) {
    const cute_spec_t *S = &rt->spec;
    for (int i = 0; i < S->nbehav; i++) {
        const cute_behav_t *b = &S->behav[i];
        if ((b->kind == CB_EYES) != lid_pass) continue;
        if (b->kind == CB_EYES) {
            float v = 0; for (int k = 0; k < b->nterms; k++) v += term_value(rt, &b->terms[k], &rt->bst[i][k], 1, dt, lagX, lagZ);
            rt->beh_lid = v;
            continue;
        }
        int targets[24]; int sides[24]; int nt = 0;
        switch (b->kind) {
        case CB_ROOT: targets[nt] = rt->n_root; sides[nt++] = 1; break;
        case CB_HEAD: targets[nt] = rt->n_hpivot; sides[nt++] = 1; break;
        case CB_BODY: targets[nt] = rt->n_body; sides[nt++] = 1; break;
        case CB_SOCKET:
            for (int r = 0; r < rt->nsock[b->index] && nt < 24; r++) { targets[nt] = rt->sock[b->index][r].node; sides[nt++] = rt->sock[b->index][r].side ? rt->sock[b->index][r].side : 1; }
            break;
        case CB_PART:
            for (int m = 0; m < rt->nm && nt < 24; m++) if (rt->meshes[m].spec && rt->meshes[m].name == b->index) { targets[nt] = rt->meshes[m].node; sides[nt++] = rt->meshes[m].side; }
            break;
        }
        for (int q = 0; q < nt; q++) {
            float v = 0;
            for (int k = 0; k < b->nterms; k++) v += term_value(rt, &b->terms[k], &rt->bst[i][k], sides[q], q == 0 ? dt : 0.f, lagX, lagZ);
            apply_channel(&rt->nodes[targets[q]], b->channel, b->op, v);
        }
    }
}

/* ── per frame ──────────────────────────────────────────────────────────────────────── */

static void kit_update(cute_rt_t *rt, float dt, const pose_t *P) {
    const cute_spec_t *S = &rt->spec;
    rt->t += dt;
    const float t = rt->t;
    int mood = P->mood;
    float kw = dt > 0 ? 1.f - expf(-dt * 6) : 1.f;
    for (int m = 0; m < CUTE_N_MOODS; m++) rt->w[m] += ((m == mood ? 1.f : 0.f) - rt->w[m]) * kw;
    const float *w = rt->w;
    float act = clampf(1 + 0.35f * w[CM_HAPPY] - 0.95f * w[CM_SLEEPY] - 0.5f * w[CM_LISTENING], 0, 1.5f);
    layer_t L; memset(&L, 0, sizeof(L));
    motion(rt, &L, t, act, w, S->motion, S->motion_amp, S->motion_rate, S->motion_height);
    if (rt->tilt.t < 0 && t > rt->tilt.next) { rt->tilt.t = 0; rt->tilt.dir = rnd(rt) < 0.5f ? -1 : 1; }
    if (rt->tilt.t >= 0) {
        rt->tilt.t += dt; float q = rt->tilt.t / 1.5f;
        L.head[2] += 0.2f * rt->tilt.dir * sinf(PI_F * fminf(1, q)) * (w[CM_IDLE] + 0.6f * w[CM_LISTENING]);
        if (q >= 1) { rt->tilt.t = -1; rt->tilt.next = t + rand_ab(rt, 3.5f, 7); }
    }
    L.pos[1] += 0.35f * w[CM_HAPPY] * P->pos[1];
    rt->lift += (L.lift - rt->lift) * kw;

    node_t *root = &rt->nodes[rt->n_root];
    set3(root->pos, P->pos[0] + L.pos[0], P->pos[1] + L.pos[1] + rt->lift, P->pos[2] + L.pos[2]);
    set3(root->rot, P->rot[0] + L.rot[0], P->rot[1] + L.rot[1], P->rot[2] + L.rot[2]);
    set3(rt->nodes[rt->n_lean].rot, P->spine[0] * 0.5f, P->spine[1] * 0.5f, P->spine[2] * 0.5f);
    float sq = (P->squash + L.squash) * 1.25f, sy = 1 + sq, sx = 1.f / sqrtf(fmaxf(0.3f, sy));
    set3(rt->nodes[rt->n_body].scale, sx, sy, sx);
    float hsy = 1 + sq * S->head_squash, hsx = 1.f / sqrtf(fmaxf(0.3f, hsy));
    node_t *hp = &rt->nodes[rt->n_hpivot];
    set3(hp->pos, rt->neck[0], rt->neck[1] * (rt->has_body ? sy : 1.f), rt->neck[2]);
    set3(hp->scale, hsx, hsy, hsx);
    set3(hp->rot, P->head[0] * S->head_pitch + L.head[0], P->head[1] + L.head[1], P->head[2] + L.head[2]);

    float roll = P->head[2] + L.head[2] + P->rot[2] + L.rot[2], pitch = P->head[0] + L.head[0];
    float vy = dt > 0 ? (root->pos[1] - rt->spring.last_y) / dt : 0; rt->spring.last_y = root->pos[1];
    if (dt > 0) {
        rt->spring.vz += (140 * (roll - rt->spring.z) - 7 * rt->spring.vz) * dt; rt->spring.z += rt->spring.vz * dt;
        rt->spring.vx += (140 * (pitch - rt->spring.x) - 7 * rt->spring.vx) * dt - vy * 0.9f * dt * 60 * 0.02f; rt->spring.x += rt->spring.vx * dt;
    }
    float lagZ = clampf(rt->spring.z - roll, -0.6f, 0.6f), lagX = clampf(rt->spring.x - pitch, -0.6f, 0.6f);

    /* behaviours on the eyes run before the face (they feed its lid) */
    behaviours(rt, dt, lagX, lagZ, true);
    kit_face(rt, P, dt);
    kit_limbs(rt, P, &L, dt, lagX, lagZ);
    /* part meshes return to rest; then the update-hook stand-ins */
    for (int m = 0; m < rt->nm; m++) if (rt->meshes[m].spec) {
        node_t *n = &rt->nodes[rt->meshes[m].node];
        memcpy(n->pos, n->bpos, sizeof(n->pos)); memcpy(n->rot, n->brot, sizeof(n->rot)); memcpy(n->scale, n->bscale, sizeof(n->scale));
    }
    behaviours(rt, dt, lagX, lagZ, false);
}

static void world_update(cute_rt_t *rt) {
    for (int i = 0; i < rt->nn; i++) {
        node_t *n = &rt->nodes[i];
        float R[9];
        bool rotated = n->rot[0] != 0.f || n->rot[1] != 0.f || n->rot[2] != 0.f;
        if (n->use_rm) {
            memcpy(R, n->rm, sizeof(R));
            if (rotated) { float E[9]; euler_mat(E, n->rot, n->order); mat3_mul(R, R, E); }
        } else if (rotated) euler_mat(R, n->rot, n->order);
        else { static const float I[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }; memcpy(R, I, sizeof(R)); }
        m34_t L = { { R[0] * n->scale[0], R[1] * n->scale[1], R[2] * n->scale[2], n->pos[0],
                      R[3] * n->scale[0], R[4] * n->scale[1], R[5] * n->scale[2], n->pos[1],
                      R[6] * n->scale[0], R[7] * n->scale[1], R[8] * n->scale[2], n->pos[2] } };
        n->world = n->parent >= 0 ? m_mul(&rt->nodes[n->parent].world, &L) : L;
    }
}

void cute_rt_update(cute_rt_t *rt, float dt) {
    if (!rt) return;
    FLUID_PROF_BEGIN(c0);
    dt = fminf(dt, 1.f / 30.f);                        /* CreatureTarget.update clamps both */
    engine_update(rt, dt, &rt->pose);
    kit_update(rt, dt, &rt->pose);
    world_update(rt);
    FLUID_PROF_END(FP_C_UPDATE, c0);
}

/* ── create: rig, sockets, parts, face, normalisation, camera fit ─────────────────── */

static void rest_bbox(cute_rt_t *rt, float *mn, float *mx) {
    for (int k = 0; k < 3; k++) { mn[k] = 1e9f; mx[k] = -1e9f; }
    for (int m = 0; m < rt->nm; m++) {
        const mesh_t *me = &rt->meshes[m];
        if (!me->spec) continue;
        const geo_t *g = &rt->geos[me->geo];
        const m34_t *W = &rt->nodes[me->node].world;
        for (int c = 0; c < 8; c++) {   /* box corners → world AABB (three Box3.applyMatrix4) */
            v3_t p = m_apply(W, (c & 1) ? g->bmax[0] : g->bmin[0], (c & 2) ? g->bmax[1] : g->bmin[1], (c & 4) ? g->bmax[2] : g->bmin[2]);
            float v[3] = { p.x, p.y, p.z };
            for (int k = 0; k < 3; k++) { if (v[k] < mn[k]) mn[k] = v[k]; if (v[k] > mx[k]) mx[k] = v[k]; }
        }
    }
}

static void fit_camera(cute_rt_t *rt) {
    /* targets.js CreatureTarget._frame: fit a circle around the motion envelope, view dir (0.38, 0.28, 1) */
    float lo[3] = { fminf(rt->env_min[0], -0.36f), 0, fminf(rt->env_min[2], -0.36f) };
    float hi[3] = { fmaxf(rt->env_max[0], 0.36f), fmaxf(rt->env_max[1], 1.26f), fmaxf(rt->env_max[2], 0.36f) };
    float rx = fmaxf(-lo[0], hi[0]), rz = fmaxf(-lo[2], hi[2]), rest_top = hi[1] - rt->rise;
    float d[3] = { 0.38f, 0.28f, 1.f }, dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int k = 0; k < 3; k++) rt->cam_z[k] = d[k] / dl;
    set3(rt->cam_t, 0, (lo[1] + hi[1]) * 0.5f - 0.03f, 0);
    /* lookAt: x = normalize(up × z), y = z × x */
    float *z = rt->cam_z, x[3] = { z[2], 0, -z[0] }, xl = sqrtf(x[0] * x[0] + x[2] * x[2]);
    x[0] /= xl; x[2] /= xl;
    memcpy(rt->cam_x, x, sizeof(x));
    rt->cam_y[0] = z[1] * x[2] - z[2] * x[1]; rt->cam_y[1] = z[2] * x[0] - z[0] * x[2]; rt->cam_y[2] = z[0] * x[1] - z[1] * x[0];
    float half = 0;
    const float ys[3] = { lo[1], rest_top * 0.55f, hi[1] }, rs[3] = { 0.75f, 1.f, 0.55f };
    for (int i = 0; i < 16; i++) {
        float a = (float)i / 16 * TAU_F, cx = cosf(a) * rx, cz = sinf(a) * rz;
        for (int q = 0; q < 3; q++) {
            float p[3] = { cx * rs[q] - rt->cam_t[0], ys[q] - rt->cam_t[1], cz * rs[q] - rt->cam_t[2] };
            float u = p[0] * rt->cam_x[0] + p[1] * rt->cam_x[1] + p[2] * rt->cam_x[2];
            float v = p[0] * rt->cam_y[0] + p[1] * rt->cam_y[1] + p[2] * rt->cam_y[2];
            float hh = sqrtf(u * u + v * v); if (hh > half) half = hh;
        }
    }
    rt->cam_half = half * 0.98f;
}

cute_rt_t *cute_rt_create(const cute_spec_t *S, uint32_t seed) {
    cute_rt_t *rt = CUTE_CALLOC(sizeof(cute_rt_t));
    if (!rt) return NULL;
    rt->spec = *S;
    rt->rng = seed ? seed : 0x9e3779b9u;
    memcpy(rt->H, S->head_at, sizeof(rt->H)); memcpy(rt->neck, S->head_pivot, sizeof(rt->neck));
    rt->has_body = S->has_body;
    rt->n_root = node_add(rt, -1);
    rt->n_norm = node_add(rt, rt->n_root);
    rt->n_lean = node_add(rt, rt->n_norm);
    rt->n_body = node_add(rt, rt->n_lean);
    rt->n_hpivot = node_add(rt, rt->n_lean);
    rt->n_head = node_add(rt, rt->n_hpivot);
    rt->nodes[rt->n_hpivot].order = 1;                 /* 'YXZ' */
    memcpy(rt->nodes[rt->n_hpivot].pos, rt->neck, sizeof(rt->neck));
    for (int k = 0; k < 3; k++) rt->nodes[rt->n_head].pos[k] = rt->H[k] - rt->neck[k];
    /* sockets */
    const int parent_of[3] = { rt->n_norm, rt->n_body, rt->n_head };
    rt->sock[CS_ROOT][0] = (srec_t){ rt->n_norm, 0 }; rt->nsock[CS_ROOT] = 1;
    rt->sock[CS_BODY][0] = (srec_t){ rt->n_body, 0 }; rt->nsock[CS_BODY] = 1;
    rt->sock[CS_HEAD][0] = (srec_t){ rt->n_head, 0 }; rt->nsock[CS_HEAD] = 1;
    for (int s = CS_TOP; s < CUTE_N_SOCKETS; s++) {
        const cute_socket_t *c = &S->sockets[s];
        int on = c->on > CON_HEAD ? CON_BODY : c->on;
        const float org[3] = { on == CON_HEAD ? rt->H[0] : 0, on == CON_HEAD ? rt->H[1] : 0, on == CON_HEAD ? rt->H[2] : 0 };
        int nsides = paired(s) ? 2 : 1;
        for (int q = 0; q < nsides; q++) {
            int side = paired(s) ? (q == 0 ? 1 : -1) : 0;
            int nd = node_add(rt, parent_of[on]);
            node_t *n = &rt->nodes[nd];
            set3(n->pos, c->pos[0] * (side ? side : 1) - org[0], c->pos[1] - org[1], c->pos[2] - org[2]);
            set3(n->rot, c->rot[0], side < 0 ? -c->rot[1] : c->rot[1], side < 0 ? -c->rot[2] : c->rot[2]);
            n->side = (int8_t)(side ? side : 1);
            node_rest(n);
            rt->sock[s][q] = (srec_t){ nd, side };
        }
        rt->nsock[s] = nsides;
    }
    for (int i = 0; i < S->nparts; i++) add_part(rt, &S->parts[i]);
    build_face(rt);
    for (int i = 0; i < rt->nn; i++) if (!rt->nodes[i].use_rm) node_rest(&rt->nodes[i]);

    /* kit state + engine */
    rt->blink.next = rand_ab(rt, 1.5f, 3.5f); rt->blink.t = -1;
    rt->tilt.next = rand_ab(rt, 3, 6); rt->tilt.t = -1; rt->tilt.dir = 1;
    rt->flick.next = rand_ab(rt, 2, 5); rt->flick.t = -1;
    rt->lid_state.since = 9;
    rt->w[CM_IDLE] = 1;
    engine_init(rt);
    zero_pose(&rt->pose);

    /* normalise in the rest pose: height 1, or bulk / 0.9 for squat wide creatures */
    kit_update(rt, 0, &rt->pose);
    set3(rt->nodes[rt->n_root].pos, 0, 0, 0); set3(rt->nodes[rt->n_root].rot, 0, 0, 0);
    world_update(rt);
    float mn[3], mx[3];
    rest_bbox(rt, mn, mx);
    float hgt = mx[1] > 0 ? mx[1] : 1.f, sxw = mx[0] - mn[0], szw = mx[2] - mn[2];
    float bulk = cbrtf(fmaxf(1e-6f, hgt * sxw * szw));
    float k = 1.f / fmaxf(hgt, bulk / 0.9f);
    rt->norm_k = k;
    set3(rt->nodes[rt->n_norm].scale, k, k, k); node_rest(&rt->nodes[rt->n_norm]);
    bool fl = S->motion == CMO_FLOAT || S->motion == CMO_FLUTTER;
    rt->rise = 0.22f + 0.08f * hgt * k + (fl ? S->motion_height + 0.04f * S->motion_amp : S->motion == CMO_HOP ? 0.02f : 0.015f);
    for (int q = 0; q < 3; q++) { rt->env_min[q] = mn[q] * k; rt->env_max[q] = mx[q] * k; }
    rt->env_max[1] += rt->rise;
    fit_camera(rt);
    kit_update(rt, 0, &rt->pose);
    world_update(rt);
    return rt;
}

void cute_rt_destroy(cute_rt_t *rt) {
    if (!rt) return;
    for (int i = 0; i < rt->ng; i++) { CUTE_FREE(rt->geos[i].v); CUTE_FREE(rt->geos[i].t); }
    CUTE_FREE(rt->zbuf); CUTE_FREE(rt->cbuf);
    CUTE_FREE(rt->cov); CUTE_FREE(rt->vbuf);
    CUTE_FREE(rt);
}

const char *cute_rt_name(const cute_rt_t *rt) { return rt ? rt->spec.name : ""; }
int cute_rt_tris(const cute_rt_t *rt) { return rt ? rt->tris : 0; }

/* ── raster (CreatureTarget.update: flat Lambert, key (0.5,1,1) ×1.0 + ambient 0.4) ──── */

static bool raster_alloc(cute_rt_t *rt, int W, int H) {
    if (rt->rW == W && rt->rH == H && rt->zbuf) return true;
    CUTE_FREE(rt->zbuf); CUTE_FREE(rt->cbuf); CUTE_FREE(rt->cov);
    size_t ns = (size_t)W * 2 * H * 4, nc = (size_t)W * H;
    rt->zbuf = CUTE_CALLOC(ns * sizeof(float));
    rt->cbuf = CUTE_CALLOC(ns * sizeof(uint32_t));
    rt->cov = CUTE_CALLOC(nc * (5 * sizeof(float) + 1));
    if (!rt->zbuf || !rt->cbuf || !rt->cov) { rt->rW = rt->rH = 0; return false; }
    rt->lum = rt->cov + nc; rt->cr = rt->lum + nc; rt->cg = rt->cr + nc; rt->cb = rt->cg + nc;
    rt->feat = (uint8_t *)(rt->cb + nc);
    if (!rt->vbuf) {
        int cap = 0;
        for (int i = 0; i < rt->ng; i++) if (rt->geos[i].nv > cap) cap = rt->geos[i].nv;
        rt->vbuf = CUTE_CALLOC((size_t)cap * 6 * sizeof(float));
        rt->vcap = rt->vbuf ? cap : 0;
    }
    rt->rW = W; rt->rH = H;
    return true;
}

void cute_rt_raster(cute_rt_t *rt, int W, int H) {
    if (!rt || !raster_alloc(rt, W, H)) return;
    FLUID_PROF_BEGIN(c0);
    const int SW = W * 2, SH = H * 4, NS = SW * SH;
    uint32_t cc = fluid_cycles();
    /* alpha byte = this frame's tag ⇒ covered (no 53 KB clear per frame; full clear on wrap) */
    if (++rt->tag == 0) { memset(rt->cbuf, 0, (size_t)NS * sizeof(uint32_t)); rt->tag = 1; }
    const uint32_t tagbits = (uint32_t)rt->tag << 24;
    fluid_prof[FP_C_CLEAR] += fluid_cycles() - cc;
    static const float LD[3] = { 0.33333333f, 0.66666667f, 0.66666667f };   /* normalize(0.5, 1, 1) */
    const float inv_h = (1.f + rt->cam_zoom) / rt->cam_half, hx = 0.5f * SW * inv_h, hy = 0.5f * SH * inv_h;
    /* the camera's centre: the usual one, or (an action's focus) on the eye nearer the camera */
    float ct[3] = { rt->cam_t[0], rt->cam_t[1], rt->cam_t[2] };
    if (rt->cam_focus > 0.f) {
        v3_t eye = m_apply(&rt->nodes[rt->eyes[0].g].world, 0.f, 0.f, 0.f);
        ct[0] += rt->cam_focus * (eye.x - ct[0]); ct[1] += rt->cam_focus * (eye.y - ct[1]); ct[2] += rt->cam_focus * (eye.z - ct[2]);
    }
    int tris = 0;
    int bx0 = SW, bx1 = 0, by0 = SH, by1 = 0;           /* union of rasterised bboxes */
    for (int m = 0; m < rt->nm; m++) {
        const mesh_t *me = &rt->meshes[m];
        if (!me->visible) continue;
        const geo_t *g = &rt->geos[me->geo];
        const m34_t *Wm = &rt->nodes[me->node].world;
        float det = m_det(Wm);
        if (fabsf(det) < 1e-12f || g->nv > rt->vcap) continue;  /* collapsed (scale 0) */
        uint32_t cv = fluid_cycles();
        /* camera-space vertices, once per mesh: screen x, y (samples), depth z; world p for normals */
        for (int i = 0; i < g->nv; i++) {
            const float *v = g->v + i * 3;
            v3_t p = m_apply(Wm, v[0], v[1], v[2]);
            float q[3] = { p.x - ct[0], p.y - ct[1], p.z - ct[2] };
            float *o = rt->vbuf + i * 6;
            o[0] = (q[0] * rt->cam_x[0] + q[1] * rt->cam_x[1] + q[2] * rt->cam_x[2]) * hx + 0.5f * SW;
            o[1] = 0.5f * SH - (q[0] * rt->cam_y[0] + q[1] * rt->cam_y[1] + q[2] * rt->cam_y[2]) * hy;
            o[2] = q[0] * rt->cam_z[0] + q[1] * rt->cam_z[1] + q[2] * rt->cam_z[2];
            o[3] = p.x; o[4] = p.y; o[5] = p.z;
        }
        const uint8_t *rgb = rt->spec.pal_rgb[me->color];
        const bool flip = det < 0.f;
        for (int t = 0; t < g->nt; t++) {
            const float *A = rt->vbuf + g->t[t * 3] * 6, *B = rt->vbuf + g->t[t * 3 + 1] * 6, *C = rt->vbuf + g->t[t * 3 + 2] * 6;
            /* back-face cull: CCW in y-up NDC = front (three FrontSide; mirrored transforms flip it) */
            float area = (B[0] - A[0]) * (C[1] - A[1]) - (C[0] - A[0]) * (B[1] - A[1]);   /* y-down: front ⇔ area < 0 */
            if (area == 0.f || ((area < 0.f) == flip)) continue;
            /* bbox clamped to the grid first, so the int casts below are plain floor/ceil */
            float fx0 = fmaxf(0.f, fminf(A[0], fminf(B[0], C[0]))), fx1 = fminf((float)SW, fmaxf(A[0], fmaxf(B[0], C[0])));
            float fy0 = fmaxf(0.f, fminf(A[1], fminf(B[1], C[1]))), fy1 = fminf((float)SH, fmaxf(A[1], fmaxf(B[1], C[1])));
            int x0 = (int)fx0, y0 = (int)fy0, x1 = (int)fx1, y1 = (int)fy1;
            if ((float)x1 < fx1) x1++;
            if ((float)y1 < fy1) y1++;
            if (x0 >= x1 || y0 >= y1) continue;
            /* flat Lambert: the face normal, facing the camera */
            float e1[3] = { B[3] - A[3], B[4] - A[4], B[5] - A[5] }, e2[3] = { C[3] - A[3], C[4] - A[4], C[5] - A[5] };
            float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
            float nl2 = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
            if (nl2 < 1e-24f) continue;
            float nd = n[0] * LD[0] + n[1] * LD[1] + n[2] * LD[2];
            if (n[0] * rt->cam_z[0] + n[1] * rt->cam_z[1] + n[2] * rt->cam_z[2] < 0) nd = -nd;
            float shade = nd > 0.f ? 0.4f + nd / sqrtf(nl2) : 0.4f;
            if (shade > 1.f) shade = 1.f;
            uint32_t col = (uint32_t)(rgb[0] * shade) | (uint32_t)(rgb[1] * shade) << 8 | (uint32_t)(rgb[2] * shade) << 16 | tagbits;
            tris++;
            if (x0 < bx0) bx0 = x0;
            if (x1 > bx1) bx1 = x1;
            if (y0 < by0) by0 = y0;
            if (y1 > by1) by1 = y1;
            uint32_t cs = fluid_cycles();
            /* edge functions stepped across the bbox at sample centres; z-test (larger z = nearer) */
            float ia = 1.f / area;
            float a0x = (B[1] - C[1]) * ia, a0y = (C[0] - B[0]) * ia;   /* ∂w0/∂x, ∂w0/∂y */
            float a1x = (C[1] - A[1]) * ia, a1y = (A[0] - C[0]) * ia;
            float px0 = x0 + 0.5f, py0 = y0 + 0.5f;
            float w0r = ((B[0] - px0) * (C[1] - py0) - (C[0] - px0) * (B[1] - py0)) * ia;
            float w1r = ((C[0] - px0) * (A[1] - py0) - (A[0] - px0) * (C[1] - py0)) * ia;
            for (int y = y0; y < y1; y++, w0r += a0y, w1r += a1y) {
                float w0 = w0r, w1 = w1r;
                float *zr = rt->zbuf + y * SW; uint32_t *cr = rt->cbuf + y * SW;
                for (int x = x0; x < x1; x++, w0 += a0x, w1 += a1x) {
                    float w2 = 1.f - w0 - w1;
                    if (w0 < 0.f || w1 < 0.f || w2 < 0.f) continue;
                    float z = w0 * A[2] + w1 * B[2] + w2 * C[2];
                    if ((cr[x] & 0xff000000u) == tagbits && z <= zr[x]) continue;
                    zr[x] = z; cr[x] = col;
                }
            }
            uint32_t ce = fluid_cycles();
            fluid_prof[FP_C_SAMPLES] += ce - cs; cv += ce - cs;   /* keep sample time out of c_vert */
        }
        fluid_prof[FP_C_VERT] += fluid_cycles() - cv;
    }
    uint32_t ca = fluid_cycles();
    rt->tris = tris;
    /* per cell: coverage, mean colour, luminance; dark covered cells = features (eyes).
     * Row-sequential integer sums (the sample rows are read in memory order) and a
     * reciprocal table: float divide is a soft-float call on this core. */
    static const float INV255N[9] = { 0, 1.f / 255, 1.f / 510, 1.f / 765, 1.f / 1020, 1.f / 1275, 1.f / 1530, 1.f / 1785, 1.f / 2040 };
    float lo = 1, hi = 0;
    /* only the rows/columns something was rasterised into this frame are read (PSRAM) */
    const int cj0 = by0 / 4, cj1 = (by1 + 3) / 4, sx0 = bx0 & ~1, sx1 = bx1 < SW ? ((bx1 + 1) & ~1) : SW;
    for (int j = 0; j < H; j++) {
        uint32_t acc[FLUID_GRID_MAX_W][4];
        memset(acc, 0, sizeof(uint32_t) * 4 * (size_t)W);
        for (int r = 0; r < 4 && j >= cj0 && j < cj1; r++) {
            const uint32_t *row = rt->cbuf + (j * 4 + r) * SW;
            for (int x = sx0; x < sx1; x++) {
                uint32_t c = row[x];
                if ((c & 0xff000000u) != tagbits) continue;
                uint32_t *a = acc[x >> 1];
                a[0]++; a[1] += c & 255; a[2] += c >> 8 & 255; a[3] += c >> 16 & 255;
            }
        }
        for (int i = 0; i < W; i++) {
            const uint32_t *a = acc[i];
            int k = j * W + i, n = (int)a[0];
            float inv = INV255N[n];
            float r = a[1] * inv, g = a[2] * inv, b = a[3] * inv;
            rt->cov[k] = n * 0.125f;
            rt->cr[k] = r; rt->cg[k] = g; rt->cb[k] = b;
            float l = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            rt->lum[k] = l;
            rt->feat[k] = n > 4 && l < 0.22f;
            if (n > 4) { if (l < lo) lo = l; if (l > hi) hi = l; }
        }
    }
    float inv_span = 1.f / fmaxf(0.15f, hi - lo);
    for (int k = 0; k < W * H; k++) rt->lum[k] = clampf((rt->lum[k] - lo) * inv_span, 0, 1);
    fluid_prof[FP_C_AGG] += fluid_cycles() - ca;
    FLUID_PROF_END(FP_C_RASTER, c0);
}

static float s_pow13[65];   /* l^1.3, 64 segments */
static inline float pow13(float l) {
    if (s_pow13[64] == 0.f) for (int i = 0; i <= 64; i++) s_pow13[i] = powf(i / 64.f, 1.3f);
    float f = clampf(l, 0, 1) * 64.f; int i = (int)f; if (i > 63) i = 63;
    return s_pow13[i] + (s_pow13[i + 1] - s_pow13[i]) * (f - i);
}

void cute_rt_fill(cute_rt_t *rt, fluid_sim_t *s, float swell) {
    if (!rt || !s || rt->rW != s->W || rt->rH != s->H) return;
    FLUID_PROF_BEGIN(c0);
    const int W = s->W, H = s->H;
    const float inv = 1.f / swell, xmax = W - 1.001f, ymax = H - 1.001f;
    for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) {
        const int k = j * W + i;
        s->tf[k] = 0;
        if (!s->mask[k]) { s->T[k] = 0; continue; }
        float fx = ((s->xs[i] * inv + 1) * 0.5f) * W - 0.5f, fy = ((s->ys[j] * inv + 1) * 0.5f) * H - 0.5f;
        fx = clampf(fx, 0, xmax); fy = clampf(fy, 0, ymax);
        int x0 = (int)fx, y0 = (int)fy, q = y0 * W + x0;
        float ax = fx - x0, ay = fy - y0;
#define BL(a) (((a)[q] * (1 - ax) + (a)[q + 1] * ax) * (1 - ay) + ((a)[q + W] * (1 - ax) + (a)[q + W + 1] * ax) * ay)
        float c = BL(rt->cov), l = BL(rt->lum);
        float T = c > 0.f ? c * (0.06f + 0.94f * pow13(l)) : 0.f;
        s->T[k] = fq(T, FQ_UNIT);
        int qn = (int)(fy + 0.5f) * W + (int)(fx + 0.5f);
        s->tf[k] = rt->feat[qn];
        if (c > 0.05f) {
            float d = c >= 0.25f ? 1.f / c : 4.f;
            s->tcol[k] = fluid_pack_rgba8(BL(rt->cr) * d, BL(rt->cg) * d, BL(rt->cb) * d, 0.f);
        }
#undef BL
    }
    FLUID_PROF_END(FP_C_FILL, c0);
}
