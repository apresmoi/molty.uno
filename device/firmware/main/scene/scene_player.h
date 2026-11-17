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
 *             sim's target (T + tcol) in painter's order: a later item covers an earlier one
 *             (a black disc erases, a ball's dark side hides what is behind). Text items also
 *             hand their letters to the renderer's crisp overlay (at most SCENE_LETTERS).
 *   spots     a spot draws nothing: it is a disc a tap can land on (scene_spot_at), with the
 *             words Molty shows for it (its reading, a subtitle) and an id the tap event names.
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
#define SCENE_FIELDS      12            /* fields of a list element                        */
#define SCENE_GROUPS      32            /* highlight groups a scene names                  */
#define SCENE_LIGHTS      4             /* the groups one spot lights                      */
#define SCENE_PTS_MAX     512           /* resolved line / curve points, all items         */
#define SCENE_LETTERS     40            /* a scene's own text, letters (the overlay holds FLUID_MAX_LETTERS) */
#define SCENE_SAY_CHARS   4096          /* its spots' readings and ids, characters          */
#define SCENE_QUEUE_MAX   8
#define SCENE_TTL_DEFAULT 60.f
#define SCENE_TTL_MAX     600.f
#define SCENE_INK         0.85f         /* a solid item's ink                              */
#define SCENE_CRISP_MAX   0.25f         /* an upright mask glyph this small (glass units) is drawn
                                         * crisp on the panel, like a letter, not in dots  */

typedef struct {
    uint8_t k;                          /* PK_*                                            */
    int16_t asset;                      /* index into the package's assets, -1 = none      */
    uint16_t pts, npts;                 /* line / curve: its points in scene_t.pts         */
    uint16_t str, nstr;                 /* text: its letters in scene_t.chars; spot: its reading in scene_t.say */
    uint16_t sid, nsid;                 /* spot: its id in scene_t.say (nsid 0: none)       */
    uint8_t grp;                        /* its highlight group, 1.. (scene_t.groups), 0 none */
    bool when_lit;                      /* shows only while its group is lit               */
    uint8_t lights[SCENE_LIGHTS];       /* spot: the groups a tap lights (0: unused)        */
    int n;                              /* ticks                                           */
    uint32_t rgb;
    float on, off;                      /* off 0 = until the end                           */
    float x, y, r, w, a0, a1, len, size, lit, side, spin, rot;
} scene_item_t;

typedef struct scene {
    pkg_t *pkg;                         /* one reference, dropped by scene_free            */
    char pkg_id[PKG_ID_MAX + 1], layout[PKG_KEY_MAX + 1];
    float dur, ttl;
    int nitems, npts, nchars, nsay;
    unsigned serial;                    /* which build it is (scene_fill keeps its drawing)   */
    int focus;                          /* a tapped spot (item) whose drawing is grown, -1 none */
    float grow, glow;                   /* its scale (1 = as built) and how lit, 0..1 (scene_grow) */
    int ngroups;
    char groups[SCENE_GROUPS][PKG_KEY_MAX + 1];
    scene_item_t items[SCENE_ITEMS_MAX];
    float pts[SCENE_PTS_MAX][2];
    char chars[SCENE_LETTERS];
    char say[SCENE_SAY_CHARS];
} scene_t;

/** Build a scene from layout `layout` of p with `params` (an object, or NULL for none) and
 *  ttl_s (≤ 0 = SCENE_TTL_DEFAULT). NULL with *err = "no_layout" | "bad_params" | "oom". */
scene_t *scene_build(pkg_t *p, const char *layout, const cJSON *params, float ttl_s, const char **err);
void scene_free(scene_t *sc);
/** When the scene ends, s from its start: min(dur if > 0, ttl). */
float scene_end(const scene_t *sc);
/** The spot a tap at (x, y) (glass units, y up) lands on at t s: of the visible spots that
 *  hold it (within r of the spot's centre, or of its segment pts), the smallest, then the
 *  nearest. Its item index, -1 for none. */
int scene_spot_at(const scene_t *sc, float t, float x, float y);

typedef struct {
    scene_t *cur;
    scene_t *q[SCENE_QUEUE_MAX];
    int nq;
    float t;                            /* s since cur started                             */
    bool started;                       /* a scene started since the last scene_take_start  */
    unsigned gen;                       /* counts the scenes started: which one cur is      */
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

/* A scene in high definition (fluid_draw_opts_t.scene): alone and crisp on black, it forms
 * from dust, and when it ends or the next one comes it breaks into dust first (out: the
 * caller holds what it showed for the drawer meanwhile), then the next forms. */
#define SCENE_DUST_S 0.45f              /* each way: dust to the scene, the scene to dust  */
typedef struct {
    unsigned gen;                       /* the scene shown (scene_player_t.gen), 0 = none   */
    bool out;                           /* it ended: what it showed goes to dust            */
    float dust;                         /* 0 = whole and crisp .. 1 = all dust, gone        */
} scene_hd_t;
/** Advance by dt: gen the scene on screen now (0: none); plain: the fluid's own look instead
 *  (nothing in high definition; the scene forms from dust again after it). */
void scene_hd_tick(scene_hd_t *h, unsigned gen, bool plain, float dt);
/** The dust for the drawer (fluid_draw_opts_t.dust): the scene's while one forms, shows or breaks up, else 0.
 *  At rest (no scene) h->dust stays 1 (all dust, ready to form), which would thin any other crisp dots (a rigged
 *  creature's) away to nothing. */
static inline float scene_hd_dust(const scene_hd_t *h) { return h->gen || h->out ? h->dust : 0.f; }

/* A tapped spot's drawing grows with the ripple the tap starts, holds while its reading plays,
 * then eases back to exactly as built. Its drawing: the items in the groups it lights (each
 * about its own centre, and brighter; those `when: "lit"` fade in), else the items inside its
 * disc (about the spot's centre). */
#define SCENE_GROW      1.25f           /* its scale, grown                                 */
#define SCENE_GROW_IN_S 0.5f            /* the growth, easing out as the ring spreads       */
#define SCENE_GROW_OUT_S 0.4f           /* back, once its reading is over                   */
#define SCENE_LIT_WHITE 0.5f            /* a lit item's colour this far towards white        */
typedef struct {
    int spot;                           /* the tapped spot (item), -1 none                  */
    float t, out;                       /* s since the tap; 1 held .. 0 back                */
} scene_grow_t;
static inline void scene_grow_tap(scene_grow_t *g, int spot) { *g = (scene_grow_t){ spot, 0.f, 1.f }; }
/** Advance by dt: reading, its reading (or a say over the scene) still plays. */
void scene_grow_tick(scene_grow_t *g, bool reading, float dt);
/** sc's focus and grow from g (none: as built). */
void scene_grow_apply(const scene_grow_t *g, scene_t *sc);

/* A subtitle over a scene: what Molty says while a drawing is up, as the reader shows it
 * (one word at a time, its ORP letter (orp; -1: none, the word centred) in the accent and on
 * the vertical centre line) but small, at the bottom, shrunk until it fits inside the glass */
#define SCENE_CAPTION_Y     (-0.56f)    /* glass units, y up                               */
#define SCENE_CAPTION_SIZE  0.16f       /* type size at most, glass units                  */
#define SCENE_CAPTION_RGB   0x9aa0b8u   /* the ink under it                                */
/* each new word starts a tiny ripple under each of its letters: it reaches a dot or so past the
 * word and fades fast, a wobble that never breaks the drawing into dust (fluid_sim_impulse) */
#define SCENE_CAPTION_RIPPLE   0.1f     /* its strength                                    */
#define SCENE_CAPTION_RIPPLE_R 0.035f   /* its radius, glass units                         */
typedef struct { const char *str; int n, orp; } scene_caption_t;

/* The scene in high definition (fluid_draw_opts_t.shapes, .kdots): its items as shapes at the
 * panel's pixels (rings, strokes, discs, its symbols and letters, the subtitle's too; a ball
 * leaves a black disc, it hides what is behind) and the dots no shape draws (a ball, an image). */
typedef struct {
    fluid_shape_t *shapes;              /* FLUID_MAX_SHAPES                                 */
    int n;                              /* how many; -1 too many: draw the scene on the dots */
    uint8_t *keep;                      /* W x H, may be NULL                               */
} scene_shapes_t;

/** Rasterise sc at t s (and cap, if any, over it) on the braille dots (the items' dots only when
 *  what is visible changes: the subtitle is laid over them, a grown item's dots follow it at
 *  half and full growth; its shapes each frame): T + tcol of every
 *  cell (zero outside the items; a cell's target is its densest dot), dots (W x H, may be
 *  NULL) the dots each cell shows (under a crisp letter or symbol: its shape, which shows when
 *  it breaks into dust), letters[0..*nletters) the crisp overlay of its text
 *  (stage px), and hd (may be NULL) the same in high definition. */
void scene_fill(fluid_sim_t *s, const scene_t *sc, float t, const scene_caption_t *cap,
                fluid_letter_t *letters, int *nletters, uint8_t *dots, scene_shapes_t *hd);

#ifdef __cplusplus
}
#endif
