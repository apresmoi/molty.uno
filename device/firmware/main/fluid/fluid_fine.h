/*
 * SPDX-License-Identifier: MIT
 *
 * fluid_fine.h: a scene's fine dust in high definition (packages/astrology/PORT-SPEC.md sections 3, 4, 7 and 8; the
 * reference is device/creatures/js/chart-engine.js). It replaces the braille haze of fluid_draw.c for such a scene.
 *
 *   specks   fixed single pixels. A pixel holds one when its density beats its ordered threshold (interleaved gradient
 *            noise: even, not clumpy). Density at d px from the drawing: 0.03 + 0.40 e^(-d/5), only 0.22 of the near term
 *            round a symbol; none on the drawing (d < 0.8). Brightness 0.32 .. 1 on the same falloff (0.6 x round a
 *            symbol), colour the water's mixed toward the nearest drawn pixel's by e^(-d/3). Built once per drawing (per
 *            tapped spot) from its shapes at full growth, on the sim task a few ms a frame (fluid_fine_work), and kept: the
 *            calm drawing's set stays while a tap shows its own.
 *   light    a light on the water (thin bright ridges where two slowly warping wave fronts cross) scales each speck's
 *            density, never its brightness: only the thin front of specks the bands' edges cross flips. About two
 *            thirds of the specks the light can never hide: a cell counts those once. The others are looked at a
 *            quarter of the cell rows a frame (FINE_PHASES), in turn: the light drifts ~0.3 px a frame, so a row's
 *            turn is never a pixel late (the one thing that goes on turns: nothing the eye follows).
 *   water    all the dust rides the water, every frame, over the whole glass: a cell's shift is the water's slope there
 *            as the creature screen's dots take it (DOT_REFR), FINE_RIDE of it, in whole px, at most a sub-dot across and two down, plus
 *            a calm drift (FINE_DRIFT_*: a slow sway of a pixel either way, so the dust is never frozen at rest). All of
 *            the cell's dust moves by it inside the cell (what leaves one side comes in at the other), so a shift costs no
 *            speck any work and changes only that cell; never onto the drawing (a lit pixel keeps its own). While the
 *            picture changes whole the shifts hold (pause), then take the water again.
 *   caption  the reading's words sit on a dark backdrop (the specks thin to none round its box, a ragged edge that
 *            follows the grains; the drawing under it darker too), coming from the box outward as the reading begins.
 *            The box is the whole reading's (fluid_fine_caption_box),
 *            so the dark holds still from word to word. A new word forms in a dithered order
 *            (fluid_draw_opts_t.cap_unformed), the one before breaks into specks that scatter, ride the water and fade
 *            into it; the reading's last one slower and farther, the dark lifting meanwhile as a front from its box
 *            outward (a frame changes only the cells it crosses).
 *   budget   what a frame draws again is what changed (each cell a signature), all of it that frame. A tapped spot's set
 *            changes only the cells by its drawing; a scene's whole set comes in before the drawer's change of picture
 *            draws its new one (fluid_morph_t: its cloud waits for it): the light holds still meanwhile (pause).
 * The drawer's part (fluid_fine_frame, fluid_fine_rect) is called by fluid_draw_cells / fluid_draw_rect when
 * fluid_draw_opts_t.fine is set; the sim task owns the build (fluid_fine_want, _work) and swaps a finished set in while
 * the drawer is idle (fluid_fine_swap). Plain C: built into the firmware and by the host tests.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "fluid_sim.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FINE_SETS        4              /* the calm drawing's, a tapped spot's, one building, and one kept from before
                                         * (a set is found again by its drawing: back at a chart, its dust is there) */
#define FINE_PARTICLES   6000           /* a word breaking up: its pixels, at most              */
/* the dust (chart-engine.js AMBIENT .. DRAWN) */
#define FINE_AMBIENT     0.03f
#define FINE_PEAK        0.40f          /* (the page: 0.85; Juan: half the dust along the lines)  */
#define FINE_FALL        5.f            /* px                                                   */
#define FINE_SYM         0.22f          /* the near term round a symbol                          */
#define FINE_LEVEL_FAR   0.32f
#define FINE_TINT_FALL   3.f            /* px                                                   */
#define FINE_DRAWN       40             /* a drawn pixel: a channel brighter than this           */
#define FINE_WATER_R     107
#define FINE_WATER_G     120
#define FINE_WATER_B     160
/* the light on the water (chart-engine.js light()) */
#define FINE_LIGHT_FLOOR 0.5f
#define FINE_LIGHT_GRID  8              /* px: sampled on this grid, bilinear (the page: 4; its ridges are ~20 px wide) */
#define FINE_LIGHT_SPEED 0.55f
#define FINE_PHASES      4              /* the light's changes go in every 4th cell row a frame (the panel's time; the
                                         * light moves ~0.5 px a frame, so a row a cell high changes about as often) */
