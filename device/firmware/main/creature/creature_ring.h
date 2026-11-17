/*
 * SPDX-License-Identifier: MIT
 *
 * creature_ring.h: the rings (a listen's, a question's arcs, the agent's progress) over a rigged creature's crisp cells,
 * as dust: a thin bright core at the rim dissolving inward into sparse red dust, feathered on both sides, twinkling slowly
 * a few rows a frame like the ambient dust. The rings themselves are the fluid's (fluid_target_ring, _arcs, _progress
 * write their ink into the sim's target); this reads that ink at each ring's line and draws it. A ring forms from the
 * water and dissolves back into it (dust drifting in, a ripple running round, eased over CR_EASE_S), never popping.
 *
 * Only the rim's cells change, and only while a ring forms, twinkles or dissolves.
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
/** This frame (dt s, frame its count): from the target's ink at the ring lines, the rings' dots and colours over the
 *  given cells: dots (in: the creature's, out: with the rings) and col (the sim's carried colour, set where a ring
 *  shows). True when any ring shows. */
bool  cr_paint(cr_t *r, const fluid_sim_t *s, float dt, uint32_t frame, uint8_t *dots, fluid_rgba16_t *col);

#ifdef __cplusplus
}
#endif
