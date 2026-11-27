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
#define SCENE_LIVE_MAX    160           /* items a beat (scene_beat) draws after them      */
#define SCENE_LIST_MAX    32            /* elements of a list param                        */
#define SCENE_FIELDS      12            /* fields of a list element                        */
#define SCENE_GROUPS      32            /* highlight groups a scene names                  */
#define SCENE_LIGHTS      4             /* the groups one spot lights                      */
#define SCENE_PTS_MAX     512           /* resolved line / curve points, all items         */
#define SCENE_LIVE_PTS    384           /* points a wave (scene_wave) draws after them     */
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
    float grow;                         /* spot: how much what a tap on it lights grows (PKG_GROW) */
    float arc, r0, r1;                  /* spot: also tappable in this ring sector about the glass centre: arc degrees
                                         * wide (0: none) round the spot's own direction, radii r0..r1 */
    uint16_t open, nopen;               /* spot: the layout a tap on it opens, in scene_t.say (nopen 0: none) */
    float ttl;                          /* ... for this long when that layout is a held one (0: until a tap beside it) */
    int8_t lst; uint8_t eln;            /* the list param it was repeated over (-1 none) and which element */
    bool live;                          /* drawn anew each frame (a beat's hand, its swelling hits): a shape only,
                                         * never in the kept dots, the fine dust or a key                         */
    int8_t layer;                       /* a beat's hit: its layer (a tap mutes it), else -1                      */
    /* a dial: a click every step degrees adds by (fast turning fast) to val, vmin..vmax; or steps through its labels
     * (nlab of them, in scene_t.say from lab, '\n' between); sets the beat player's (1 tempo, 2 pick); on face 0 or 1;
     * each click a tick of the motor, tick_ms at tick percent (0: haptic_plan.h HAPTIC_WHEEL_*); 3 a wave's pitch */
    float step, by, fast, vmin, vmax, val;
    uint8_t face, sets, nlab, tick_ms, tick;
    uint16_t lab;
} scene_item_t;

typedef struct scene {
    pkg_t *pkg;                         /* one reference, dropped by scene_free            */
    char pkg_id[PKG_ID_MAX + 1], layout[PKG_KEY_MAX + 1];
    float dur, ttl;
    float caption_y;                    /* where the reading's words go (the layout's caption_y) */
    int nitems, npts, nchars, nsay;
    int nfixed, nchars_fixed;           /* what the layout built; a beat's items follow (scene_beat)          */
    int beat;                           /* the layout's beat item, -1 none                                      */
    int wave;                           /* the layout's wave item, -1 none                                      */
    int npts_fixed;                     /* the layout's points; a wave's follow (scene_wave)                    */
    uint32_t beat_key;                  /* what its kept part was drawn from                                    */
    unsigned serial;                    /* which drawing it is (scene_fill keeps it): a build, or a beat's kept part
                                         * drawn again in place                                                 */
    unsigned look;                      /* which picture it is: its build (a new one is a change of picture, the
                                         * dissolve; a beat's kept part drawn again in place is not)            */
    int focus;                          /* a tapped spot (item) whose drawing is grown, -1 none */
    float grow, glow;                   /* its scale (1 = as built) and how lit, 0..1 (scene_grow) */
    int ngroups;
    char groups[SCENE_GROUPS][PKG_KEY_MAX + 1];
    scene_item_t items[SCENE_ITEMS_MAX + SCENE_LIVE_MAX];
    float pts[SCENE_PTS_MAX + SCENE_LIVE_PTS][2];
    cJSON *params;                      /* its params, kept when a spot of it opens a layout (they feed that one) */
    char chars[SCENE_LETTERS];
    char say[SCENE_SAY_CHARS];
} scene_t;

