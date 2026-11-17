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
 *   light    each frame a light on the water (thin bright ridges where two slowly warping wave fronts cross) scales each
 *            speck's density, never its brightness: only the thin front of specks the bands' edges cross flips.
 *   water    a speck rides the wave in whole sub-dots (its cell's shift, as the dots refract): it moves only when the
 *            rounded shift does (the panel's cost), and never onto the drawing (a lit pixel keeps its own).
 *   caption  the reading's word sits on a dark backdrop (the specks thin to none round its box, a ragged edge that follows
 *            the grains); a new word forms in a dithered order (fluid_draw_opts_t.cap_unformed), the one before breaks into
 *            specks that scatter, ride the water and fade into it; the reading's last one slower and farther, the dark
 *            filling back in with it.
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
#define FINE_RIDE        100.f          /* px: the dust rides the water this near a tapped drawing or the caption only */

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
    int nshp, nsym, pal, phase, row;   /* nsym: its symbols, copied after it (a symbol's surround is fainter) */
    uint32_t bkey;                      /* the drawing being built                               */
    uint16_t *dist, *src;               /* S x S: distance to the drawing (1/100 px), the nearest drawn pixel's
                                         * colour (rgb444) | 0x8000 a symbol's                   */
    uint16_t *rowbuf;                   /* S: a raster row                                       */
    uint32_t *bpos; uint16_t *bcol; uint16_t *bcell; int bcap;   /* bcell: column x 2 + (the light can hide it) */   /* one cell row's specks, before they are sorted */
    /* the drawer's frame */
    const fine_set_t *drawn;            /* the set the levels below are for                      */
    uint8_t *lv;                        /* per speck: its level this frame (0 hidden .. 255)     */
    int lvcap;
    int8_t *sx, *sy;                    /* N: each cell's shift this frame, stage px             */
    uint8_t ilace;                      /* which cell rows' shifts follow the water this frame   */
    uint8_t *slow;                      /* N: how the cell's specks were last worked out: 0 calm, 1 one by one, 2 calm by the caption */
    uint32_t *dsig;                     /* N: a calm cell's light-dependent specks' signature, last worked out */
    uint32_t *bsig;                     /* N: by the caption, its others' (dimmed by the dark), worked out once a box */
    uint32_t box_key;                   /* the box and dark bsig is for                          */
    float dim[5];                       /* the drawing under the caption's backdrop: its box, how dark (fluid_shapes_row_dim) */
    uint8_t *ride;                      /* N: the dust rides the water there (near a tapped drawing or the caption) */
    uint32_t ride_key;                  /* what ride was worked out for                          */
    int mx, my;                         /* the largest shift this frame, px (the windows' reach) */
    uint8_t lrow;                       /* which half of the light's grid rows moves this frame  */
    bool lall;                          /* all of it next frame (a new set)                      */
    uint32_t prof_frame, prof_rect, prof_n;   /* us spent (fluid_fine_frame, fluid_fine_rect) over prof_n frames */
    uint32_t *sig, *sig0;               /* N: what each cell's dust is this frame, and as last drawn */
    uint8_t *lgrid;                     /* the light, its grid                                   */
    int lg;
    /* the caption: the word shown, the one breaking up */
    unsigned word;
    float word_t, prev_t, cbox[4], pbox[4];  /* the word's box, the one before's: [x0, x1, y0, y1] stage px */
    bool end;                           /* the last word broke up (no word after it)             */
    fluid_shape_t cap[FLUID_MAX_LETTERS];
    int ncap;
    int np, npd;                        /* its specks; those drawn this frame (indices below)     */
    int16_t *px0, *py0; uint16_t *pc; uint8_t *pa, *pg, *pang;   /* home px, colour, alpha, hash, angle   */
    int16_t *ppx, *ppy; uint16_t *ppc;  /* this frame's (ppx < 0: none)                          */
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
/** With the drawer idle: the set waiting becomes the one drawn. */
void fluid_fine_swap(fluid_fine_t *f);
/** No scene: every set and the build's buffers freed (the next want builds again). The drawer must be idle. */
void fluid_fine_clear(fluid_fine_t *f);
/** The specks of the set drawn (0 none). */
static inline int fluid_fine_specks(const fluid_fine_t *f) { return f && f->live >= 0 ? f->set[f->live].n : 0; }

/* ── the drawer (fluid_draw.c) ── */
/** This frame's dust at o->time: each speck's level, each cell's shift and signature (f->sig: a cell whose
 *  signature changed is drawn again), the caption's backdrop and specks. */
void fluid_fine_frame(fluid_fine_t *f, const fluid_frame_t *s, const fluid_draw_opts_t *o);
/** The dust over the window t (fb px), after the shapes and dots: on black (or a dim pixel), never on a lit one;
 *  all: every cell of t (a window), else only the dirty cells (a framebuffer kept). */
void fluid_fine_rect(const fluid_fine_t *f, const uint8_t *dirty, const fluid_rect_t *t, bool all);

#ifdef __cplusplus
}
#endif
