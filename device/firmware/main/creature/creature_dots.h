/*
 * SPDX-License-Identifier: MIT
 *
 * creature_dots.h: a rigged creature drawn straight onto the braille lattice (docs/CREATURES.md section 12).
 * The C port of device/creatures/js/clip-dots.js: the body's rounded primitives projected each frame (a metaball
 * field for the merged body, solid parts for the rest), one outline with inner lines where a nearer part crosses,
 * the features (eyes, nose, inner ears, the chest patch's edge, whiskers, the props), then the world under it: a
 * static perspective floor, shimmering dust and a vignette, kept off the cat by a two-dot halo.
 *
 * Budget: the per-dot work runs only over the creature's box (its primitives' projected bounds); the floor and dust
 * are a hash per dot of the disc, written straight into the cells. Buffers in PSRAM, about 10 bytes a dot.
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
/** The creature's own dots of the last draw (not the world): on (0/1), colour 0xRRGGBB, priority (1 whisker, 2 line, 3+ features). */
void  cd_dots(const cd_t *d, const uint8_t **on, const uint32_t **rgb, const uint8_t **pri);
/** Cycles spent per stage since the last call (box, body, parts, lines, features, halo, world, cells), added into out[8]. */
void  cd_prof(cd_t *d, uint32_t *out);
/** The creature's box in dots last frame [x0, x1) x [y0, y1). */
void  cd_box(const cd_t *d, int *x0, int *y0, int *x1, int *y1);

#ifdef __cplusplus
}
#endif