/* A beat (pkg item `beat`, docs/PACKAGES.md): the beat player's pattern as a clock, from the player's own clock
 * (rhythm.h): one ring a layer (r, then 0.75, 0.525 and 0.3375 of it), a mark a step outside them (longer on a main
 * beat), a dot a hit of this bar in its layer's colour (a muted layer's at SCENE_BEAT_MUTED), the hand on the step
 * sounding, each hit swelling as it sounds, the count-in's clicks on their marks, the phrase as a row of pips, the
 * rhythm's name; at rest (stopped, choosing: rest) the hand at 12 o'clock and a play mark in the middle, while it
 * plays a small stop mark there. Only the rings and the marks are kept (the dots, the fine dust): they change with the
 * pattern's shape alone, a new rhythm, and then are drawn again in place (scene_t.serial, never scene_t.look). All the
 * rest is live items (scene_item_t.live): shapes each frame, so a bar of a phrase, a mute, a tempo, the name or the
 * wheel's words change nothing kept and never the picture whole. */
#define SCENE_BEAT_STEPS  32
#define SCENE_BEAT_LAYERS 4
#define SCENE_BEAT_MUTED  0.3f          /* a muted layer's dots: this much of its colour  */
#define SCENE_BEAT_SWELL_S 0.25f        /* a hit swells and settles back in this long      */
typedef struct {
    bool on;                            /* a pattern is loaded                              */
    int steps;
    float angle[SCENE_BEAT_STEPS];      /* each step's angle (degrees, 90 = 12 o'clock, clockwise as it goes) */
    uint32_t beats;                     /* the main beats (step bits)                       */
    int nlayers;
    uint32_t rgb[SCENE_BEAT_LAYERS], hits[SCENE_BEAT_LAYERS], muted;   /* this bar's hits a layer */
    int step;                           /* sounding now, -1 none (a count-in)               */
    int cin_step;                       /* the count-in's click (a step), -1 none           */
    float since;                        /* s since that step or click began                  */
    int slot, norder;                   /* where in the phrase                              */
    bool rest;                          /* stopped, choosing: the hand at rest, the play mark */
    bool enter;                         /* ... at the regions: the enter mark instead        */
    bool hide_name;                     /* (a wheel shows its own words in the middle)       */
    char label[13];
} scene_beat_t;
/** The beat's items after the layout's own (a no-op without a beat item). Each frame, before scene_fill. */
void scene_beat(scene_t *sc, const scene_beat_t *b);
/** The layer whose hit dot (x, y) is on (glass units, y up; a finger's reach round it), -1 none. */
int scene_beat_hit(const scene_t *sc, float x, float y);

/* Wheels (pkg items `dial` and `centre`, docs/PACKAGES.md): a ring at the edge the finger turns in clicks
 * (sense/ring_input.h), on the device, with no round trip: a number (a tempo) or a choice among labels (a list, the beat
 * player's library). Two faces, each its own dial: face 0 while the beat plays (or with no beat), face 1 while it is
 * stopped; the centre's tap plays and stops it (mole_fluid.c). The agent hears where a wheel settled (a `ring` event).
 * scene_wheel draws them, live, each frame: the rail, its ticks (a number) or one bead a label (a choice) stepping a
 * click at a time (never sliding: a frame of a turn redraws only what a click changed), the notch at 12 o'clock that
 * flashes on each click, and in the middle the value while it turns (and
 * SCENE_WHEEL_SHOW_S after), or the choice, under the beat's play mark, all the while face 1 is up. */