/* the caption (chart-engine.js PATCH .. GRAIN) */
#define FINE_PATCH0      10.f           /* px from the word's box: dark .. (the page: 4; Juan: darker, wider) */
#define FINE_PATCH1      48.f           /* .. to all the dust (the page: 40; wider, its front crossing it fits a frame) */
#define FINE_PATCH_RAG   30.f           /* px: the edge as ragged as the grains (the page: 26)   */
#define FINE_DIM         0.65f          /* the drawing under the word: this much darker          */
#define FINE_REVEAL_S    0.f            /* a word forms over this (0: whole on its first frame, on its beat; the page: 0.09) */
#define FINE_SWAP_S      0.13f          /* the word before breaks up over this (the page: 0.22: at reading pace most of
                                         * each word's time had the one before still scattering round it) .. */
#define FINE_SWAP_SPREAD 7.f            /* .. its specks scattering this far, px                 */
#define FINE_DARK_IN_S   0.5f           /* the backdrop comes from the word's box outward over this */
#define FINE_END_S       0.7f           /* the reading's last word: slower ..                    */
#define FINE_END_SPREAD  26.f           /* .. and farther                                        */
#define FINE_GRAIN       0.7f           /* of a word's pixels, this many become specks           */
/* the calm drift: two slow plane waves across the glass sway each cell's dust (fluid_fine_frame) */
#define FINE_DRIFT_PX    1.4f           /* px at most (rounded: a pixel either way, never more)    */
#define FINE_DRIFT_S     8.f            /* s a sway: a cell moves a pixel about once a second      */
#define FINE_RIDE        0.7f           /* the water moves the dust this much of what it moves the dots */
#define FINE_RIDE_FRAMES 6              /* a cell's dust rides the water's front this many frames (~25 px of it: the
                                         * band a tap's wave moves stays thin, the panel's frame holds it) .. */
#define FINE_RIDE_REST   12             /* .. then rests till the water there is calm this many frames */
#define FINE_CATCHUP     1500           /* specks of cells never worked out, off their row's turn a frame, at most */

typedef struct {
    uint32_t key;                       /* the drawing it was built from (a hash of its shapes), 0 = empty */
    uint32_t base;                      /* the scene it was last wanted for                      */
    uint32_t used;                      /* when it was last wanted (the oldest goes first)       */
    int n, cap;
    uint32_t *pos;                      /* x | y << 9 | q << 18 | near << 25: its pixel, its threshold over its
                                         * density (0..127) and how much the light matters there (0..127)      */
    uint16_t *col;                      /* its colour at its brightness, rgb565 (little-endian)  */
    uint32_t *start;                    /* N + 1: each cell's specks, cell order: those the light never hides first */
    uint16_t *nstat;                    /* N: how many of them (shown whatever the light)        */
    uint32_t *ssig;                     /* N: their signature, calm (no shift, no caption near)  */
} fine_set_t;

