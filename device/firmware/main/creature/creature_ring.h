/*
 * SPDX-License-Identifier: MIT
 *
 * creature_ring.h: the rings (a listen's, a question's arcs, the agent's progress) over a rigged creature's crisp cells,
 * as ring-options.html's chosen one: a wave runs out from the centre through the real water (the creature wobbles as it
 * passes), its crest turns the ring's colour near the rim and lands there as a bright line with a soft band of dust
 * feathering inward behind it; letting go, a wave runs in from the rim carrying the colour inward, fading. About 1 s
 * each way (the ring's own wave is driven faster than a touch's ripple). The rings themselves are the fluid's
 * (fluid_target_ring, _arcs, _progress write their ink into the sim's target): this reads that ink at each line.
 *
 * Plain C, no ESP-IDF (host-testable).
 */
#pragma once

#include <stdint.h>
#include "fluid_sim.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cr cr_t;

/** For the sim's grid (its cell centres and mask). NULL on OOM. */
cr_t *cr_create(const fluid_sim_t *s);
void  cr_destroy(cr_t *r);
/** The listen ring's colour (0xRRGGBB) for its line instead of the ink's, or -1: the ink's (the fluid's ring colour). */
void  cr_listen_rgb(cr_t *r, int32_t rgb);
/** This frame (dt s, frame its count): from the target's ink at the ring lines, the ring's wave into the sim's water
 *  (s->h) and its dots and colours over the given cells: dots (in: the creature's, out: with the ring) and col (the sim's
 *  carried colour, set where the ring shows); creature: per cell, the creature is there (its colour never shows on it).
 *  True while a ring shows, forms or lets go. */
bool  cr_paint(cr_t *r, fluid_sim_t *s, float dt, uint32_t frame, const uint8_t *creature, uint8_t *dots, fluid_rgba16_t *col);

#ifdef __cplusplus
}
#endif
