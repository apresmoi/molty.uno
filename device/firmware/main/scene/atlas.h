/*
 * SPDX-License-Identifier: MIT
 *
 * atlas.h: the sky atlas (docs/AGENT-LINK.md "Atlas", the molty-atlas skill): the night sky's best known objects as
 * cards, browsed on the device with no round trip. The agent works out the sky (its catalogue, the maths, the place it
 * keeps) and sends each kind's objects once (atlas.load): names, two lines of numbers, a drawing, tonight's path in the
 * sky. The finger does the rest here: the kinds on the rim, a kind's objects, a card's drawing and where it is now.
 *
 * Plain C, no FreeRTOS / ESP-IDF: pkg_cmd's link task loads it, mole_fluid.c turns and draws it on the fluid task,
 * sense.c's accept button presses its centre; one spin lock between them. web/web_molty.c runs the same code and
 * test/host/atlas_test.c checks it.
 *
 *   store     ATLAS_KINDS kinds, each up to ATLAS_OBJECTS objects, in PSRAM on the device (atlas_load); freed when the
 *             screen that showed them has been gone ATLAS_ORPHAN_S (atlas_frame).
 *   levels    the kinds (a bead each, 30 degrees, the middle opens one), a kind's objects (a bead each, 15 degrees, five
 *             a click turned fast, the kind's name above goes back up), each object's card: its drawing, and with a
 *             place, where it is now (the middle turns the card). Without a place the where face is hidden.
 *   drawing   stars as discs and their figure's lines (a constellation, a cluster's real stars, a double), or a field of
 *             dots on the braille lattice (a picture's stipple, sent; a model of a galaxy, a globular, a nebula, made
 *             here from its catalogue numbers, the same maths as the prototype's), or a lit globe (the Moon, a planet),
 *             or a meteor shower's radiant in its constellation. All of it still once formed: a new card is drawn again
 *             in place (scene_t.serial), and only once the glass has caught up with the last one.
 *   where     the horizon as a circle seen from above, north up and east right, the object's path for the next hours
 *             (ATLAS_PATH points the agent sent), where it is now (its path at the time since the load), when it rises,
 *             is highest or sets (minutes after the load, read on the clock the agent sent).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "scene_player.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ATLAS_KINDS    10
#define ATLAS_OBJECTS  96               /* a kind's objects at most (88 constellations)                       */
#define ATLAS_ID       24               /* an id (the ring event carries 24)                                    */
#define ATLAS_NAME     24               /* a name's letters                                                    */
#define ATLAS_FACTS    22               /* a card's numbers' letters                                           */
#define ATLAS_SHORT    15               /* a kind's name on the glass                                          */
#define ATLAS_STARS    64               /* a star drawing's stars at most                                      */
#define ATLAS_LINES    64               /* ... and its lines                                                   */
#define ATLAS_DOTS     700              /* a dot drawing's dots at most (a picture's stipple, a model)         */
#define ATLAS_PATH     16               /* tonight's path: its points                                          */
#define ATLAS_ORPHAN_S 2.f              /* the screen gone this long: the store is freed                      */
#define ATLAS_KEEP_S   60.f             /* ... a load no screen has shown yet waits this long                  */
#define ATLAS_FIT      0.56f            /* a drawing's reach from its centre, in its own units                 */
#define ATLAS_TOP_Y    0.75f            /* the kind's name above a card: a tap there goes up                   */

enum { ATLAS_EYE, ATLAS_BINOCULARS, ATLAS_TELESCOPE };
enum { AD_NONE, AD_STARS, AD_DOTS, AD_MODEL, AD_GLOBE, AD_RADIANT };
enum { AM_GALAXY = 1, AM_GLOBULAR, AM_NEBULA, AM_PLANETARY, AM_OPEN };
enum { ATLAS_AT_KINDS, ATLAS_AT_OBJECTS };
enum { ATLAS_DRAWING, ATLAS_WHERE };

/* A drawing's point: x, y in the drawing's units (ATLAS_FIT its reach, north up, east left), a level 0..15 (its size or
 * brightness), a colour 0..7 (atlas_star_rgb: blue-white to orange, a nebula's oxygen teal and hydrogen red). */