typedef struct fluid_fine {
    int W, H, N, S, S2, ox, oy;         /* the grid, the stage (px; S2: half of it, the build's field) and where it sits in the fb */
    int16_t *cx, *cy;                   /* W + 1, H + 1 cell edges, stage px                     */
    uint8_t *cellx, *celly;             /* S: the cell column / row of a stage px                */
    /* the sets: the sim task builds, the drawer draws `live` (swapped with the drawer idle) */
    fine_set_t set[FINE_SETS];
    int live, ready, build;             /* slots, -1 none                                        */
    uint32_t keep;                      /* the calm drawing's key: its set stays                 */
    uint32_t scene;                     /* the scene the dust is for now (fluid_fine_want)       */
    uint32_t clock;                     /* counts the wants (fine_set_t.used)                    */
    bool levels_ok;                     /* the set that waits has its levels (it may swap in)    */
    const fine_set_t *lv_base;          /* ... worked out against this drawn set                 */
    uint8_t *fresh;                     /* N: the cell takes those levels at the swap (else it keeps its own) */
    uint32_t *tot2;                     /* N: ... and this signature                             */
    uint8_t *lgrid2;                    /* the light they were worked out against                */
    uint8_t *lit;                       /* S x S bits: the drawing's own pixels (no speck there) */
    /* the build (sim task) */
    fluid_shape_t *shp;                 /* FLUID_MAX_SHAPES: the drawing at full growth          */
    int16_t (*box)[4];
    int64_t build_t0; int build_us, build_calls;   /* the build's time, for the log */
    int nshp, nsym, pal, phase, row;   /* nsym: its symbols, copied after it (a symbol's surround is fainter) */
    uint32_t bkey;                      /* the drawing being built                               */
    uint16_t *dist, *src;               /* S2 x S2 (2 x 2 px blocks): distance to the drawing (1/100 px), the nearest
                                         * drawn block's colour (rgb444) | 0x8000 a symbol's     */
    uint16_t *rowbuf;                   /* S: a raster row                                       */
    uint32_t *bpos; uint16_t *bcol; uint16_t *bcell; int bcap;   /* bcell: column x 2 + (the light can hide it) */   /* one cell row's specks, before they are sorted */
    /* the drawer's frame */
    const fine_set_t *drawn;            /* the set drawn (its levels below), NULL none           */
    uint8_t *lv, *lv2;                  /* per speck: its level (0 hidden .. 255); a spare for the next set */
    int lvcap, lv2cap;
    int8_t *sx, *sy;                    /* N: each cell's shift, stage px (its dust rides the water and the drift inside the cell) */
    uint8_t *ride;                      /* N: the water's front at the cell (FINE_RIDE_FRAMES): 0 none, 1.. riding, past it
                                         * resting till the water is calm (FINE_RIDE_REST frames)  */
    int8_t *dq, *wq;                    /* 2N each: the drift's part of it and the water's (in its steps), x then y, each
                                         * step held until the sway or the slope is well past it (never back and forth) */
    float *dcol;                        /* 4W: sin, cos of each drift wave's column part          */
    uint32_t *tot;                      /* N: the cell's own specks' signature, as last worked out */
    uint32_t *stat;                     /* N: ... the part of those the light never hides         */
    uint32_t *eff;                      /* N: what that part was worked out for (1 plain, else the backdrop's gen; 0 not yet) */
    uint8_t *vis;                       /* N: the cell's dust shows this frame                    */
    uint8_t *imm;                       /* N: the word's own cells this frame (a word forming)    */
    uint8_t *why;                       /* N: what moves its dust this frame (1 << FLUID_ANIM_WORD, _WATER) */
    bool rim;                           /* a ring on the rim (fluid_fine_rim)                    */
    bool pause;                         /* the light and the specks' shifts hold still this frame (the picture is changing
                                         * whole: the drawer's) */
    uint8_t lrow;                       /* which quarter of the light's grid rows, and of the cell rows' light-dependent
                                         * specks, is worked out this frame (FINE_PHASES)        */
    bool lgrid_ok;                      /* the light's grid was worked out whole once             */
    float last_t;                       /* the last frame's time                                 */
    float light_t;                      /* the light's own clock: it stands while the light holds (pause), so it goes on
                                         * from there after, never all at once                    */
    uint32_t gen;                       /* the caption's backdrop as the cells by it are worked out for (box, dark) */
    float dim[7];                       /* the drawing under the caption's backdrop: its box, how dark, from and to how far (fluid_shapes_row_dim) */
    bool dark_on; float dark_t0;        /* the backdrop is up, since                             */
    uint32_t step_key;                  /* the forming word's step, last frame                    */
    uint32_t prof_frame, prof_rect, prof_n, prof_eval;   /* us spent (fluid_fine_frame, fluid_fine_rect) and specks worked out over prof_n frames */
    uint32_t prof_drift, prof_specks;   /* .. of fluid_fine_frame: the shifts' loop, the specks' loop (us) */
    uint32_t *sig, *sig0;               /* N: what each cell's dust is this frame, and as last drawn */
    uint8_t *lgrid;                     /* the light, its grid                                   */
    int lg;
    /* the caption: the word shown, the one breaking up */
    unsigned word;
    float word_t, prev_t, cbox[4], pbox[4];  /* the word's box, the one before's: [x0, x1, y0, y1] stage px */
    float want_box[4], bbox[4];         /* the backdrop's box: as given (fluid_fine_caption_box; x1 < x0: each word's own), in use */
    bool end;                           /* the last word broke up (no word after it)             */
    fluid_shape_t cap[FLUID_MAX_LETTERS];
    int ncap;
    int np, npd;                        /* its specks; those drawn this frame (indices below)     */
    int16_t *px0, *py0; uint16_t *pc; uint8_t *pa, *pg, *pang;   /* home px, colour, alpha, hash, angle   */
    int16_t *ppx, *ppy; uint16_t *ppc;  /* this frame's (ppx < 0: none)                          */
    int16_t pbb[4], pbb0[4];            /* ... and their box: x0, x1, y0, y1 (x1 < x0 none); last frame's */
    /* each cell's specks as last drawn (fluid_fine_rect: the cell whole), for fluid_fine_move */
    uint8_t *lvd; int lvdcap;           /* per speck of the set drawn: its level drawn            */
    int8_t *sxd, *syd;                  /* N: the shift drawn                                     */
    uint8_t *dok;                       /* N: those are known (the set's, the cell drawn whole)   */
} fluid_fine_t;

