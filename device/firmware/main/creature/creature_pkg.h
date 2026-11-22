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
/** What a rigged creature's first frame would make (its dots and its rings: tables that take a few hundred ms on the
 *  device), made ahead, on whichever task made it. grid: W, H, xs, ys and mask as the sim's it will play on (only those
 *  are read: a copy will do). */
void   cpk_prepare(cpk_t *k, const fluid_sim_t *grid);
void   cpk_destroy(cpk_t *k);
/** Moods and actions go to its engine (me_set_mood, me_act, ...; its own gestures are already given to it). */
mood_engine_t *cpk_engine(cpk_t *k);
const cp_t    *cpk_player(const cpk_t *k);
/** The agent is working (mood `working`): the package's `work` input, for its own life while it waits. */
void   cpk_set_work(cpk_t *k, bool on);
/** What its life (cp_life) reads of the world: seconds since anyone did anything, and the owner's idle-life setting
 *  (1 lively, 0.5 calm, 0 off). */
void   cpk_set_idle(cpk_t *k, float idle_s, float life);
const cb_t    *cpk_body(const cpk_t *k);
/** One frame: the engine, the player, the body into the cells, the ambient over them (its
 *  splashes and currents into s), then s's target. */
void   cpk_frame(cpk_t *k, fluid_sim_t *s, float dt, float swell);
int    cpk_tris(const cpk_t *k);
/** A rigged creature (docs/CREATURES.md section 12): drawn straight on the braille dots, not through the fluid's ink. */
bool   cpk_crisp(const cpk_t *k);
/** A tap or a shake: a rigged creature plays first (a shake: "shaken"), then comes to the front of the frame facing the
 *  viewer, attending (cc_come). */
void   cpk_come(cpk_t *k, const char *first);
/** The sky behind a rigged creature (sky.h: s->moon_c, s->star_c as its last sky_frame made them), drawn into its world
 *  from its next frame; NULL: none. s must live as long as k uses it. */
struct sky;
void   cpk_sky(cpk_t *k, const struct sky *s);
/** A rigged creature's dots: the share of their rows' work on the calling core (cd_set_share), the rest on the other. */
void   cpk_set_share(cpk_t *k, int pct);
/** The listen ring over a rigged creature, given directly (creature_ring.h cr_listen): its colour (0xRRGGBB, -1: the
 *  fluid ring's own) and its ink (fluid_ring_t.ink). */
void   cpk_listen(cpk_t *k, int32_t rgb, float ink);
/** Per cell of the last frame: 1 where the creature's own dots are (cd_creature). */
const uint8_t *cpk_cells_creature(const cpk_t *k);
/** The drawer's fluid_draw_opts_t.frame for its cells (cpk_paint's frame and the rings' interlace: FLUID_FRAME). */
uint32_t cpk_draw_frame(const cpk_t *k);
/** A listen's voice, 0..1 (the mic's level): ripples from the listen ring's rim (cr_voice). */
void   cpk_voice(cpk_t *k, float level);
const struct cc *cpk_clip(const cpk_t *k);
/** Its last frame's cells: a braille byte (the rings over them, after cpk_paint) and a colour (0xRRGGBB) each; NULL before
 *  the first frame or for a channel creature. */
void   cpk_cells(const cpk_t *k, const uint8_t **dots, const uint32_t **rgb);
/** Write those colours into the sim's carried colour (after its colour step), so each cell shows its own at once; and the
 *  rings the target holds this frame (listen, question, progress: fluid_target_ring and the rest) over them. */
void   cpk_paint(cpk_t *k, fluid_sim_t *s);

#ifdef __cplusplus
}
#endif