typedef struct { float x, y; uint8_t level, colour; } atlas_pt_t;

/** The colour of a point's colour index. */
uint32_t atlas_star_rgb(int colour);
/**
 * A model drawing from catalogue numbers, the prototype's: a galaxy from its Hubble type (hubble: 0 spiral, 1
 * elliptical, 2 lenticular, 3 irregular, 4 an irregular with arms), its tilt (ratio: minor over major axis) and its angle on the sky (pa, degrees
 * from north through east); a globular as a Plummer sphere; a nebula as soft lobes (planetary: a shell); an open cluster
 * of its size (arcmin) and richness. seed: the object's. Into out (at most max), how many.
 */
int atlas_model(int type, int hubble, float ratio, float pa, float size, uint32_t seed, atlas_pt_t *out, int max);
/** The prototype's seed of an object id (make.mjs: each letter, times 31 plus the next, from 7, to 16 bits). */
uint32_t atlas_seed(const char *id);

/* ── the store: the link task loads it ─────────────────────────────────────────────────────────────────────────── */
/**
 * atlas.load: {kinds: [{id, name, tint, count, up}] (resets the store), clock (minutes of the place's day now, -1: no
 * place), kind, objects: [{id, name, facts, see, d, w}]} (one kind's objects, in the wheel's order). NULL, or an error
 * code ("invalid_args", "oom"). docs/AGENT-LINK.md has every field.
 */
const char *atlas_load(const cJSON *args);
/** atlas.show: {kind, index, face ("drawing" | "where"), level ("kinds" | "objects")}: where the screen goes now. */
const char *atlas_show(const cJSON *args);
/** Drop it all (the screen closed, a new load). */
void atlas_clear(void);
/** The PSRAM it holds now, bytes (the objects, their drawings, the card's dots). */
size_t atlas_bytes(void);
/** A loaded kind, objects included, by id: its index, -1 none (tests). */
int atlas_kind_index(const char *id);

/* ── the screen: mole_fluid.c on the fluid task (the turning, the drawing), sense.c's accept button ─────────────── */
/** The layout's atlas item, -1 none. */
int atlas_item(const scene_t *sc);
/** An atlas shows with something loaded: the accept button presses its centre. */
bool atlas_active(void);
/** Where it is: level (ATLAS_AT_*), the kind, the object (-1 at the kinds), the face (ATLAS_*). */
void atlas_where(int *level, int *kind, int *index, int *face);
/** The rim turned `clicks` (clockwise positive; fast: five objects a click). 0 nothing moved, 1 moved, 2 it passed the
 *  first object (a firmer tick). */
int atlas_turn(int clicks, bool fast);
/** The middle: opens the kind; on a card, turns it (with a place). */
void atlas_local_centre(void);
/** The kind's name above a card: back up to the kinds. */
void atlas_local_up(void);
/** The wheel's beads now: at most max labels round the one chosen (its index in them), each its tint (0: grey), the
 *  degrees a click. Labels are the names (the agent hears them in a `ring` event). */
int atlas_beads(char (*labels)[PKG_KEY_MAX + 1], uint32_t *tints, int max, int *index, float *step);
/** The wheel's face for where it is (mole_fluid.c's): the beads, no words in the middle (the atlas draws its own), the
 *  dial's step the level's (30 degrees a kind, 15 an object). */
void atlas_wheel(scene_t *sc, scene_wheel_t *w, char (*labels)[PKG_KEY_MAX + 1], uint32_t *tints, int max);
/** Where it is as one number: a turn that changed it ends in a `ring` event. */
float atlas_key(void);
/** Where the wheel settled, for the `ring` event: spot "kind" or "object", the object's or the kind's id and name. */
void atlas_settled(char *spot, int nspot, char *id, int nid, char *label, int nlabel, int *index);
/** Whether the kind's name above shows (a tap there goes up), and where. */
bool atlas_label(float *y);
/** Each frame, after scene_beat: the screen's items for where it is. behind: the glass has not caught up with the last
 *  picture (the drawer owed cells): it stays as it is. sc NULL: no atlas on screen (its store is freed after a while). */
void atlas_frame(scene_t *sc, float dt, bool behind);

#ifdef __cplusplus
}
#endif
