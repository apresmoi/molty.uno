/* fluid_ring.h — how a listen looks: a circle of the fluid (fluid_target_ring, a target like the
 * creature's) fixed just inside the rim, in ring_color. Units: stage units (1 = the rim; at 412 px
 * one unit = 206 px, and on the 52×32 grid a cell is ≈ 0.038 wide × 0.0625 tall). */
#pragma once

#define RING_R            0.875f   /* centre line, of the radius                          */
#define RING_CORE         0.35f    /* full-ink core, half-width in CELLS along the normal  */
#define RING_SOFT         1.00f    /* soft falloff beyond the core, in cells               */
#define RING_INK          0.20f    /* target ink (the creature is ~1, the dim background ~0.1) */
#define RING_FADE_S       0.3f     /* fade in when a listen starts / melt out when it ends */

/* The progress ring (fluid_target_progress): the same line, filling clockwise from 12 o'clock as the
 * agent's work goes; with no percent a short comet circles instead. */
#define RING_ASOFT        0.012f   /* soft ends of a part ring, turns                     */
#define PROG_EASE         4.f      /* the fill eases toward a new percent, 1/s            */
#define PROG_SPIN         0.45f    /* the comet, turns per second                         */
#define PROG_COMET        0.22f    /* the comet's length, turns                           */
#define PROG_TAIL_INK     0.55f    /* the fill's ink at 12 o'clock, of its head's         */
#define PROG_FLASH_S      0.7f     /* done: the full ring flashes this long, then melts    */
#define PROG_FLASH_INK    1.2f     /* the flash's extra ink, of RING_INK                   */
#define PROG_MELT_S       0.6f     /* it melts out over this long                          */
#define PROG_COLOR        0x7FE0D0u /* default: a mint glow, apart from the listening ring */
