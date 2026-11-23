// A heading's word forming in view (the landing's intro preview, features/home/intro.ts). A word
// is otherwise played unseen and shown formed. Here the firmware's own formation plays on screen
// (a scene forms from dust: scene_player.h SCENE_DUST_S), and the page may give its water a
// roughness that calms as it goes: the drawer scatters a line's dots where the water is steep
// and gathers them back where it is calm (fluid_draw.c HD_CALM), so the letters condense out of
// the window's own dust. It ends on the word as it is formed unseen: calm water, whole lines.

export interface WordIntro {
  /** Wall seconds it plays in view (every frame drawn), then the word is as it always is. */
  seconds: number;
  /** The scene's seconds per wall second at `t` (default 1). */
  clock?: (t: number) => number;
  /** The water's height at (x, y) of the window (-1 .. 1, glass units) at `t`: 0 by `seconds`. */
  water?: (x: number, y: number, t: number) => number;
  /** The firmware's formation (dust to the scene, SCENE_DUST_S) is played unseen first, under
   *  the water at its `t` 0: its haze, which it lays only once formed (fluid_draw.c), is then
   *  there from the first frame shown and comes up with the window instead of on one frame. */
  formed?: boolean;
}

const clamp01 = (x: number) => (x <= 0 ? 0 : x >= 1 ? 1 : x);
const smooth = (x: number) => {
  const t = clamp01(x);
  return t * t * (3 - 2 * t);
};
/** Rough water: a few crossing swells, moving (cells' heights, Q10 units of fluid_sim.h). */
const ROUGH = 3.2;
const rough = (x: number, y: number, t: number) =>
  ROUGH * (Math.sin(23 * x + 5.1 * t) * Math.sin(29 * y - 3.7 * t) + 0.7 * Math.sin(31 * (x + 0.6 * y) - 6.3 * t));
/** A fixed random 0..1 per place, smooth between lattice points `n` apart (value noise). */
const hash = (i: number, j: number) => {
  const s = Math.sin(i * 127.1 + j * 311.7) * 43758.5453;
  return s - Math.floor(s);
};
const noise = (x: number, y: number, n: number) => {
  const u = x * n,
    v = y * n;
  const i = Math.floor(u),
    j = Math.floor(v);
  const fu = smooth(u - i),
    fv = smooth(v - j);
  const a = hash(i, j) + (hash(i + 1, j) - hash(i, j)) * fu;
  const b = hash(i, j + 1) + (hash(i + 1, j + 1) - hash(i, j + 1)) * fu;
  return a + (b - a) * fv;
};

export const WORD_INTROS: Record<"a" | "b" | "c", WordIntro> = {
  // still water; the device's own formation, dust to lines, played a third as fast
  a: { seconds: 2.1, clock: (t) => (t < 1.9 ? 0.32 : 1) },
  // the dust comes up, rough; the word forms under it as calm spreads from its middle to its ends
  b: {
    seconds: 2.4,
    formed: true,
    water: (x, y, t) => {
      const edge = 1.15 * smooth((t - 0.25) / 1.95);
      return rough(x, y, t) * smooth((Math.hypot(x, 2.5 * y) - edge) / 0.18 + 0.5);
    },
  },
  // the word forms under rough water that calms patch by patch, the last specks at random
  c: {
    seconds: 2.4,
    water: (x, y, t) => {
      const at = 0.3 + 1.6 * (0.55 * noise(x, y, 7) + 0.45 * hash(Math.round(x * 40), Math.round(y * 40)));
      return rough(x, y, t) * (1 - smooth((t - at) / 0.35));
    },
  },
};
