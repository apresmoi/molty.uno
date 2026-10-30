// Hamster: (1) "wheel" — turns side-on and runs flat out on an invisible wheel (feet a blur,
// dust streaming back through the fluid), faster and faster, until the wheel flings it out
// of the window backwards, tumbling; it totters back in, dizzy. (2) "eat" — seeds drop in
// one by one; it catches each and stuffs it in its cheeks, the pouches ballooning bigger
// every time, until its face is all cheeks; then it tucks them away.
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

const RUN0 = 0.35,
  FLING = 2.35,
  BACK = 3.0,
  WLEN = 4.4;
// the run's phase: speeding up from ~2 to ~5 strides a second (a pure function of t)
const runPhase = (t: number) => {
  const h = Math.max(0, Math.min(t, FLING) - RUN0);
  return Math.PI * 2 * (2 * h + 0.75 * h * h);
};

// the wheel: a ring of rungs behind the hamster, spinning under its feet
const RUNGS = 14,
  WHEEL_R = 0.8;
const RUNG: Array<[number, number]> = [
  [-1, -0.4],
  [1, -0.4],
  [1, 0.4],
  [-1, 0.4],
];
// the wheel's turn: driven by the run, coasting to a stop after the fling
const wheelTurn = (t: number) => {
  const run = runPhase(t) * 0.22,
    v = 0.22 * Math.PI * 2 * (2 + 1.5 * (FLING - RUN0)),
    h = Math.max(0, t - FLING);
  return -(run + (v * (1 - Math.exp(-h * 2.5))) / 2.5);
};

const wheel: Labeled = {
  name: "wheel",
  label: "Hamster wheel",
  len: WLEN,
  start(b) {
    b.g.marks = Array.from({ length: RUNGS }, () => ({
      kind: "poly" as const,
      x: 0,
      y: 0,
      size: 0,
      rot: 0,
      color: "#e6dccb",
      pts: RUNG,
      d: -2,
    }));
  },
  frame(b, t) {
    const show = step(t, 0, 0.35) * (1 - step(t, 3.7, 4.1)),
      turn = wheelTurn(t);
    for (let i = 0; i < RUNGS; i++) {
      const a = (i / RUNGS) * Math.PI * 2 + turn,
        r = b.g.marks[i]!;
      r.x = Math.cos(a) * WHEEL_R * WINDOW;
      r.y = (Math.sin(a) * WHEEL_R + 0.02) * WINDOW;
      r.rot = a + Math.PI / 2;
      r.size = 0.075 * show;
    }
    const { g, f } = b;
    const run = step(t, RUN0 - 0.15, RUN0 + 0.1) * (1 - step(t, FLING - 0.05, FLING));
    const ph = runPhase(t);
    if (t < FLING) {
      g.yaw = 0.8 * step(t, 0, 0.35);
      f.bob = 0.09 * Math.abs(Math.sin(ph)) * run;
      f.pitch = 0.22 * run;
      f.lean = 0.04 * Math.sin(ph) * run;
      g.zoom = 1 - 0.08 * run;
    } else if (t < BACK) {
      // flung out backwards (left), tumbling
      const q = (t - FLING) / (BACK - FLING);
      g.yaw = 0.8;
      g.ox = -2.0 * easeIn(Math.min(1, q * 1.3)) - 0.15 * Math.sin(Math.PI * q);
      g.oy = 0.35 * Math.sin(Math.PI * Math.min(1, q * 1.2));
      g.roll = 2.2 * easeOut(q);
      g.zoom = 0.92;
    } else {
      // totters back in from the left, dizzy
      const q = Math.min(1, (t - BACK) / (WLEN - 0.25 - BACK));
      g.ox = -1.9 * (1 - easeOut(q));
      g.yaw = 0.8 * (1 - step(q, 0.7, 1));
      g.roll = 0.12 * Math.sin((t - BACK) * 9) * (1 - q);
      f.bob = 0.04 * Math.abs(Math.sin((t - BACK) * 12)) * (1 - q);
      g.zoom = zlerp(0.92, 1, q);
    }
    f.gaze = 1;
    f.lx = 0.6 * run;
    f.lid = t > BACK ? Math.max(0.5 * (1 - step(t, 3.8, 4.1)), blink(t, 4.1)) : Math.max(0.35 * run, blink(t, 1.2)); // determined, then dizzy
    f.mouth = 0;
    // dust streaming back off the wheel
    if (run > 0.5 && every(t, b.dt, 0.11)) {
      const p = b.where("foot") ?? [0, -0.6];
      const v = 1.6 + 1.2 * step(t, RUN0, FLING);
      b.fx.emit({
        x: p[0] - 0.1,
        y: p[1] - 0.02,
        vx: -v,
        vy: 0.15 * (b.rand(Math.floor(t * 9)) - 0.2),
        life: 0.35,
        strength: 0.18,
        radius: 0.05,
      });
    }
    if (b.crossed(FLING)) {
      b.mood("surprised");
      b.fx.rim(0.6);
    }
    if (b.crossed(BACK)) b.mood("sleepy"); // dizzy
    if (b.crossed(3.95)) b.mood("idle");
    if (b.crossed(WLEN - 0.25)) b.fx.ripple(0, -0.95, 0.3, 0.12);
  },
  post(b, t) {
    // the feet: a blur of strides while it runs, tucked while it tumbles
    const run = step(t, RUN0 - 0.15, RUN0 + 0.1) * (1 - step(t, FLING - 0.05, FLING));
    const ph = runPhase(t),
      tuck = bump(t, FLING, BACK);
    for (const [i, r] of b.socket("foot").entries()) {
      const s = Math.sin(ph + i * Math.PI);
      r.rotation.x += 0.9 * s * run - 0.8 * tuck;
      r.position.z += 0.07 * s * run;
      r.position.y += 0.04 * Math.max(0, Math.cos(ph + i * Math.PI)) * run + 0.05 * tuck;
    }
  },
};

