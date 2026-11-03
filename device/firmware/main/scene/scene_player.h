/*
 * SPDX-License-Identifier: MIT
 *
 * scene_player.h: a package's layout on screen (docs/PACKAGES.md sections 1 to 3).
 *
 * Plain C, no FreeRTOS / ESP-IDF: pkg_cmd.c builds scenes on the link task, mole_fluid.c
 * plays them on the fluid task (FD_ST_SCENE, fluid_director.h), test/host/scene_test.c
 * runs the same code.
 *
 *   build     scene_build: a layout plus the agent's params → a flat list of items with
 *             every binding resolved ("$name" from params, "@field" from the element of the
 *             item's "each" list, "polar" into x / y). Every param is type-checked against
 *             the layout's declaration: an unknown or missing param, a wrong type, a value
 *             out of range, an asset that does not exist or too many items / letters is
 *             "bad_params". The scene holds a reference to its package (its assets).
 *   player    one scene on screen and a queue behind it. play replaces both; queue appends
 *             (or plays at once when nothing is on). A scene ends at its layout's dur (if
 *             > 0) or its ttl_s, whichever comes first; then the next one in the queue, else
 *             nothing: the director goes back to the creature.
 *   fill      each frame, the scene's visible items (on ≤ t < off) rasterised into the
 *             sim's target (T + tcol); the strongest item wins a cell. Text items also
 *             hand their letters to the renderer's crisp overlay (at most SCENE_LETTERS).
 * Geometry is in glass units: the round window has radius 1, x right, y up, angles in
 * degrees, 0 = right, counter-clockwise.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "pkg_parse.h"
#include "fluid_sim.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCENE_ITEMS_MAX   128           /* resolved items (each expands)                   */
#define SCENE_LIST_MAX    32            /* elements of a list param                        */
#define SCENE_PTS_MAX     512           /* resolved line / curve points, all items         */
#define SCENE_LETTERS     FLUID_MAX_LETTERS
#define SCENE_QUEUE_MAX   8
#define SCENE_TTL_DEFAULT 60.f
#define SCENE_TTL_MAX     600.f
#define SCENE_INK         0.85f         /* a solid item's ink                              */

typedef struct {
    uint8_t k;                          /* PK_*                                            */
    int16_t asset;                      /* index into the package's assets, -1 = none      */
    uint16_t pts, npts;                 /* line / curve: its points in scene_t.pts         */
    uint16_t str, nstr;                 /* text: its letters in scene_t.chars              */
    int n;                              /* ticks                                           */
    uint32_t rgb;
    float on, off;                      /* off 0 = until the end                           */
    float x, y, r, w, a0, a1, len, size, lit, side, spin, rot;
} scene_item_t;

typedef struct scene {
    pkg_t *pkg;                         /* one reference, dropped by scene_free            */
    char pkg_id[PKG_ID_MAX + 1], layout[PKG_KEY_MAX + 1];
    float dur, ttl;
    int nitems, npts, nchars;
    scene_item_t items[SCENE_ITEMS_MAX];
    float pts[SCENE_PTS_MAX][2];
    char chars[SCENE_LETTERS];
} scene_t;

/** Build a scene from layout `layout` of p with `params` (an object, or NULL for none) and
 *  ttl_s (≤ 0 = SCENE_TTL_DEFAULT). NULL with *err = "no_layout" | "bad_params" | "oom". */
scene_t *scene_build(pkg_t *p, const char *layout, const cJSON *params, float ttl_s, const char **err);
void scene_free(scene_t *sc);
/** When the scene ends, s from its start: min(dur if > 0, ttl). */
float scene_end(const scene_t *sc);

typedef struct {
    scene_t *cur;
    scene_t *q[SCENE_QUEUE_MAX];
    int nq;
    float t;                            /* s since cur started                             */
    bool started;                       /* a scene started since the last scene_take_start  */
} scene_player_t;

/** Replace what plays (and the queue) with sc. Takes ownership. */
void scene_play(scene_player_t *p, scene_t *sc);
/** Play sc after the queue (at once when nothing plays). Takes ownership; false when the
 *  queue is full (sc is not taken). */
bool scene_queue(scene_player_t *p, scene_t *sc);
/** Drop everything. */
void scene_clear(scene_player_t *p);
/** Advance by dt: the current scene ends at scene_end, the next one starts. */
void scene_tick(scene_player_t *p, float dt);
/** A new scene started since the last call (the fluid melts into it). */
bool scene_take_start(scene_player_t *p);
static inline bool scene_playing(const scene_player_t *p) { return p->cur != NULL; }

/** Rasterise sc at t s into its run: T + tcol of every cell (zero outside the items),
 *  letters[0..*nletters) the crisp overlay of its text items (stage px). */
void scene_fill(fluid_sim_t *s, const scene_t *sc, float t, fluid_letter_t *letters, int *nletters);

#ifdef __cplusplus
}
#endif