#define SCENE_WHEEL_SHOW_S 0.9f
#define SCENE_WHEEL_SCALE  75.f        /* a number's ticks: this far each side of the notch */
#define SCENE_WHEEL_LABELS 32
#define SCENE_NOTCH_RGB   0x7be0a0u     /* the notch on face 0 (the tempo): Molty's green, the accent */
#define SCENE_CHOOSE_RGB  0xffd88au     /* ... on face 1 (choosing): warm gold                       */
#define SCENE_WHEEL_ABOVE_Y 0.27f       /* a pick's region label above the name (of the beat's 0.672 r), its zone's half */
#define SCENE_WHEEL_ABOVE_W 0.30f       /* width and half height: a tap there goes up a level             */
#define SCENE_WHEEL_ABOVE_H 0.075f
typedef struct {
    int face;                           /* 0, 1: the dial up                              */
    float value;                        /* its value (a number dial)                      */
    int index;                          /* its label (a choice)                           */
    bool turning;                       /* a finger is on it                              */
    float since;                        /* s since the finger left it                      */
    float flash;                        /* the notch's flash: 1 at a click, down to 0     */
    int nlabels;
    const char (*labels)[PKG_KEY_MAX + 1];
    const uint32_t *tints;              /* each label's tint (a bead), NULL: none            */
    int level;                          /* a pick's: 0 a region's rhythms, 1 the regions     */
    const char *above, *below;          /* small words over the name (the region, a tap goes up) and under it (a
                                         * region's size), NULL: none                         */
    uint32_t above_rgb;
    bool quiet;                         /* no words in the middle: a wave draws its own      */
    float land;                         /* the notch longer on a landing (a wave's octave), 1 down to 0 */
} scene_wheel_t;
/** The dial on face f, the centre: item indices, -1 none. */
int scene_dial(const scene_t *sc, int face);
int scene_centre(const scene_t *sc);
/** Dial item n's own labels into out (at most max), how many. */
int scene_dial_labels(const scene_t *sc, int n, char (*out)[PKG_KEY_MAX + 1], int max);
/** Wheel w's items after the layout's own and the beat's (call scene_beat first). */
void scene_wheel(scene_t *sc, const scene_wheel_t *w);
/** The words a wheel shows in the middle now (the beat's name gives way to them). */
bool scene_wheel_words(const scene_t *sc, const scene_wheel_t *w);

/* A wave (pkg item `wave`, docs/PACKAGES.md): the tone player's chord on one circle (wave.h), drawn from the speaker's
 * own clock like a beat, all of it live: the circle at r, its outline pushed out by the sound the hand drew there (the
 * trace: SCENE_WAVE_PTS + 1 radii on the design's 0.5 circle, wave_player.h wav_trace), each voice's hits a dot on it,
 * joined as its shape (two a line, three a triangle ...), the hand (crisp, then a blur of what it sweeps a frame, gone
 * once the beats are a note), the readouts (a count a second or hertz and the note; the chord's name; the whole
 * numbers up top, a tap on one mutes its voice, as a tap on a voice's dots does while they show). At most `budget`
 * shapes: the outline takes what the rest leaves, simplified to fit (never under SCENE_WAVE_MIN_SEGS strokes), and
 * the dots of the densest voices give way first. Sizes are the design's on its 0.5 circle, times r / 0.5. */
#define SCENE_WAVE_PTS      720
#define SCENE_WAVE_REACH    0.14f       /* the outline's reach out from the circle at a hit, on the 0.5 circle  */
#define SCENE_WAVE_AFTERGLOW 0.12f      /* s: the outline keeps what the hand drew this long, while slow        */
#define SCENE_WAVE_MIN_SEGS 48
#define SCENE_WAVE_TOP_Y    0.73f       /* the note (chords) or the whole numbers (playing) up top              */
enum { SCENE_WAVE_NOTES, SCENE_WAVE_CHORDS, SCENE_WAVE_PLAYING };
typedef struct {
    bool on;
    int level;                          /* SCENE_WAVE_*                                                          */
    int nv;
    uint8_t count[5];                   /* each voice's whole number: its dots a turn                            */
    uint32_t rgb[5], muted;
    float H, fc;                        /* the clock: turns (0..1), turns a second                                */
    float ph[5], f[5], env[5];          /* each voice: its phase from its hit, its rate (Hz), its level           */
    bool pluck[5];                      /* ... ringing as a preview (stopped)                                    */
    const float *trace;                 /* SCENE_WAVE_PTS + 1 radii, NULL: the plain circle                      */
    const char *name, *top, *big, *unit;   /* a lowercase b in them is a flat                                    */
    uint32_t name_rgb, top_rgb, unit_rgb;
    float tone;                         /* 0 a beat .. 1 a note: the note in the unit fades in                    */
    float land;                         /* the number a little bigger on a landing, 1 down to 0                   */
    bool mark;                          /* the centre's mark shows (playing: not while the rim turns)            */
    bool still;                         /* the same as last frame, the outline too: nothing moves                */
    int budget;                         /* shapes it may make                                                    */
} scene_wave_t;
/** The wave's items after the layout's own (a no-op without a wave item). Each frame, after scene_beat. */
void scene_wave(scene_t *sc, const scene_wave_t *w);
/** How far a voice at hz hits a second has gone from beat to note: 0 to 12, 1 from 32 (its dots hand over to the
 *  outline, the afterglow holds whole). */
