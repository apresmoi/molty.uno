// Penguin: (1) "slide" — turns, flops onto its belly and toboggans out of the window, a wake
// of spray behind it; then waddles back in from the other side, flippers out for balance,
// and flaps, pleased. (2) "eat" — a fish is tossed in; the penguin tips its head back,
// catches it in its beak and swallows it whole, head first (gulp, gulp), then flaps.
import {
  WINDOW,
  backOut,
  blink,
  bump,
  easeIn,
  easeOut,
  exposureAnchor,
  lerp,
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

const FLOP = 0.45,
  DOWN = 0.8,
  GONE = 1.85,
  IN = 2.25,
  HOME = 3.75,
  TIP = 1.3;

const slide: Labeled = {
  name: "slide",
  label: "Belly slide",
  len: 4.4,
  frame(b, t) {
    const { g, f } = b;
    let belly = 0;
    if (t < GONE) {
      g.yaw = 1.25 * step(t, 0, 0.35);
      belly = step(t, FLOP, DOWN);
      const hop = bump(t, FLOP - 0.05, DOWN);
      const th = TIP * belly;
      f.pitch = th;
      f.bob = 0.3 * Math.sin(th) + 0.12 * hop;
      f.squash = -0.1 * bump(t, 0.15, FLOP) + 0.06 * hop;
      // tobogganing off to the right, faster and faster
      const q = Math.max(0, (t - DOWN) / (GONE - DOWN));
      g.ox = -0.25 * Math.sin(th) + 2.1 * easeIn(q) * 0.9 + 0.25 * q;
      g.zoom = 0.9;
    } else if (t < IN) {
      g.ox = 9;
      g.zoom = 0.9;
    } else {
      // waddles back from the right, facing left, rocking side to side
      const q = Math.min(1, (t - IN) / (HOME - IN)),
        w = Math.sin((t - IN) * Math.PI * 2 * 2.2);
      g.ox = 1.9 * (1 - easeOut(q));
      g.yaw = -1.2 * (1 - step(t, HOME - 0.3, HOME));
      f.lean = 0.2 * w * (1 - step(t, HOME - 0.3, HOME));
      f.bob = 0.03 * Math.abs(w);
      g.zoom = zlerp(0.9, 1, step(t, HOME - 0.5, HOME));
    }
    f.still = belly;
    // lying down, its slate back faces out: an icy sheen keeps it from sinking into the dark
    g.tint = "#e6eeff";
    g.tintK = 0.45 * step(t, FLOP, DOWN) * (1 - step(t, HOME - 0.4, HOME));
    f.gaze = 1;
    f.lx = t < GONE ? 0.7 : t < HOME ? -0.6 : 0;
    f.lid = blink(t, 3.95);
    f.mouth = 0;
    // the wake: spray off its belly as it slides
    if (t > DOWN && t < GONE && every(t, b.dt, 0.09)) {
      const p = b.where("belly") ?? [0, -0.5];
      if (Math.abs(p[0]) < 1.15) {
        const r = b.rand(Math.floor(t * 11));
        b.fx.emit({
          x: p[0] - 0.25,
          y: p[1] - 0.15,
          vx: -0.9 - r,
          vy: 0.9 + 0.6 * r,
          g: 6,
          life: 0.4,
          strength: 0.18,
          radius: 0.05,
        });
        b.fx.ripple(p[0] - 0.2, p[1] - 0.2, 0.2, 0.07);
      }
    }
    if (b.crossed(DOWN)) {
      b.fx.ripple(0, -0.8, 0.6, 0.15); // belly flop
      b.mood("happy");
    }
    if (b.crossed(IN)) b.mood("idle");
    if (b.crossed(HOME)) b.mood("happy"); // flaps
    if (b.crossed(4.2)) b.mood("idle");
  },
  post(b, t) {
    // flippers swept back along the body while sliding, out for balance while waddling
    const back = step(t, FLOP, DOWN) * (1 - step(t, GONE - 0.1, GONE)),
      out = step(t, IN, IN + 0.2) * (1 - step(t, HOME - 0.3, HOME));
    for (const r of b.kit.sock.side ?? []) {
      const g = b.node(r.g);
      g.rotation.x += 1.1 * back;
      g.rotation.z += r.side * 0.7 * out;
    }
  },
};

// ---------------------------------------------------------------- eat: the fish

const FISH: Array<[number, number]> = [
  [1, 0],
  [0.55, 0.35],
  [-0.2, 0.32],
  [-0.6, 0.1],
  [-0.6, -0.1],
  [-0.2, -0.32],
  [0.55, -0.35],
];
const TAIL: Array<[number, number]> = [
  [-0.55, 0],
  [-1.05, 0.38],
  [-1.05, -0.38],
];
const CATCH = 1.25,
  DOWN_AT = 1.55,
  SWALLOWED = 2.35;

const eat: Labeled = {
  name: "eat",
  label: "Swallows a fish",
  len: 4.0,
  start(b) {
    b.g.marks = [
      { kind: "poly", x: 0, y: 2, size: 0, rot: 0, color: "#d6e6ff", pts: FISH },
      { kind: "poly", x: 0, y: 2, size: 0, rot: 0, color: "#d6e6ff", pts: TAIL },
    ];
    b.state.side = b.rand(5) < 0.5 ? -1 : 1;
    b.mood("idle");
  },
  frame(b, t) {
    const { g, f, state: s } = b,
      L = 4.0,
      side = s.side!;
    const out = step(t, 3.35, L - 0.1);
    g.face = 0.7 * step(t, 0, 0.4) * (1 - out);
    g.zoom = t < 3.35 ? zlerp(1, 1.15, step(t, 0, 0.5)) : zlerp(1.15, 1, backOut(Math.min(1, (t - 3.35) / (L - 3.45))));
    g.oy = -0.1 * step(t, 0, 0.5) * (1 - out);
    // head back to catch, beak up while it swallows
    const up = step(t, 0.6, CATCH) * (1 - step(t, SWALLOWED, SWALLOWED + 0.3));
    f.nod = -0.9 * up;
    f.pitch = -0.12 * up;
    const k = b.where("beak") ?? [0, 0.1];
    // the fish: tossed in an arc from one side, caught crosswise, flipped head-down and gulped
    const fish = g.marks[0]!,
      tail = g.marks[1]!;
    let fx: number, fy: number, rot: number, sz: number;
    if (t < CATCH) {
      const q = Math.max(0, t / CATCH);
      fx = lerp(-1.25 * side, k[0], q);
      fy = lerp(0.2, k[1] + 0.12, q) + 0.75 * Math.sin(Math.PI * q);
      rot = side < 0 ? -2.5 * q : Math.PI + 2.5 * q; // tumbling through the air
      sz = 0.15;
    } else {
      const flip = step(t, CATCH, DOWN_AT),
        sw = easeIn(Math.min(1, Math.max(0, (t - DOWN_AT) / (SWALLOWED - DOWN_AT))));
      // nose down into the beak, sinking in tail last
      rot = lerp(side < 0 ? -2.5 : Math.PI + 2.5, -Math.PI / 2 + (side < 0 ? -2 * Math.PI : 0), flip);
      fx = k[0];
      fy = k[1] + 0.12 + 0.14 * flip - 0.3 * sw;
      sz = 0.15 * (1 - sw);
      if (t > CATCH + 0.05) fx += 0.03 * Math.sin(t * 30) * (1 - flip); // flapping in its beak
    }
    fish.x = fx * WINDOW;
    fish.y = fy * WINDOW;
    fish.rot = rot;
    fish.size = sz * step(t, 0, 0.1);
    tail.x = fish.x;
    tail.y = fish.y;
    tail.rot = rot + 0.3 * Math.sin(t * 18);
    tail.size = fish.size;
    f.gaze = 1;
    f.lx = t < CATCH ? Math.max(-1, Math.min(1, fx - k[0])) : 0;
    f.ly = t < CATCH ? 0.6 : 0.2;
    f.mouth = t > CATCH - 0.15 && t < SWALLOWED ? (t < CATCH ? 0.7 : 0.35) : 0;
    // gulp, gulp: the neck works it down
    const gulps = bump(t, 1.75, 2.0) + bump(t, 2.1, 2.35) + 1.3 * bump(t, 2.4, 2.75);
    f.squash = 0.07 * gulps;
    f.lid = t > 2.4 && t < 2.75 ? 1 : blink(t, 0.4);
    if (b.crossed(CATCH)) {
      b.fx.ripple(k[0] - 0.15, k[1] + 0.1, 0.35, 0.08);
      b.fx.ripple(k[0] + 0.15, k[1] + 0.1, 0.35, 0.08);
    }
    if (b.crossed(CATCH) && t < CATCH + 0.1)
      for (let i = 0; i < 3; i++) {
        // drips off the fish
        const r = b.rand(20 + i) - 0.5;
        b.fx.emit({
          x: k[0] + r * 0.3,
          y: k[1] + 0.1,
          vx: r * 0.6,
          vy: 0.3,
          g: 5,
          life: 0.5,
          strength: 0.14,
          radius: 0.04,
        });
      }
    if (b.crossed(2.8)) b.mood("happy");
    if (b.crossed(3.6)) b.mood("idle");
  },
};

export const beats: Beat[] = [rimmed(slide, 0), rimmed(eat, 2)];
