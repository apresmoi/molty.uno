/*
 * SPDX-License-Identifier: MIT
 *
 * creature_dots.h: a rigged creature drawn straight onto the braille lattice (docs/CREATURES.md section 12).
 * The C port of device/creatures/js/clip-dots.js: the body's rounded primitives projected each frame (a metaball
 * field for the merged body, solid parts for the rest), one outline with inner lines where a nearer part crosses,
 * the features (eyes, nose, inner ears, the chest patch's edge, whiskers, the props), then the world under it: a
 * static perspective floor, shimmering dust and a vignette, kept off the cat by a two-dot halo.
 *
 * Budget: the per-dot work runs only over the creature's box (its primitives' projected bounds), 8 bytes a dot in PSRAM;
 * the floor is a layer redrawn only when the camera moves along it, the dust a fixed set of motes. On ESP the
 * row-parallel passes split into two bands, the second on core 0 (a task made with cd_create).
 *
 * Plain C, no ESP-IDF (host-testable).
 */
#pragma once

#include <stdint.h>
#include "creature_clip.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CD_VIEW_SIZE 1.9f        /* world units across the panel                                      */
#define CD_FLOOR_AT  0.8f        /* the floor under the creature, as a fraction of the panel's height  */

typedef struct cd cd_t;

/** For W x H cells (2W x 4H dots). NULL on OOM. */
cd_t *cd_create(const cc_t *c, int W, int H);
void  cd_destroy(cd_t *d);
/** Draw the frame: c's world matrices from its camera; frame counts frames (the dust's shimmer). */
void  cd_draw(cd_t *d, const cc_t *c, uint32_t frame);
/** The cells of the last draw: a braille byte (fluid_braille_bit order) and a colour 0xRRGGBB a cell. */
void  cd_cells(const cd_t *d, const uint8_t **dots, const uint32_t **rgb);
#define CD_STAGES 7
/** Cycles spent per stage since the last call, added into out[CD_STAGES]: projection, the body and the solid parts, lines,
 *  features, the halo's rows, the halo's columns with the world and the cells; then, within those, the waits for the
 *  second band (the other core, ESP). */
void  cd_prof(cd_t *d, uint32_t *out);

#ifdef __cplusplus
}
#endif