/* EXPERIMENT (scene dust A/B, for Juan to compare in the simulator; delete once he picks). Process-wide, any time:
 *   kind  FINE_DUST_SPECKS (0, the default): the fine specks as above.
 *         FINE_DUST_DOTS (1): the creature screen's dust instead: braille dots on the water's cells, denser and more
 *         ordered near the drawing, loose far away, every one riding the wave over the whole glass (fluid_draw.c),
 *         each change sent the frame it happens.
 *         FINE_DUST_DOTS_ILACE (2): the same, the water's own changes sent in every other cell row a frame, rows
 *         alternating, as the creature screen sends its own (the drawing's and the caption's changes at once).
 *         FINE_DUST_FINE_DOTS (3): the fine specks exactly as kind 0, and over them a sparse
 *         layer of the creature screen's dots on the same lattice and the same water: they twinkle at rest and ride a
 *         tap's wave over the whole glass, each change sent the frame it happens; drawn on black pixels only (never on
 *         the drawing, a speck or the caption's backdrop).
 *   hard  percent: kind 3's density, the share of the lattice's dots away from the drawing that carry a hard dot
 *         (FINE_HARD_PCT by default, 0..25). */
#define FINE_DUST_SPECKS 0
#define FINE_DUST_DOTS   1
#define FINE_DUST_DOTS_ILACE 2
#define FINE_DUST_FINE_DOTS  3
#define FINE_HARD_PCT    1
#define FINE_HARD_MAX    25
void  fluid_fine_dust_kind(int kind);
int   fluid_fine_dust_kind_get(void);
void  fluid_fine_hard_density(int percent);
int   fluid_fine_hard_density_get(void);
/** kinds whose dust is the fine specks (0 and 3) */
static inline bool fluid_fine_specks_kind(int kind) { return kind == FINE_DUST_SPECKS || kind == FINE_DUST_FINE_DOTS; }

/** The dust for a W x H grid on an fb_w x fb_h panel (its largest centred square: the stage). NULL: no memory. */
fluid_fine_t *fluid_fine_create(int W, int H, int fb_w, int fb_h);
void fluid_fine_destroy(fluid_fine_t *f);

/* ── the sim task ── */
/** The dust wanted now, for scene `scene` (any number that is the scene's own: a set made for another scene never
 *  shows on this one, fluid_fine_swap): for the drawing `key` (non-zero; the same key, the same drawing: a hash of its shapes), its
 *  shapes at full growth (stage px; caption letters are left out). keep: the scene's calm drawing. A set built before
 *  (FINE_SETS are kept) swaps in after a pass for its levels; else a build starts. False: not now (the scene's calm set
 *  is being built and is not given up for a tapped spot's): ask again. */
