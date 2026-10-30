// Mochi: (1) "squish" — hops at the glass and sticks to it like dough: splats flat, slowly
// sags and slides down the glass, peels off with a jiggle and bounces back. (2) "eat" — a
// strawberry drifts down, Mochi opens wide and gulps it whole, puffs up (a strawberry
// daifuku now: a little pinker) and chews, pleased.
import {
  WINDOW,
  backOut,
  blink,
  bump,
  exposureAnchor,
  lerp,
  smooth,
  step,
  zlerp,
  type Beat,
  type BeatCtx,
} from "./api";

const INK = "#0b0b10";
/** Gives a beat's first `n` marks (its props) a dark rim just behind each, so a prop reads on a
 *  light body and on the dark glass alike, and pins the exposure (a body zoomed up to fill the
 *  window would otherwise expose itself to black). */
function rimmed<B extends Beat>(beat: B, n: number): B {
  return {
    ...beat,
    start(b: BeatCtx) {
      beat.start?.(b);
      const ms = b.g.marks.filter((m) => m.color !== "#000000"); // its own anchor, if any
      b.g.marks = [
        ...ms.slice(0, n),
        ...ms.slice(0, n).map((m) => ({ ...m, color: INK })),
        ...ms.slice(n),
        exposureAnchor(),
      ];
    },
    frame(b: BeatCtx, t: number) {
      beat.frame(b, t);
      const ms = b.g.marks;
      for (let i = 0; i < n; i++) {
        const m = ms[i]!,
          o = ms[n + i]!;
        o.x = m.x;
        o.y = m.y;
        o.rot = m.rot;
        o.d = (m.d ?? 4) - 0.1;
        o.size = m.size > 0 ? m.size * 1.12 + 0.03 : 0;
      }
    },
  };
}

type Labeled = Beat & { label: string };
const every = (t: number, dt: number, p: number) => Math.floor(t / p) !== Math.floor((t - dt) / p);

const PRESS = 0.16;

const squish: Labeled = {
  name: "squish",
  label: "Sticky squish",
  len: 4.0,
  frame(b, t) {
    const { g, f } = b,
      HIT = 0.72,
      OFF = 2.55,
      L = 4.0,
      Z = 1.5;
    const out = smooth((t - OFF) / (L - OFF - 0.1));
    g.face = step(t, 0, 0.6) * (1 - out);
    g.pin = g.face;
    let sx = 1,
      sy = 1;
    if (t < 0.35) {
      g.zoom = lerp(1, 0.9, step(t, 0, 0.35)); // crouch back
      f.squash = -0.16 * step(t, 0, 0.35);
    } else if (t < HIT) {
      const x = (t - 0.35) / (HIT - 0.35);
      g.zoom = lerp(0.9, Z, x * x); // the jump at the glass
      g.oy = 0.12 * Math.sin(Math.PI * x);
      f.squash = 0.12 * Math.sin(Math.PI * x);
    } else if (t < OFF) {
      // stuck: splat, then it sags and slides down the glass like warm dough
      const h = t - HIT,
        k = Math.exp(-h * 6) * Math.cos(h * 20),
        sag = step(t, 1.15, 2.45);
      g.zoom = Z * (1 + 0.04 * k);
      g.press = PRESS * (1 + 0.6 * k);
      sx = 1.34 + 0.08 * k - 0.16 * sag;
      sy = 0.72 - 0.06 * k + 0.26 * sag;
      g.oy = -0.55 * sag;
    } else {
      // peels off with a pop and jiggles back into a ball
      const x = Math.min(1, (t - OFF) / (L - OFF)),
        h = t - OFF,
        j = Math.exp(-h * 4.5) * Math.cos(h * 16);
      g.press = PRESS * (1 - smooth(x / 0.15));
      g.zoom = zlerp(Z, 1, backOut(x));
      g.oy = -0.55 * (1 - smooth(x / 0.5));
      sx = 1 + 0.2 * j;
      sy = 1 - 0.18 * j;
    }
    g.sx = sx;
    g.sy = sy;
    f.still = t > HIT && t < OFF ? 0.8 : 0;
    f.gaze = 1;
    // squeezed shut at the splat, eyes down as it slides, then a sheepish look at you
    f.ly = -0.6 * step(t, 1.3, 1.5) * (1 - step(t, 2.2, 2.35));
    f.lid = t > HIT && t < HIT + 0.3 ? 1 : blink(t, 2.4);
    f.mouth = 0;
    // the slide leaves a smear: ripples along its lower edge
    if (t > 1.2 && t < 2.45 && every(t, b.dt, 0.12)) {
      const s = b.where("face") ?? [0, -0.2];
      b.fx.ripple(s[0] + (b.rand(Math.floor(t * 9)) - 0.5) * 0.6, s[1] - 0.55, 0.22, 0.08);
    }
    if (b.crossed(HIT)) b.fx.rim(0.85, 6);
    if (b.crossed(OFF)) b.fx.rim(0.4);
    if (b.crossed(OFF + 0.35)) b.fx.ripple(0, -0.95, 0.5, 0.15); // lands
    if (b.crossed(OFF + 0.2)) b.mood("happy");
    if (b.crossed(L - 0.4)) b.mood("idle");
  },
};

