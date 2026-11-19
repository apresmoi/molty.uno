/*
 * SPDX-License-Identifier: MIT
 *
 * fluid_fine.h: a scene's fine dust in high definition (packages/astrology/PORT-SPEC.md sections 3, 4, 7 and 8; the
 * reference is device/creatures/js/chart-engine.js). It replaces the braille haze of fluid_draw.c for such a scene.
 *
 *   specks   fixed single pixels. A pixel holds one when its density beats its ordered threshold (interleaved gradient
 *            noise: even, not clumpy). Density at d px from the drawing: 0.03 + 0.85 e^(-d/5), only 0.22 of the near term
 *            round a symbol; none on the drawing (d < 0.8). Brightness 0.32 .. 1 on the same falloff (0.6 x round a
 *            symbol), colour the water's mixed toward the nearest drawn pixel's by e^(-d/3). Built once per drawing (per
 *            tapped spot) from its shapes at full growth, on the sim task a few ms a frame (fluid_fine_work), and kept: the
 *            calm drawing's set stays while a tap shows its own.
 *   light    a light on the water (thin bright ridges where two slowly warping wave fronts cross) scales each speck's
 *            density, never its brightness: only the thin front of specks the bands' edges cross flips. About two
 *            thirds of the specks the light can never hide: a cell counts those once. The others are looked at a
 *            quarter of the cell rows a frame (FINE_PHASES), in turn.
 *   water    near a tapped spot's drawing or the caption (FINE_RIDE) a cell's dust rides the wave: all of it by the
 *            cell's shift in whole sub-dots, inside the cell (what leaves one side comes in at the other), so a shift
 *            costs no speck any work; never onto the drawing (a lit pixel keeps its own).
 *   caption  the reading's words sit on a dark backdrop (the specks thin to none round its box, a ragged edge that
 *            follows the grains; the drawing under it darker too). The box is the whole reading's (fluid_fine_caption_box),
 *            so the dark holds still from word to word. A new word forms in a dithered order
 *            (fluid_draw_opts_t.cap_unformed), the one before breaks into specks that scatter, ride the water and fade
 *            into it; the reading's last one slower and farther, the dark going in a few steps with it.
 *   budget   what a frame draws again is what changed (each cell a signature). The caption's own goes at once; the
 *            light's and the water's on their rows' turns, within the frame's quota (fluid_draw_pace). A first set, and
 *            a scene in a transition, come and go row by row behind a wavy line (fluid_fine_turn): no frame sends the
 *            whole panel.
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

#define FINE_SETS        3              /* the calm drawing's, a tapped spot's, and one building */
#define FINE_PARTICLES   6000           /* a word breaking up: its pixels, at most              */
/* the dust (chart-engine.js AMBIENT .. DRAWN) */
#define FINE_AMBIENT     0.03f
#define FINE_PEAK        0.85f
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
#define FINE_PATCH1      72.f           /* .. to all the dust (the page: 40)                     */
#define FINE_PATCH_RAG   30.f           /* px: the edge as ragged as the grains (the page: 26)   */
#define FINE_DIM         0.65f          /* the drawing under the word: this much darker          */
#define FINE_REVEAL_S    0.09f          /* a word forms over this                                */
#define FINE_SWAP_S      0.22f          /* the word before breaks up over this ..                */
#define FINE_SWAP_SPREAD 7.f            /* .. its specks scattering this far, px                 */
#define FINE_END_S       0.7f           /* the reading's last word: slower ..                    */
#define FINE_END_SPREAD  26.f           /* .. and farther                                        */
#define FINE_GRAIN       0.7f           /* of a word's pixels, this many become specks           */
#define FINE_RIDE        64.f           /* px: the dust rides the water this near a tapped drawing or the caption only */
#define FINE_CATCHUP     8000           /* specks worked out off their row's turn a frame, at most (a set swapped in) */
#define FINE_REVEAL_SET_S 0.5f          /* a first set forms cell by cell over this              */
#define FINE_DARK_STEPS  4              /* the backdrop fades out in this many steps (each works the cells by it out again) */

typedef struct {
    uint32_t key;                       /* the drawing it was built from, 0 = empty              */
    int n, cap;
    uint32_t *pos;                      /* x | y << 9 | q << 18 | near << 25: its pixel, its threshold over its
                                         * density (0..127) and how much the light matters there (0..127)      */
    uint16_t *col;                      /* its colour at its brightness, rgb565 (little-endian)  */
    uint32_t *start;                    /* N + 1: each cell's specks, cell order: those the light never hides first */
    uint16_t *nstat;                    /* N: how many of them (shown whatever the light)        */
    uint32_t *ssig;                     /* N: their signature, calm (no shift, no caption near)  */
} fine_set_t;

