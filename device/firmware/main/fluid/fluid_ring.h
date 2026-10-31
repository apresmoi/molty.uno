/* fluid_ring.h — how a listen looks: a circle of the fluid (fluid_target_ring, a target like the
 * creature's) fixed just inside the rim, in ring_color. Units: stage units (1 = the rim; at 412 px
 * one unit = 206 px, and on the 52×32 grid a cell is ≈ 0.038 wide × 0.0625 tall). */
#pragma once

#define RING_R            0.875f   /* centre line, of the radius                          */
#define RING_CORE         0.35f    /* full-ink core, half-width in CELLS along the normal  */
#define RING_SOFT         1.00f    /* soft falloff beyond the core, in cells               */
#define RING_INK          0.20f    /* target ink (the creature is ~1, the dim background ~0.1) */
#define RING_FADE_S       0.3f     /* fade in when a listen starts / melt out when it ends */
