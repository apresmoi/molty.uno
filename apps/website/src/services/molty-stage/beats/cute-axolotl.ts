// Axolotl: (1) "bubbles" — swims up to the glass, gills fanning, puffs up and blows a string
// of bubbles that wobble up through the fluid and pop; then one big bubble swells at its
// mouth and pops in its face (a ring through the fluid) — startled, then delighted.
// (2) "eat" — a little red worm wriggles down; the axolotl watches, holds dead still, and
// SNAPS it up with a suck (the fluid pulled into its mouth); a gulp, a happy gill wiggle.
import { WINDOW, backOut, blink, bump, exposureAnchor, lerp, step, zlerp, type Beat, type BeatCtx } from "./api";

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
const DISC: Array<[number, number]> = Array.from({ length: 8 }, (_, i) => {
  const a = (i / 8) * Math.PI * 2;
  return [Math.cos(a), Math.sin(a)] as [number, number];
});
const BUBBLE = "#e4f3ff";

/** Gills: fan the ear sockets (each carries a side's three fronds) by `amp` at `rate` Hz. */
function fanGills(b: Parameters<NonNullable<Beat["post"]>>[0], t: number, amp: number, rate: number) {
  if (amp <= 0) return;
  for (const r of b.kit.sock.ear ?? []) {
    const g = b.node(r.g);
    g.rotation.z += r.side * amp * Math.sin(t * rate * Math.PI * 2 + (r.side > 0 ? 0 : 1.2));
  }
}

const SMALL = 5,
  B0 = 1.0,
  BGAP = 0.22,
  RISE = 0.9,
  BIG0 = 2.25,
  POP = 2.85;

const bubbles: Labeled = {
  name: "bubbles",
  label: "Blows bubbles",
  len: 4.2,
  start(b) {
    b.g.marks = Array.from({ length: SMALL + 1 }, () => ({
      kind: "poly" as const,
      x: 0,
      y: 2,
      size: 0,
      rot: 0,
      color: BUBBLE,
      pts: DISC,
    }));
    b.mood("idle");
  },
  frame(b, t) {
    const { g, f } = b,
      L = 4.2;
    const out = step(t, 3.4, L - 0.1);
    g.face = step(t, 0, 0.6) * (1 - out);
    g.pin = g.face;
    g.zoom = t < 3.4 ? zlerp(1, 1.4, step(t, 0, 0.7)) : zlerp(1.4, 1, backOut(Math.min(1, (t - 3.4) / (L - 3.5))));
    g.oy = -0.12 * step(t, 0, 0.7) * (1 - out); // down a little: room for the bubbles
    g.roll = 0.05 * Math.sin(t * 2.4) * (1 - out); // floating
    const m = b.where("mouth") ?? [0, -0.25];
    // puff up before blowing, and before the big one
    f.squash = 0.08 * bump(t, 0.55, B0 + 0.1) + 0.06 * bump(t, BIG0 - 0.3, POP);
    f.mouth = t > B0 && t < B0 + SMALL * BGAP ? 0.25 : t > BIG0 && t < POP ? 0.3 : 0;
    f.gaze = 1;
    f.ly = t < POP ? 0.5 * step(t, B0, B0 + 0.3) - 0.8 * step(t, BIG0, BIG0 + 0.2) : 0;
    f.lid = blink(t, 0.7) || blink(t, 3.6);
    for (let i = 0; i < SMALL; i++) {
      const s = g.marks[i]!,
        q = (t - B0 - i * BGAP) / RISE;
      if (q < 0 || q > 1) {
        s.size = 0;
        continue;
      }
      s.x = (m[0] + 0.07 * Math.sin(q * 10 + i * 2) + 0.05 * (i % 2 ? 1 : -1) * q) * WINDOW;
      s.y = (m[1] + 0.08 + 1.05 * q) * WINDOW;
      s.size = 0.035 + 0.03 * q;
    }
    // the big one swells at its mouth, wobbling, and pops
    const big = g.marks[SMALL]!;
    const grow = step(t, BIG0, POP);
    big.x = m[0] * WINDOW;
    big.y = (m[1] - 0.02 + 0.22 * grow) * WINDOW;
    big.size = t < POP ? 0.24 * grow * (1 + 0.06 * Math.sin(t * 20)) : 0;
    big.rot = t;
    for (let i = 0; i < SMALL; i++) {
      const at = B0 + i * BGAP;
      if (b.crossed(at))
        b.fx.emit({
          x: m[0],
          y: m[1] + 0.1,
          vx: 0.05 * (i % 2 ? 1 : -1),
          vy: 1.05 / RISE,
          life: RISE,
          strength: 0.1,
          radius: 0.045,
        });
      if (b.crossed(at + RISE)) b.fx.ripple(m[0], m[1] + 1.13, 0.3, 0.07); // pop
    }
    if (b.crossed(POP)) {
      for (let j = 0; j < 6; j++) {
        const a = (j / 6) * Math.PI * 2;
        b.fx.ripple(m[0] + 0.32 * Math.cos(a), m[1] + 0.2 + 0.32 * Math.sin(a), 0.4, 0.08);
      }
      b.mood("surprised");
    }
    if (b.crossed(POP + 0.5)) b.mood("happy");
    if (b.crossed(3.9)) b.mood("idle");
  },
  post(b, t) {
    fanGills(b, t, 0.35 * step(t, 0.3, 0.7) * (1 - step(t, 3.3, 3.7)), 3);
  },
};

