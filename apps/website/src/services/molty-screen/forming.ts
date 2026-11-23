// A heading's word forming in view (the landing's title, the first time: features/home/opening.ts).
// A word is otherwise played unseen and shown formed. Here the dust comes up rough and the word
// forms under it as calm spreads from its middle to its ends: the drawer scatters a line's dots
// where the water is steep and gathers them back where it is calm (fluid_draw.c HD_CALM), so the
// letters condense out of the window's own dust. It ends on the word as it is formed unseen: calm
// water, whole lines.

/** Wall seconds it plays in view (every frame drawn), then the word is as it always is. */
export const FORMING_S = 2.4;

const smooth = (x: number) => {
  const t = x <= 0 ? 0 : x >= 1 ? 1 : x;
  return t * t * (3 - 2 * t);
};
/** Rough water: a few crossing swells, moving (cells' heights, Q10 units of fluid_sim.h). */
const ROUGH = 3.2;
const rough = (x: number, y: number, t: number) =>
  ROUGH * (Math.sin(23 * x + 5.1 * t) * Math.sin(29 * y - 3.7 * t) + 0.7 * Math.sin(31 * (x + 0.6 * y) - 6.3 * t));

/** The water's height at (x, y) of the window (-1 .. 1, glass units) at `t` s: rough, calm from
 *  the middle out. */
export function formingWater(x: number, y: number, t: number) {
  const edge = 1.15 * smooth((t - 0.25) / 1.95);
  return rough(x, y, t) * smooth((Math.hypot(x, 2.5 * y) - edge) / 0.18 + 0.5);
}