// the strawberry: a rounded heart-ish body (point down) and a leaf crown
const BERRY: Array<[number, number]> = [
  [0, -1],
  [0.55, -0.45],
  [0.75, 0.15],
  [0.6, 0.6],
  [0.2, 0.75],
  [-0.2, 0.75],
  [-0.6, 0.6],
  [-0.75, 0.15],
  [-0.55, -0.45],
];
const LEAF: Array<[number, number]> = [
  [0, -0.2],
  [0.9, 0.25],
  [0.3, 0.35],
  [0, 0.9],
  [-0.3, 0.35],
  [-0.9, 0.25],
];

const eat: Labeled = {
  name: "eat",
  label: "Strawberry gulp",
  len: 4.0,
  start(b) {
    b.g.marks = [
      { kind: "poly", x: 0, y: 2, size: 0, rot: 0, color: "#ff8a9e", pts: BERRY },
      { kind: "poly", x: 0, y: 2, size: 0, rot: 0, color: "#b8f08c", pts: LEAF },
    ];
    b.mood("idle");
  },
  frame(b, t) {
    const { g, f } = b,
      L = 4.0,
      GULP = 1.75;
    const out = step(t, 3.3, L - 0.1);
    g.face = 0.7 * step(t, 0, 0.5) * (1 - out);
    g.zoom = 1 + 0.15 * step(t, 0, 0.6) * (1 - out);
    const m = b.where("mouth") ?? [0, -0.15];
    // the berry drifts down, swaying, to just in front of the mouth, then is sucked in
    const fall = step(t, 0.1, 1.4),
      inn = step(t, GULP - 0.12, GULP + 0.06);
    const bx = lerp(m[0] + 0.1 * Math.sin(t * 5) * (1 - fall), m[0], inn),
      by = lerp(1.15, m[1] + 0.12, fall) * (1 - inn) + m[1] * inn;
    const size = 0.17 * step(t, 0.05, 0.3) * (1 - inn);
    const [berry, leaf] = [g.marks[0]!, g.marks[1]!];
    berry.x = bx * WINDOW;
    berry.y = by * WINDOW;
    berry.size = size;
    berry.rot = 0.25 * Math.sin(t * 4) * (1 - fall);
    leaf.x = berry.x;
    leaf.y = berry.y + size * 0.8;
    leaf.size = size * 0.6;
    leaf.rot = berry.rot;
    // eyes on it as it falls; mouth wide just before, a gulp, then chewing
    f.gaze = 1;
    f.ly = lerp(0.6, -0.4, fall) * (1 - step(t, GULP, GULP + 0.2));
    f.mouth =
      t > 1.25 && t < GULP
        ? step(t, 1.25, 1.5)
        : t >= GULP && t < 3.0
          ? 0.25 * Math.max(0, Math.sin((t - GULP) * 16))
          : 0;
    f.lid = t > GULP && t < GULP + 0.35 ? 1 : blink(t, 3.2);
    f.pitch = 0.18 * bump(t, 1.4, GULP + 0.15);
    // swallowed: it puffs up round, then the chews jiggle it
    f.squash =
      0.14 * bump(t, GULP, GULP + 0.4) -
      0.05 * Math.max(0, Math.sin((t - GULP) * 16)) * step(t, GULP + 0.3, GULP + 0.4) * (1 - step(t, 2.9, 3.0));
    g.sx = 1 + 0.1 * bump(t, GULP, GULP + 0.4);
    g.tint = "#ff8fa6";
    g.tintK = 0.22 * step(t, GULP, GULP + 0.3) * (1 - step(t, 3.0, 3.6));
    if (b.crossed(GULP)) {
      b.fx.ripple(m[0], m[1] - 0.05, 0.35, 0.1);
      for (let i = 0; i < 4; i++) {
        // a puff of sugar dust
        const a = Math.PI * (0.15 + 0.7 * (i / 3));
        b.fx.emit({
          x: m[0],
          y: m[1],
          vx: 1.0 * Math.cos(a),
          vy: 0.8 * Math.sin(a),
          g: 3,
          life: 0.45,
          strength: 0.12,
          radius: 0.04,
        });
      }
    }
    if (b.crossed(3.0)) b.mood("happy");
    if (b.crossed(3.7)) b.mood("idle");
  },
};

export const beats: Beat[] = [rimmed(squish, 0), rimmed(eat, 2)];
