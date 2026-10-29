// Kitten: (1) "laser" — a light dot darts about the glass; eyes and head snap after it; it
// stops, the kitten crouches and wiggles its bum, POUNCES and slaps both paws flat on the
// glass — and the dot slips out from under them and off the edge. It stares after it.
// (2) "eat" — a bowl of milk: head down, lap lap lap (drips in the fluid), up again with a
// milk moustache, licks it off, and a slow, contented cat blink.
import {
  WINDOW,
  backOut,
  blink,
  bump,
  exposureAnchor,
  hold,
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
const DOT: Array<[number, number]> = Array.from({ length: 8 }, (_, i) => {
  const a = (i / 8) * Math.PI * 2;
  return [Math.cos(a), Math.sin(a)] as [number, number];
});

const HOPS = 5,
  HOP = 0.32,
  POUNCE = 2.3,
  OFF = 3.2;

const laser: Labeled = {
  name: "laser",
  label: "Chases the dot",
  len: 4.3,
  start(b) {
    b.g.marks = [{ kind: "poly", x: 0, y: 0, size: 0, rot: 0, color: "#ffe2e2", pts: DOT }];
    b.mood("listening");
    b.state.side = b.rand(9) < 0.5 ? -1 : 1;
  },
  frame(b, t) {
    const { g, f, state: s } = b,
      L = 4.3,
      side = s.side!;
    // the dot: hops between random spots, then rests low in front, then escapes sideways
    const spot = (i: number): [number, number] =>
      i >= HOPS ? [0.12 * side, -0.5] : [(b.rand(i * 2 + 1) - 0.5) * 1.3, (b.rand(i * 2 + 2) - 0.5) * 1.1];
    let dx: number, dy: number;
    if (t < POUNCE + 0.05) {
      const i = Math.min(HOPS, Math.floor(t / HOP)),
        q = smooth((t - i * HOP) / 0.1),
        a = i ? spot(i - 1) : [-0.9 * side, 0.6],
        c = spot(i);
      dx = lerp(a[0]!, c[0], q);
      dy = lerp(a[1]!, c[1], q);
    } else {
      const q = step(t, POUNCE + 0.05, POUNCE + 0.45);
      dx = lerp(0.12 * side, 1.35 * side, q);
      dy = lerp(-0.5, -0.3, q);
    }
    const dot = g.marks[0]!;
    dot.x = dx * WINDOW;
    dot.y = dy * WINDOW;
    dot.size = 0.075 * step(t, 0, 0.1);
    // the kitten: tracking, crouch + bum wiggle, pounce onto the glass, stare after it
    const crouch = step(t, 1.75, 1.95) * (1 - step(t, POUNCE - 0.05, POUNCE));
    const on = step(t, POUNCE, POUNCE + 0.18) * (1 - smooth((t - OFF) / (L - OFF - 0.1)));
    g.face = Math.max(0.5 * step(t, 0, 0.4), on) * (1 - step(t, OFF, L - 0.1));
    g.pin = on;
    if (t < POUNCE) g.zoom = lerp(1, 0.95, crouch);
    else if (t < OFF)
      g.zoom = zlerp(0.95, 1.85, step(t, POUNCE, POUNCE + 0.18)) * (1 + 0.03 * bump(t, POUNCE + 0.18, POUNCE + 0.35));
    else g.zoom = zlerp(1.85, 1, backOut(Math.min(1, (t - OFF) / (L - OFF))));
    g.press = 0.12 * on;
    f.squash = -0.12 * crouch + 0.08 * bump(t, POUNCE - 0.05, POUNCE + 0.2);
    f.lean = 0.14 * Math.sin(t * 28) * crouch; // the bum wiggle
    f.gaze = 1;
    f.lx = Math.max(-1, Math.min(1, dx * 1.2));
    f.ly = Math.max(-1, Math.min(1, dy * 1.2));
    f.turn = 0.35 * dx * (1 - on);
    f.tilt = -0.25 * dx;
    f.tiltW = 0.6 * (1 - crouch) * (1 - on);
    f.lid = blink(t, 3.6);
    f.mouth = 0;
    if (b.crossed(POUNCE + 0.18)) b.fx.rim(0.75, 5);
    if (t > POUNCE + 0.1 && t < POUNCE + 0.5 && every(t, b.dt, 0.08)) b.fx.ripple(dx, dy, 0.18, 0.05); // the dot zips off
    if (b.crossed(POUNCE + 0.5)) b.mood("surprised");
    if (b.crossed(OFF + 0.2)) b.mood("idle");
  },
  post(b, t) {
    // both front paws up and flat on the glass, patting after the dot
    const up = step(t, POUNCE - 0.02, POUNCE + 0.15) * (1 - step(t, OFF, OFF + 0.3));
    if (up <= 0) return;
    for (const [i, r] of b.socket("foot").entries()) {
      const pat = 0.04 * Math.max(0, Math.sin(t * 22 + i * Math.PI)) * hold(t, POUNCE + 0.25, OFF - 0.2, 0.1);
      r.position.y += (0.55 + pat) * up;
      r.position.z += 0.24 * up;
      r.rotation.x -= 1.35 * up;
    }
  },
};

// ---------------------------------------------------------------- eat: milk

// outlines counter-clockwise (a clockwise fan faces away from the light and draws black)
const BOWL: Array<[number, number]> = [
  [-0.72, -0.4],
  [0.72, -0.4],
  [1, 0.3],
  [-1, 0.3],
];
const MILK: Array<[number, number]> = [
  [-0.9, -0.06],
  [0.9, -0.06],
  [0.95, 0.06],
  [-0.95, 0.06],
];
const LAP0 = 0.7,
  LAP1 = 2.3;

const eat: Labeled = {
  name: "eat",
  label: "Laps up milk",
  len: 4.3,
  start(b) {
    b.g.marks = [
      { kind: "poly", x: 0, y: -2, size: 0.3, rot: 0, color: "#7d8fbf", pts: BOWL },
      { kind: "poly", x: 0, y: -2, size: 0.3, rot: 0, color: "#ffffff", pts: MILK },
      { kind: "blot", x: 0, y: 0, size: 0, rot: 0.3, color: "#ffffff" },
      exposureAnchor(),
    ];
    b.mood("idle");
  },
  frame(b, t) {
    const { g, f } = b,
      L = 4.3;
    const bowlIn = step(t, 0, 0.45) * (1 - step(t, 3.6, L - 0.1));
    g.face = 0.5 * step(t, 0, 0.4) * (1 - step(t, 3.6, L - 0.1));
    g.oy = 0.12 * bowlIn;
    const by = lerp(-1.4, -0.66, bowlIn);
    g.marks[0]!.y = by * WINDOW;
    g.marks[1]!.y = (by + 0.1) * WINDOW;
    // head down into the bowl, lapping
    const down = step(t, LAP0 - 0.3, LAP0) * (1 - step(t, LAP1, LAP1 + 0.3));
    const lap = Math.max(0, Math.sin((t - LAP0) * 24)) * hold(t, LAP0, LAP1, 0.1);
    f.nod = 1.3 * down + 0.3 * lap;
    f.pitch = 0.22 * down;
    f.still = 0.6 * down;
    f.mouth = 0.35 * lap;
    f.lid = down > 0.5 ? 0.55 : Math.max(blink(t, 2.75), 0.9 * hold(t, 3.3, 3.75, 0.12)); // the slow blink
    f.gaze = 1;
    f.ly = -0.6 * down;
    if (t > LAP0 && t < LAP1 && every(t, b.dt, 0.26)) {
      const r = b.rand(Math.floor(t * 10));
      b.fx.ripple((r - 0.5) * 0.3, by + 0.13, 0.22, 0.06);
    }
    if (b.crossed(1.3) || b.crossed(1.9)) {
      const r = b.rand(Math.floor(t * 10)) - 0.5;
      b.fx.emit({ x: r * 0.2, y: by + 0.2, vx: r * 0.8, vy: 0.9, g: 5, life: 0.4, strength: 0.14, radius: 0.04 });
    }
    // up again: a milk moustache under the nose, licked away in two licks
    const m = b.where("mouth") ?? [0, -0.1];
    const tache = g.marks[2]!;
    tache.x = m[0] * WINDOW;
    tache.y = (m[1] + 0.04) * WINDOW;
    tache.size =
      0.13 * step(t, LAP1 + 0.25, LAP1 + 0.35) * (1 - 0.45 * step(t, 2.85, 2.95) - 0.55 * step(t, 3.15, 3.25));
    tache.rot = 0.3 + 0.2 * Math.sin(t * 3);
    if (t > 2.75) f.mouth = 0.5 * (bump(t, 2.75, 2.97) + bump(t, 3.05, 3.27));
    if (b.crossed(LAP1 + 0.3)) b.fx.ripple(0, by + 0.13, 0.3, 0.1);
    if (b.crossed(3.3)) b.mood("happy");
    if (b.crossed(3.9)) b.mood("idle");
  },
};

export const beats: Beat[] = [rimmed(laser, 1), rimmed(eat, 3)];