typedef struct fluid_fine {
    int W, H, N, S, ox, oy;             /* the grid, the stage (px) and where it sits in the fb  */
    int16_t *cx, *cy;                   /* W + 1, H + 1 cell edges, stage px                     */
    uint8_t *cellx, *celly;             /* S: the cell column / row of a stage px                */
    /* the sets: the sim task builds, the drawer draws `live` (swapped with the drawer idle) */
    fine_set_t set[FINE_SETS];
    int live, ready, build;             /* slots, -1 none                                        */
    uint32_t keep;                      /* the calm drawing's key: its set stays                 */
    /* the build (sim task) */
    fluid_shape_t *shp;                 /* FLUID_MAX_SHAPES: the drawing at full growth          */
    int16_t (*box)[4];
    int64_t build_t0; int build_us, build_calls;   /* the build's time, for the log */
    int nshp, nsym, pal, phase, row;   /* nsym: its symbols, copied after it (a symbol's surround is fainter) */
    uint32_t bkey;                      /* the drawing being built                               */
    uint16_t *dist, *src;               /* S x S: distance to the drawing (1/100 px), the nearest drawn pixel's
                                         * colour (rgb444) | 0x8000 a symbol's                   */
    uint16_t *rowbuf;                   /* S: a raster row                                       */
    uint32_t *bpos; uint16_t *bcol; uint16_t *bcell; int bcap;   /* bcell: column x 2 + (the light can hide it) */   /* one cell row's specks, before they are sorted */
    /* the drawer's frame */
    const fine_set_t *drawn;            /* the set drawn (its levels below), NULL none           */
    uint8_t *lv, *lv2;                  /* per speck: its level (0 hidden .. 255); a spare for the next set */
    int lvcap, lv2cap;
    int8_t *sx, *sy;                    /* N: each cell's shift, stage px (its dust rides the water inside the cell) */
    uint32_t *tot;                      /* N: the cell's own specks' signature, as last worked out */
    uint32_t *stat;                     /* N: ... the part of those the light never hides         */
    uint32_t *eff;                      /* N: what that part was worked out for (1 plain, else the backdrop's gen; 0 not yet) */
    uint8_t *vis;                       /* N: the cell's dust shows this frame                    */
    uint8_t *imm;                       /* N: its change goes to the panel at once (the caption's), not on its row's turn */
    uint8_t *ride;                      /* N: the dust rides the water there (near a tapped drawing or the caption) */
    uint32_t ride_key;                  /* what ride was worked out for                          */
    uint8_t lrow;                       /* which quarter of the light's grid rows, and of the cell rows' light-dependent
                                         * specks, is worked out this frame (FINE_PHASES)        */
    bool lgrid_ok;                      /* the light's grid was worked out whole once             */
    float reveal, last_t;               /* a first set forms cell by cell: 0..1; the last frame's time */
    uint32_t gen;                       /* the caption's backdrop as the cells by it are worked out for (box, dark) */
    float dim[5];                       /* the drawing under the caption's backdrop: its box, how dark (fluid_shapes_row_dim) */
    uint32_t step_key;                  /* the forming word's step, last frame                    */
    uint32_t prof_frame, prof_rect, prof_n, prof_eval;   /* us spent (fluid_fine_frame, fluid_fine_rect) and specks worked out over prof_n frames */
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
    int16_t pbb[4];                     /* ... and their box: x0, x1, y0, y1 (x1 < x0 none)       */
} fluid_fine_t;

/** The dust for a W x H grid on an fb_w x fb_h panel (its largest centred square: the stage). NULL: no memory. */
fluid_fine_t *fluid_fine_create(int W, int H, int fb_w, int fb_h);
void fluid_fine_destroy(fluid_fine_t *f);

/* ── the sim task ── */
/** The dust wanted now: for the drawing `key` (non-zero; the same key, the same drawing), its shapes at full growth
 *  (stage px; caption letters are left out). keep: the scene's calm drawing, whose set is kept to come back to. A set
 *  built already is swapped in at once; else a build starts (any other build is dropped). */
void fluid_fine_want(fluid_fine_t *f, uint32_t key, bool keep, const fluid_shape_t *sh, int n, int palette);
/** Advance the build for about budget_us (<= 0: to its end). True: a set waits for fluid_fine_swap. */
bool fluid_fine_work(fluid_fine_t *f, int budget_us);
/** With the drawer idle: the set waiting becomes the one drawn. Cells whose specks are the same in both keep their
 *  levels (nothing of theirs is drawn again); a first set forms cell by cell. */
void fluid_fine_swap(fluid_fine_t *f);
/** With the drawer idle: the caption's backdrop for the whole reading (stage px [x0, x1, y0, y1], the union of its
 *  words' boxes), so the dark does not move with each word; NULL: each word's own box. */
void fluid_fine_caption_box(fluid_fine_t *f, const float box[4]);
/** How long the build wanted last took, us (0 while it runs or none), and in how many fluid_fine_work calls. */
int fluid_fine_built(const fluid_fine_t *f, int *calls);
/** No scene: every set and the build's buffers freed (the next want builds again). The drawer must be idle. */
void fluid_fine_clear(fluid_fine_t *f);
/** The specks of the set drawn (0 none). */
static inline int fluid_fine_specks(const fluid_fine_t *f) { return f && f->live >= 0 ? f->set[f->live].n : 0; }

/* ── the drawer (fluid_draw.c) ── */
/** This frame's dust at o->time: each cell's signature (f->sig: a cell whose signature differs from f->sig0, as drawn,
 *  is drawn again: at once where f->imm says, else on its row's turn), its specks' levels and its shift, the caption's
 *  backdrop and specks. Cell rows j with (j + phase) % m == 0 follow the water this frame. */
void fluid_fine_frame(fluid_fine_t *f, const fluid_frame_t *s, const fluid_draw_opts_t *o, int phase, int m);
/** When cell (i, j) of a scene on fine dust (H cell rows) is there in a transition: once its dust (1 gone .. 0 whole)
 *  is under this. No scatter: the scene rises from the bottom behind a wavy water line, and drains from the top (the
 *  cells that turn in a frame are a few runs along a few rows: few windows; turning all over, each would be one). */
float fluid_fine_turn(int i, int j, int H);
/** The dust over the window t (fb px), after the shapes and dots: on black (or a dim pixel), never on a lit one;
 *  all: every cell of t (a window), else only the dirty cells (a framebuffer kept). */
void fluid_fine_rect(const fluid_fine_t *f, const uint8_t *dirty, const fluid_rect_t *t, bool all);

#ifdef __cplusplus
}
#endif
