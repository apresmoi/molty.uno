/*
 * SPDX-License-Identifier: MIT
 *
 * creature_body.h: a creature package's body (bones and v2 parts) posed by its player and
 * drawn into the fluid's target, the way the lab's renderer draws it
 * (molty-creature-lab/renderer.mjs + molty2-runtime.mjs, the look the owner approved):
 *
 *   rig       bones in a tree, rest pose plus the player's outputs; parts (ellipsoid, box,
 *             cone, cylinder, tube) meshed as three.js meshes them, on their bones
 *   camera    orthographic, from (0.16, 0.2, 1), fitted once at rest so the body fills 80 %
 *             of the round glass; an action's zoom and focus (the inspect gesture) on top
 *   light     flat Lambert, ambient 0.4 + key 1.15 from the lab's dither light, coming round
 *             to the front as the body turns side on; tints from the player recolour the parts
 *   cells     2×2 samples a cell: coverage, colour, luminance, auto-exposure, then the lab's
 *             "dither" style (lifted floor, saturated hues, shadows deeper not greyer, warm
 *             highlights, softer silhouette) and very dark covered cells as holes (the eyes)
 *   glass     the ambient's marks (bubbles, stars, ink) stamped over the cells
 *
 * Plain C, no ESP-IDF (host-testable).
 */
#pragma once

#include "creature_player.h"
#include "fluid_sim.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CB_WINDOW 0.85f   /* the ambient's window: radius of the glass in its units      */

typedef struct cb cb_t;

/** Mesh the body of c's package and fit the camera at rest. NULL on OOM. */
cb_t *cb_create(const cp_t *c);
void  cb_destroy(cb_t *b);
/** Pose from the player's last frame and rasterise into W×H cells; zoom ≥ 0 enlarges
 *  (1 + zoom), focus 0..1 moves the view toward the eye (cute_act_pose_t). */
void  cb_raster(cb_t *b, const cp_t *c, int W, int H, float zoom, float focus);
/** The body's coverage of each cell from the last raster (the ambient's env.cov). */
const float *cb_cov(const cb_t *b);
/** Stamp marks over the cells, shallowest d first (the lab's stamp). */
void  cb_stamp(cb_t *b, const cp_mark_t *m, int n);
/** Write the fluid target: T, tcol; feature cells are holes. swell breathes the target
 *  about the centre. */
void  cb_fill(cb_t *b, fluid_sim_t *s, float swell);
/** Triangles drawn in the last raster. */
int   cb_tris(const cb_t *b);
/** The cells of the last raster + stamp: coverage, luminance, colour (0..1), holes. */
void  cb_cells(const cb_t *b, const float **cov, const float **lum, const float **r, const float **g, const float **bl, const uint8_t **hole);
/** Where point `at` on bone `bone` (its local coordinates; nbones = the root) was in the last
 *  raster, framed as at rest (no zoom): out x right, y down, 0..1 across the glass, and its
 *  height in the body's units (the floor at 0). */
void  cb_glass(const cb_t *b, int bone, const float at[3], float out[3]);

#ifdef __cplusplus
}
#endif