// ---------------------------------------------------------------- eat: stuffing seeds

const DROPS = [0.3, 1.0, 1.7];
const POUCH_Y = -0.13,
  POUCH_Z = 0.25; // the pouches' rest centre (pack-a.js POUCH.c)
// a sunflower seed: a pointed oval (counter-clockwise; marks' glyphs are half-size, so a poly)
const SEED: Array<[number, number]> = [
  [0, -1],
  [0.45, -0.55],
  [0.55, 0.2],
  [0.3, 0.8],
  [0, 1],
  [-0.3, 0.8],
  [-0.55, 0.2],
  [-0.45, -0.55],
];
const FALL = 0.45,
  STUFF = 0.18;
const stuffed = (t: number) => {
  let k = 0;
  for (const d of DROPS) k += 0.42 * backOut(Math.max(0, Math.min(1, (t - d - FALL - STUFF) / 0.25)));
  return k * (1 - step(t, 3.55, 4.05));
};

const eat: Labeled = {
  name: "eat",
  label: "Stuffs its cheeks",
  len: 4.3,
  start(b) {
    b.g.marks = DROPS.map(() => ({ kind: "poly" as const, x: 0, y: 2, size: 0, rot: 0, color: "#f6e6c4", pts: SEED }));
    b.mood("idle");
  },
  frame(b, t) {
    const { g, f } = b,
      L = 4.3;
    const out = step(t, 3.45, L - 0.1);
    const close = step(t, 2.35, 2.7) * (1 - out);
    g.face = Math.max(0.55 * step(t, 0, 0.4), close) * (1 - out);
    g.pin = close;
    g.zoom = zlerp(1.08, 1.55, close);
    const paws = b.where("seed") ?? [0, -0.3],
      m = b.where("mouth") ?? [0, -0.2];
    let look = 0.4;
    for (const [i, d] of DROPS.entries()) {
      const s = g.marks[i]!;
      const q = (t - d) / FALL;
      if (q < 0 || t > d + FALL + STUFF) {
        s.size = 0;
        continue;
      }
      if (q < 1) {
        // falling into its paws
        s.x = paws[0] * WINDOW;
        s.y = lerp(1.15, paws[1] + 0.05, q * q) * WINDOW;
        s.rot = Math.PI + 0.4 * Math.sin(t * 9 + i);
        s.size = 0.1;
        look = lerp(0.7, -0.3, q);
      } else {
        // up to the mouth and in
        const k = (t - d - FALL) / STUFF;
        s.x = lerp(paws[0], m[0] + (i % 2 ? 0.12 : -0.12), k) * WINDOW;
        s.y = lerp(paws[1] + 0.05, m[1], k) * WINDOW;
        s.size = 0.1 * (1 - k);
        look = -0.3;
      }
    }
    f.gaze = 1 - close;
    f.ly = look;
    const st = stuffed(t);
    f.squash = 0.04 * st;
    f.mouth = 0;
    f.lid = blink(t, 3.2);
    f.turn = 0.18 * Math.sin(t * 7) * close; // cheeks jiggle as it wiggles its head
    for (const d of DROPS)
      if (b.crossed(d + FALL + STUFF)) {
        b.fx.ripple(m[0] - 0.35, m[1], 0.25, 0.08);
        b.fx.ripple(m[0] + 0.35, m[1], 0.25, 0.08);
      }
    if (b.crossed(2.45)) b.fx.rim(0.45, 5); // the cheeks fill the window
    if (b.crossed(2.4)) b.mood("happy");
    if (b.crossed(3.5)) b.mood("idle");
  },
  post(b, t) {
    const st = stuffed(t),
      grab = DROPS.reduce((a, d) => a + bump(t, d + FALL - 0.12, d + FALL + STUFF), 0);
    for (const m of b.part("pouch")) {
      // the kit re-sets the pouches' scale and x every frame, but not y / z: set those outright
      // (adding to them would drift a little further every frame)
      const side = m.position.x > 0 ? 1 : -1;
      m.scale.set(m.scale.x * (1 + 1.0 * st), m.scale.y * (1 + 0.6 * st), m.scale.z * (1 + 0.5 * st));
      m.position.x += side * 0.07 * st;
      m.position.y = POUCH_Y - 0.03 * st;
      m.position.z = POUCH_Z;
    }
    for (const m of [...b.part("paw"), ...b.part("seed")]) m.position.y += 0.07 * grab;
  },
};

export const beats: Beat[] = [rimmed(wheel, 0), rimmed(eat, DROPS.length)];