bool fluid_fine_want(fluid_fine_t *f, uint32_t scene, uint32_t key, bool keep, const fluid_shape_t *sh, int n, int palette);
/** Advance the build for about budget_us (<= 0: to its end). True: a set waits for fluid_fine_swap. */
bool fluid_fine_work(fluid_fine_t *f, int budget_us);
/** The dust drawn is scene `scene`'s own (fluid_fine_want's number), swapped in. */
static inline bool fluid_fine_shown(const fluid_fine_t *f, uint32_t scene) { return f && f->drawn && f->drawn->base == scene; }
/** With the drawer idle: the set waiting becomes the one drawn. Cells whose specks are the same in both keep their
 *  levels (nothing of theirs is drawn again). A set drawn for another scene's drawing is dropped at once (the dust on
 *  the glass is the scene's own). True: most of the glass's dust changed (a scene's own set: it comes in while a change of
 *  picture has drawn nothing of its new one yet, fluid_draw_morph_open). */
bool fluid_fine_swap(fluid_fine_t *f);
/** With the drawer idle: the caption's backdrop for the whole reading (stage px [x0, x1, y0, y1], the union of its
 *  words' boxes), so the dark does not move with each word; NULL: each word's own box. */
void fluid_fine_caption_box(fluid_fine_t *f, const float box[4]);
/** With the drawer idle: a ring is on the rim (a listen's: the voice touches the water there), so the drawing never
 *  breaks into dots under it (fluid_draw.c). */
void fluid_fine_rim(fluid_fine_t *f, bool on);
/** How long the build wanted last took, us (0 while it runs or none), and in how many fluid_fine_work calls. */
int fluid_fine_built(const fluid_fine_t *f, int *calls);
/** No scene: every set and the build's buffers freed (the next want builds again). The drawer must be idle. */
void fluid_fine_clear(fluid_fine_t *f);
/** The specks of the set drawn (0 none). */
static inline int fluid_fine_specks(const fluid_fine_t *f) { return f && f->live >= 0 ? f->set[f->live].n : 0; }

/* ── the drawer (fluid_draw.c) ── */
/** This frame's dust at o->time: each cell's signature (f->sig: a cell whose signature differs from f->sig0, as drawn,
 *  is drawn again), its specks' levels and its shift, the caption's backdrop and specks, and what moved it (f->why). */
void fluid_fine_frame(fluid_fine_t *f, const fluid_frame_t *s, const fluid_draw_opts_t *o);
/** The light on the water at stage px (x, y), 0..1 (bilinear on its grid; as last worked out by fluid_fine_frame). */
static inline float fluid_fine_light(const fluid_fine_t *f, float x, float y) {
    const float gx = x * (1.f / FINE_LIGHT_GRID), gy = y * (1.f / FINE_LIGHT_GRID);
    int ix = (int)gx, iy = (int)gy;
    ix = ix < 0 ? 0 : ix > f->lg - 2 ? f->lg - 2 : ix; iy = iy < 0 ? 0 : iy > f->lg - 2 ? f->lg - 2 : iy;
    const float ax = gx - ix, ay = gy - iy;
    const uint8_t *g0 = f->lgrid + iy * f->lg + ix;
    return ((g0[0] * (1.f - ax) + g0[1] * ax) * (1.f - ay) + (g0[f->lg] * (1.f - ax) + g0[f->lg + 1] * ax) * ay) * (1.f / 255.f);
}
/** The dust over the window t (fb px), after the shapes and dots: on black (or a dim pixel), never on a lit one;
 *  all: every cell of t (a window), else only the dirty cells (a framebuffer kept). */
void fluid_fine_rect(const fluid_fine_t *f, const uint8_t *dirty, const fluid_rect_t *t, bool all);
/** Cell k of a kept framebuffer fb (fb px, stride), black but for its specks (no drawing, dot or letter in it): its specks
 *  as last drawn set to black, then put as they are now (a few pixels, not the cell's). False: not known as drawn (or the
 *  word's breaking specks reach it): draw it whole. */
bool fluid_fine_move(fluid_fine_t *f, int k, uint16_t *fb, int stride);
/** The glass shows every cell's specks as they are now (a change of picture ended on the look whole). */
void fluid_fine_drawn(fluid_fine_t *f);

#ifdef __cplusplus
}
#endif
