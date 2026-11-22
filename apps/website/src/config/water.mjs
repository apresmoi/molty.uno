// The page's water, the part that is the website's own: what focuses the page's dust, and how
// far a wave carries. The dust itself and the water are the firmware's (services/molty-screen
// field.ts, device/firmware/web/web_field.c). Plain .mjs: the tests read it under Node.

/** What focuses the page's dust: round a device (radii in its widths) and round a heading (in
 *  its width and its height), how much in focus (0..1) at each distance (0..1 of the radius)
 *  from the centre out. The page lays its denser dust by these, and the water's field moves
 *  the same dust.
 *  @type {Record<"device" | "word", { rx: number, ry: number, stops: Array<[number, number]> }>} */
export const FOCUS = {
  device: {
    rx: 1.3,
    ry: 1.3,
    stops: [
      [0.38, 1],
      [0.55, 0.45],
      [0.78, 0.12],
      [1, 0],
    ],
  },
  word: {
    rx: 0.64,
    ry: 1.4,
    stops: [
      [0, 0.7],
      [0.55, 0.3],
      [1, 0],
    ],
  },
};

/** The page's water against the device's (fluid_sim.c: a step keeps 0.985 of a wave's height
 *  and 0.975 of its motion, so a tap's ring is gone a screen's width away): what a step keeps
 *  here, so a ring crosses the page as one front with calm water behind it. The screens on the
 *  page take the same numbers (one water); a device's own are unchanged. */
export const CARRY = { leak: 1, damp: 0.998 };

/** A heading's window takes the page's water this many times over. The firmware's drawer loosens
 *  a scene's line where the water's slope would shift a dot a sub-dot and a half (fluid_draw.c
 *  HD_CALM, HD_FADE); a tap's front is that steep for about 450 px, and a little under it from
 *  there to the page's far edge. Taken this much steeper, a heading's lines loosen as a front
 *  from anywhere on the page passes, least from far and most under the finger, and the calm
 *  water behind the front still leaves them sharp. */
export const HEADING_GAIN = 2.5;
