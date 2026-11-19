// The page's water, page-side: the calm dust between the screens and the words, and the wave a
// tap sends through it. The dust is the firmware's fine dust for water with nothing drawn on it
// (device/firmware/main/fluid/fluid_fine.c: fixed single pixels where the ordered threshold,
// interleaved gradient noise, is under the density), with the firmware's own numbers. The numbers
// below are copies of the firmware's; scripts/tests/water.test.ts fails when they differ.
// Plain .mjs: the tile's script and the tests read it under Node.

/** fluid_fine.h, by name. */
export const FINE = {
  AMBIENT: 0.03, // FINE_AMBIENT: the dust's density far from any drawing
  PEAK: 0.85, // FINE_PEAK: ... and its most, on a drawing's edge
  LEVEL_FAR: 0.32, // FINE_LEVEL_FAR: a far speck's brightness
  LIGHT_FLOOR: 0.5, // FINE_LIGHT_FLOOR: the share of the density the light on the water never hides
  WATER: [107, 120, 160], // FINE_WATER_R, _G, _B
};

/** fluid_sim.c: the wave. c2 is its speed squared (cells a step, squared), 60 steps a second, on
 *  a grid 52 cells across a 412 px stage; leak is what a step keeps of the water's height. */
export const WAVE = { C2: 0.11, STEPS: 60, CELLS: 52, STAGE: 412, LEAK: 0.985 };
/** A wave's speed on a screen, as a share of the screen's width a second. */
export const WAVE_SPEED = (Math.sqrt(WAVE.C2) * WAVE.STEPS) / WAVE.CELLS;
/** What a wave keeps of its height after a second. */
export const WAVE_KEEP = WAVE.LEAK ** WAVE.STEPS;

/** The tile's side, px: the dust repeats every this far. */
export const TILE = 512;

/** fluid_fine.c ign(): a pixel's ordered threshold, 0..1, in single precision as there. */
export function ign(x, y) {
  const a = Math.fround(Math.fround(Math.fround(0.06711056) * x) + Math.fround(Math.fround(0.00583715) * y));
  const v = Math.fround(Math.fround(52.9829189) * Math.fround(a - Math.floor(a)));
  return Math.fround(v - Math.floor(v));
}

/** Calm water: the specks no light hides. */
export const CALM = FINE.AMBIENT * FINE.LIGHT_FLOOR;

/** A speck's colour at brightness `level` (0..1): on the dark page, or its ink on paper (the
 *  light theme: the water's hue, darker the brighter it would be on the dark page). */
export function speck(level, paper) {
  return FINE.WATER.map((c) => Math.round(paper ? 255 - (255 - c * 0.5) * Math.min(1, level * 1.3) : c * level));
}
