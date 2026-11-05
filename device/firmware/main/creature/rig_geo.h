/*
 * SPDX-License-Identifier: MIT
 *
 * rig_geo.h: small linear algebra (3×4 affine, three.js Euler orders) and the three.js
 * geometry generators (same vertex and index order, same winding), shared by the .molty
 * runtime (cute_rig.c) and the creature package body (creature_body.c). Plain C.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* row-major [r00 r01 r02 tx | r10 … ty | r20 … tz] */
typedef struct { float m[12]; } m34_t;
typedef struct { float x, y, z; } v3_t;

m34_t rg_mul(const m34_t *a, const m34_t *b);
static inline v3_t rg_apply(const m34_t *a, float x, float y, float z) {
    v3_t r = { a->m[0] * x + a->m[1] * y + a->m[2] * z + a->m[3],
               a->m[4] * x + a->m[5] * y + a->m[6] * z + a->m[7],
               a->m[8] * x + a->m[9] * y + a->m[10] * z + a->m[11] };
    return r;
}
float rg_det(const m34_t *a);
void  rg_rot_axis(float *R, int axis, float a);
void  rg_mat3_mul(float *o, const float *a, const float *b);
/* 3×3 rotation of euler e in order 0 'XYZ', 1 'YXZ', 2 'ZXY', 3 'XZY', 4 'YZX', 5 'ZYX' */
void  rg_euler(float *R, const float *e, int order);
/* quaternion.setFromUnitVectors(+z, n) as a 3×3 rotation */
void  rg_rot_from_z(float *R, v3_t n);

/* a mesh: nv vertices (xyz), nt triangles (CCW = front), its bounds */
typedef struct { int nv, nt; float *v; uint16_t *t; float bmin[3], bmax[3]; } geo_t;

bool rg_sphere(geo_t *g, int w, int h, const float *s, float pinch, bool tear);   /* SphereGeometry(1, w, h) scaled by s */
bool rg_cylinder(geo_t *g, float rt, float rb, float h, int rs);                  /* CylinderGeometry; ConeGeometry = rt 0; rs <= 32 */
bool rg_capsule(geo_t *g, float r, float len, int cap, int rs);
bool rg_torus(geo_t *g, float R, float tube, int radial, int tub, float arc);
bool rg_circle(geo_t *g, float r, int seg, float t0, float tl);
bool rg_box(geo_t *g, const float *s);
/* TubeGeometry(CatmullRomCurve3(path) centripetal, seg, r, 6 radial, open) */
bool rg_tube(geo_t *g, const float (*path)[3], int n, int seg, float r);

#ifdef __cplusplus
}
#endif
