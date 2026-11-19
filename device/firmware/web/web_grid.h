/*
 * SPDX-License-Identifier: MIT
 *
 * web_grid.h: the largest grid a web build's water may have. The chip's cap (fluid_sim.h: 64 x 40, a 466 px panel's)
 * is its memory's; a browser has room for a heading's wide window and for the page's own water (web_field.c). Included
 * after fluid_sim.h by the two files that size by the cap: web_sim.c (fluid_sim.c) and web_molty.c (mole_fluid.c).
 * A 466 px panel's grid is what it is on the device (fluid_grid_for_stage: 59 x 36), whatever the cap.
 */
#pragma once
#include "fluid_sim.h"
#undef FLUID_GRID_MAX_W
#undef FLUID_GRID_MAX_H
#define FLUID_GRID_MAX_W 416
#define FLUID_GRID_MAX_H 256
