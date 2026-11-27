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

#include <stdbool.h>
#include <stdint.h>
#include "creature_clip.h"
#include "sky.h"
#include "weather.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CD_VIEW_SIZE 1.75f       /* world units across the panel                                      */
#define CD_FLOOR_AT  0.72f       /* where the creature stands: the near floor, below the middle       */
#define CD_HORIZON_AT 0.495f     /* the floor's horizon, as a fraction of the panel's height          */

typedef struct cd cd_t;

/** For W x H cells (2W x 4H dots). NULL on OOM. */
cd_t *cd_create(const cc_t *c, int W, int H);
void  cd_destroy(cd_t *d);
/** Draw the frame: c's world matrices from its camera; frame counts frames (the dust's shimmer). */
void  cd_draw(cd_t *d, const cc_t *c, uint32_t frame);
/** The cells of the last draw: a braille byte (fluid_braille_bit order) and a colour 0xRRGGBB a cell. */
void  cd_cells(const cd_t *d, const uint8_t **dots, const uint32_t **rgb);
/** Per cell of the last draw: 1 where the creature's own dots are (not the world's). */
const uint8_t *cd_creature(const cd_t *d);
/** The sky behind the creature (sky.h) for the next draws: the moon's cells, then a falling star's (n 0: none). The
 *  lists stay the caller's and must live until the next call. A cell with the creature's dots keeps only them and the
 *  halo keeps the sky's dots off it, as the floor's; a moon cell takes the place of the dust's motes there. */
void  cd_sky(cd_t *d, const sky_cell_t *moon, int nmoon, const sky_cell_t *star, int nstar);
/** The weather (weather.h, as its last weather_frame made it) for the next draws, and the water's height on the same
 *  grid (fluid_sim_t.h: it pushes and breaks the wet floor's mirror); w NULL or with nothing drawn: none. Both stay the
 *  caller's. Behind: as the sky. In front and on the glass: never on the creature's own cells nor near its eyes. */
void  cd_weather(cd_t *d, const weather_t *w, const int16_t *h);
/** The weather's dots merged in the last draw: mirrored in the wet floor, behind, in front, on the glass. */
void  cd_weather_dots(const cd_t *d, int out[4]);
/** The first band's share of the rows' work, percent (20..90; CD_SHARE at first): the caller balances the two cores. */
void  cd_set_share(cd_t *d, int pct);
#define CD_STAGES 8
/** Cycles spent per stage since the last call, added into out[CD_STAGES]: projection, the body and the solid parts, lines,
 *  features, the halo's rows, the halo's columns with the world and the cells; then, within those, the waits for the
 *  second band; and the second band's own time on the other core (ESP). */
void  cd_prof(cd_t *d, uint32_t *out);

/* The frame rate lab (diag/fps_lab.h, FPS-LAB.md): the creature's own cells (its dots, its halo's, its colour per cell,
 * before the world goes round them) kept for a pose seen before, so the raster is skipped; the world (floor, dust, sky,
 * weather) is made over them every frame as ever. A pose is its key: every input of the raster (each node's world
 * matrix and the camera; the shapes and the palette are the clip's own, fixed). */
enum { CD_RASTER, CD_SAME, CD_CACHED };
#define CD_REUSE_MAX 128
typedef struct {
    uint64_t key;                 /* the last frame's pose key (0: none made)                                          */
    bool gesture;                 /* ... it played more than its mood then (cc_free false)                              */
    uint8_t last;                 /* how the last frame's creature was made: CD_RASTER, CD_SAME (the frame before's kept),
                                   * CD_CACHED (a pose kept from before)                                               */
    uint32_t frames[3], gframes[3];   /* frames made each way since the settings last changed; ... in a gesture       */
    uint32_t bytes;               /* the cache's memory (PSRAM)                                                         */
    uint64_t cyc[3], band[3];     /* cycles in cd_draw on its own core, and in the second band, each way, summed (ESP)  */
    uint64_t upkeep;              /* ... of the key and the cache's own work (looking up, keeping a raster's cells)      */
} cd_reuse_t;
/** The pose key of c's frame now (never 0). */
uint64_t cd_pose_key(const cc_t *c);
/** From the next draw: make the pose key (key), keep the frame before's cells for the same pose (skip), keep the cells
 *  of the last n distinct poses (n 0: none; at most CD_REUSE_MAX, PSRAM). All off: the raster every frame, as ever. A
 *  change resets the counters and makes or frees the cache (on the drawing task). */
void  cd_reuse(cd_t *d, bool key, bool skip, int n);
const cd_reuse_t *cd_reuse_stats(const cd_t *d);

#ifdef __cplusplus
}
#endif
