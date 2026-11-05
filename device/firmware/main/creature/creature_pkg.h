/*
 * SPDX-License-Identifier: MIT
 *
 * creature_pkg.h: a creature package alive in the fluid: its own mood engine, the player
 * (creature_player.h) and the body (creature_body.h), one frame at a time. What the fluid
 * (mole_fluid.c) and the host test drive. Plain C, host-testable.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "creature_body.h"
#include "creature_player.h"
#include "mood_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cpk cpk_t;

/** Parse the package and build its body. NULL with *err (static). */
cpk_t *cpk_create(const char *json, size_t len, uint32_t seed, const char **err);
void   cpk_destroy(cpk_t *k);
/** Moods and actions go to its engine (me_set_mood, me_act, ...; its own gestures are already given to it). */
mood_engine_t *cpk_engine(cpk_t *k);
const cp_t    *cpk_player(const cpk_t *k);
/** The agent is working (mood `working`): the package's `work` input, for its own life while it waits. */
void   cpk_set_work(cpk_t *k, bool on);
const cb_t    *cpk_body(const cpk_t *k);
/** One frame: the engine, the player, the body into the cells, the ambient over them (its
 *  splashes and currents into s), then s's target. */
void   cpk_frame(cpk_t *k, fluid_sim_t *s, float dt, float swell);
int    cpk_tris(const cpk_t *k);

#ifdef __cplusplus
}
#endif