float scene_wave_fuse(float hz);

/** Build a scene from layout `layout` of p with `params` (an object, or NULL for none) and
 *  ttl_s (≤ 0 = SCENE_TTL_DEFAULT). NULL with *err = "no_layout" | "bad_params" | "oom". */
scene_t *scene_build(pkg_t *p, const char *layout, const cJSON *params, float ttl_s, const char **err);
void scene_free(scene_t *sc);
/** When the scene ends, s from its start: min(dur if > 0, ttl). */
float scene_end(const scene_t *sc);
/** The spot a tap at (x, y) (glass units, y up) lands on at t s: of the visible spots that
 *  hold it (within r of the spot's centre, or of its segment pts), the smallest, then the
 *  nearest; if none holds it so, of those whose sector does (arc, r0, r1), the same. Its item
 *  index, -1 for none. */
int scene_spot_at(const scene_t *sc, float t, float x, float y);

typedef struct {
    scene_t *cur;
    scene_t *q[SCENE_QUEUE_MAX];
    int nq;
    float t;                            /* s since cur started                             */
    scene_t *back;                      /* the scene a tap opened cur over: it comes back when cur ends ... */
    float back_t;                       /* ... its clock, which kept running (its ttl ends both) */
    char say[PKG_SAY_MAX + 1];          /* ... and the tapped spot's reading, to play over cur once it is up (scene_open_say) */
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
/** A tap on spot item n of sc opens a layout (its `open`): that layout built with its params taken by name, each from
 *  the tapped spot's list element if it has such a field, else from sc's own params; a list keeps only the elements
 *  whose `of` (if they have one) is the spot's id. It lasts its dur, else the spot's ttl, else until sc's end. NULL: the
 *  spot opens nothing, or *err ("no_layout" | "bad_params" | "oom"). */
scene_t *scene_open_build(const scene_t *sc, int n, const char **err);
/** Play sc over the current scene, which waits under it (one deep: a scene opened from an opened one replaces it) and
 *  comes back when sc ends or at scene_back; its clock runs on meanwhile. Takes ownership. spot: the current scene's
 *  spot whose tap opened it (scene_open_build's n; -1 none): its reading is kept to play over sc. */
void scene_open(scene_player_t *p, scene_t *sc, int spot);
/** That reading, once sc is up whole (the caller says when: after the change of picture): into out (size bytes); its
 *  length, 0 for none. Taken once; going back, or another scene, drops it. While it plays the caller keeps sc from
 *  ending (scene_room), so a short layout does not end under its own sentence. */
int scene_open_say(scene_player_t *p, char *out, size_t size);
/** Back to the scene under an opened one, now. False: none was open. */
bool scene_back(scene_player_t *p);
/** Advance by dt: the current scene ends at scene_end, the next one starts. */
void scene_tick(scene_player_t *p, float dt);
/** How long until anything ends, s (the scene, or the one under it: its ttl ends both); a large number with no
 *  scene. A caller that must not let it end yet (a reading or a say plays over it, a turn is on) ticks it by no more
 *  than this less a little: its clock runs on (what it lights in turn still does), its end waits. */
float scene_room(const scene_player_t *p);
/** A new scene started since the last call (the fluid melts into it). */
bool scene_take_start(scene_player_t *p);
static inline bool scene_playing(const scene_player_t *p) { return p->cur != NULL; }

/* A scene in high definition on the dots (fluid_draw_opts_t.scene, no fine dust): alone and crisp on black, it forms
 * from dust, and when it ends or the next one comes it breaks into dust first (out: the
 * caller holds what it showed for the drawer meanwhile), then the next forms. (On fine dust a scene comes and goes by
 * the drawer's change of picture instead: fluid_draw_morph_look.) */
#define SCENE_DUST_S 0.6f               /* each way: dust to the scene, the scene to dust */
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
 * disc (about the spot's centre). Its reading's first word (and with it the caption's backdrop)
 * comes once the growth is over (scene_grow_settled), and after its reading the drawing stays
 * grown while the backdrop lifts (SCENE_GROW_HOLD_S): the growth, the backdrop and the tap's
 * wave never all change the same frames. */
#define SCENE_GROW      PKG_GROW        /* its scale, grown, unless its spot says (grow)      */
#define SCENE_GROW_IN_S 0.5f            /* the growth, easing out as the ring spreads       */
#define SCENE_GROW_HOLD_S 0.8f          /* held grown so long after its reading (the backdrop's lift, FINE_END_S, and a frame or two) */
#define SCENE_GROW_OUT_S 0.4f           /* then back                                        */
#define SCENE_LIT_WHITE 0.5f            /* a lit item's colour this far towards white        */
typedef struct {
    int spot;                           /* the tapped spot (item), -1 none                  */
    float t, out, rest;                 /* s since the tap; 1 held .. 0 back; s since its reading ended */
} scene_grow_t;
static inline void scene_grow_tap(scene_grow_t *g, int spot) { *g = (scene_grow_t){ spot, 0.f, 1.f, SCENE_GROW_HOLD_S }; }
/** Advance by dt: reading, its reading (or a say over the scene) still plays. */
void scene_grow_tick(scene_grow_t *g, bool reading, float dt);
/** Its growth is over (or none): a tapped spot's reading shows its first word only then. */
static inline bool scene_grow_settled(const scene_grow_t *g) { return g->spot < 0 || g->t >= SCENE_GROW_IN_S; }
/** sc's focus and grow from g (none: as built). */
void scene_grow_apply(const scene_grow_t *g, scene_t *sc);
/** The scale a tap on spot item n grows its drawing to (its grow; SCENE_GROW for none). */
static inline float scene_spot_grow(const scene_t *sc, int n) { return sc && n >= 0 && n < sc->nitems && sc->items[n].grow >= 1.f ? sc->items[n].grow : SCENE_GROW; }
/** Points just outside the outline of what a tap on spot item n grows, at full growth (glass units, y up), about
 *  SCENE_OUTLINE_STEP apart, SCENE_OUTLINE_GAP out: both sides of a line, outside (and inside, if wide) a ring, round a
 *  symbol. The wave a tap on a spot starts from there (SCENE_OUTLINE_WAVE each), not from the finger. Returns how many
 *  went to fn. */
#define SCENE_OUTLINE_STEP 0.05f
#define SCENE_OUTLINE_GAP  0.05f
#define SCENE_OUTLINE_WAVE 0.07f        /* each bump's strength ..                         */
#define SCENE_OUTLINE_R    0.04f        /* .. and radius, glass units                       */
int scene_outline(const scene_t *sc, int n, void (*fn)(void *ctx, float x, float y), void *ctx);

/* A subtitle over a scene: what Molty says while a drawing is up, as the reader shows it
 * (one word at a time, its ORP letter (orp; -1: none, the word centred) in the accent and on
 * the vertical centre line) but small, at the bottom, shrunk until it fits inside the glass */
#define SCENE_CAPTION_Y     PKG_CAPTION_Y   /* glass units, y up: sized for here, shown at the layout's caption_y */
#define SCENE_CAPTION_SIZE  0.16f       /* type size at most, glass units                  */
#define SCENE_CAPTION_RGB   0x9aa0b8u   /* the ink under it                                */
/* each new word starts a tiny ripple under each of its letters: it reaches a dot or so past the
 * word and fades fast, a wobble that never breaks the drawing into dust (fluid_sim_impulse) */
#define SCENE_CAPTION_RIPPLE   0.12f    /* its strength (the design's 0.16, gentler for the panel's time) */
#define SCENE_CAPTION_RIPPLE_R 0.06f    /* its radius, glass units                         */
typedef struct { const char *str; int n, orp; } scene_caption_t;
/** The box a caption word's letters take at glass height y on a stage px wide: [x0, x1, y0, y1], stage px (y down). */
void scene_caption_box(const scene_caption_t *cap, float y, int stage, float box[4]);

/* The scene in high definition (fluid_draw_opts_t.shapes, .kdots): its items as shapes at the
 * panel's pixels (rings, strokes, discs, its symbols and letters, the subtitle's too; a ball
 * leaves a black disc, it hides what is behind) and the dots no shape draws (a ball, an image). */
typedef struct {
    fluid_shape_t *shapes;              /* FLUID_MAX_SHAPES                                 */
    int n;                              /* how many; -1 too many: draw the scene on the dots */
    uint8_t *keep;                      /* W x H, may be NULL                               */
    uint8_t *still;                     /* W x H, may be NULL: the cells a tapped spot's drawing covers at full
                                         * growth and one round (the water never breaks them), all 0 untapped */
} scene_shapes_t;
/** Over a scene in high definition the rings are shapes of it (the scene stays as it is under them): a thin line just
 *  inside the rim, in SCENE_RING_SEGS parts. A listen's (ring) draws itself round from 12 o'clock both ways and lets go
 *  the same way back; the agent's progress (prog) fills clockwise from 12 o'clock, or a comet circles (no percent, or
 *  thinking: the wait between a listen and its answer). The listen's wins where both are. Only the parts that are on
 *  are shapes, so a part coming or going draws its own few cells again. Into shp[n..) before the caption's letters
 *  (they stay last); returns the new count (n: too many, or n < 0). */
#define SCENE_RING_SEGS 48
#define SCENE_RING_R    0.936f          /* its line, of the radius: the largest circle whole inside the water's cells
                                         * (their first and last rows and columns are the glass's edge) */
#define SCENE_RING_HW   1.5f            /* half its width, px                              */
int scene_rings(fluid_shape_t *shp, int n, int stage, const fluid_ring_t *ring, const fluid_progress_t *prog, bool thinking,
                uint32_t listen_rgb, uint32_t prog_rgb);
/** A finger held to close the scene (hold: its share of SCENE_HOLD_S): from SCENE_HOLD_SHOW_S on, SCENE_HOLD_DOTS dots
 *  round the glass at SCENE_HOLD_R (inside a wheel's rail), filling clockwise from 12 o'clock, the rest faint. Into
 *  shp[n..) before the caption's letters; returns the new count. */
#define SCENE_HOLD_S      1.0f          /* held this long, still: the scene closes                                */
#define SCENE_HOLD_SHOW_S 0.25f         /* ... its ring shows from here (a tap never flashes it)                  */
#define SCENE_HOLD_SLOP   0.05f         /* glass units it may move (~12 px; a wheel's click is 0.19 at its rim)   */
#define SCENE_HOLD_DOTS   32
#define SCENE_HOLD_R      0.885f
#define SCENE_HOLD_RGB    0x7be0a0u     /* Molty's green, as the notch                                            */
int scene_hold_ring(fluid_shape_t *shp, int n, int stage, float hold);
/** The scene's shapes at t with its tapped spot (sc->focus) at full growth and lit, no caption: what its fine dust
 *  gathers round (fluid_fine_want). n of them (-1: too many); key: a hash of them (the same drawing, the same key). */
int scene_full_shapes(fluid_sim_t *s, const scene_t *sc, float t, fluid_shape_t *out, uint32_t *key);
/** Just the key scene_full_shapes would give (cheap: no shapes). */
uint32_t scene_full_key(const fluid_sim_t *s, const scene_t *sc, float t);

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