// ---------------------------------------------------------------- eat: the worm

const SEG = 4,
  SNAP = 1.85;

const eat: Labeled = {
  name: "eat",
  label: "Snaps a worm",
  len: 4.0,
  start(b) {
    b.g.marks = Array.from({ length: SEG }, () => ({
      kind: "poly" as const,
      x: 0,
      y: 2,
      size: 0,
      rot: 0,
      color: "#ff8e86",
      pts: DISC,
    }));
    b.state.side = b.rand(3) < 0.5 ? -1 : 1;
    b.mood("idle");
  },
  frame(b, t) {
    const { g, f, state: s } = b,
      L = 4.0,
      side = s.side!;
    const out = step(t, 3.3, L - 0.1);
    g.face = step(t, 0, 0.5) * (1 - out);
    g.pin = 0.8 * g.face;
    const lunge = step(t, SNAP - 0.05, SNAP + 0.06) * (1 - step(t, SNAP + 0.25, SNAP + 0.7));
    if (t < 3.3) g.zoom = zlerp(1, 1.25, step(t, 0.9, 1.6)) * zlerp(1, 1.3, lunge);
    else g.zoom = zlerp(1.25, 1, backOut(Math.min(1, (t - 3.3) / (L - 3.4))));
    g.ox = 0.1 * side * lunge;
    const m = b.where("mouth") ?? [0, -0.25];
    // the worm: wriggles down to hang above and beside the mouth; sucked in at the snap
    const sink = step(t, 0, 1.3),
      suck = step(t, SNAP - 0.02, SNAP + 0.1);
    const wx = lerp(0.6 * side, m[0] + 0.55 * side, sink),
      wy = lerp(1.2, m[1] - 0.08, sink); // beside the mouth, clear of the eye
    for (let j = 0; j < SEG; j++) {
      const sg = g.marks[j]!;
      const px = wx + (j - 1.5) * 0.065 * Math.cos(0.6 * Math.sin(t * 7)) + 0.03 * Math.sin(t * 9 + j * 1.4),
        py = wy + 0.05 * Math.sin(t * 8 + j * 1.3) + (j - 1.5) * 0.03 * Math.sin(t * 3);
      sg.x = lerp(px, m[0], suck) * WINDOW;
      sg.y = lerp(py, m[1], suck) * WINDOW;
      sg.size = 0.042 * step(t, 0, 0.15) * (1 - suck);
    }
    // watch it, freeze, snap, gulp
    const freeze = step(t, 1.3, 1.45) * (1 - step(t, SNAP, SNAP + 0.05));
    f.gaze = 1;
    f.lx = t < SNAP ? Math.max(-1, Math.min(1, (wx - m[0]) * 2)) : 0;
    f.ly = t < SNAP ? Math.max(-1, Math.min(1, (wy - m[1]) * 1.5)) : 0;
    f.turn = 0.2 * side * step(t, 0.6, 1.3) * (1 - step(t, SNAP, SNAP + 0.2));
    f.still = Math.max(freeze, 0.5);
    f.mouth = t > SNAP - 0.05 && t < SNAP + 0.15 ? 0.9 : 0;
    const gulp = bump(t, SNAP + 0.2, SNAP + 0.65);
    f.squash = 0.08 * gulp;
    f.lid = gulp > 0.3 ? 1 : Math.max(0.25 * freeze, blink(t, 3.2));
    if (b.crossed(SNAP)) {
      // the suck: the fluid round the worm pulled into the mouth
      b.fx.push(wx, wy, (m[0] - wx) * 1.2, (m[1] - wy) * 1.2, 0.18);
      b.fx.ripple(m[0], m[1] - 0.05, 0.4, 0.09);
    }
    if (b.crossed(2.6)) b.mood("happy");
    if (b.crossed(3.5)) b.mood("idle");
  },
  post(b, t) {
    fanGills(b, t, 0.3 * bump(t, SNAP + 0.5, 3.3), 4);
  },
};

export const beats: Beat[] = [rimmed(bubbles, SMALL + 1), rimmed(eat, SEG)];
