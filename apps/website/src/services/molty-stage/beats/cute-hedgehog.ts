// Hedgehog: (1) "roll" — it tucks into a spiky ball, rolls across the window and bumps the
// edge, rolls all the way back to the other side and bumps again, rolls home, uncurls a bit
// dizzy. (2) "eat" — an apple rolls in and bumps it; it curls up in surprise and the apple
// ends up stuck on its spikes; it uncurls wearing it, shakes it down in front and munches it.
import { blink, bump, clamp01, easeOut, hold, lerp, step, type Beat, type BeatCtx } from "./api";

/** Uniform scale (the rig types expose only `set`). */
const sc = (o: { scale: { set(x: number, y: number, z: number): unknown } }, k: number) => o.scale.set(k, k, k);

/** Tuck: the head and its spiky cloak roll down over the face, the feet tuck in. */
const tuck = (b: BeatCtx, k: number) => {
  if (k <= 0) return;
  const hp = b.kit.headPivot;
  hp.rotation.x += k * 1.9;
  hp.position.y -= k * 0.03;
  for (const m of b.part("feet")) sc(m, 1 - 0.9 * k);
  for (const m of b.part("ears")) sc(m, 1 - 0.8 * k);
};

// the path: [time, x] keys of the ball's centre (fit units); the roll follows the distance
const PATH: Array<[number, number]> = [
  [0.75, 0],
  [1.5, 0.42],
  [2.75, -0.42],
  [3.45, 0],
];
const R = 0.3; // the ball's on-screen radius (fit units) for the roll angle
const BALL = "#f0cfae"; // the tint that lifts the dark curled ball

const posAt = (t: number) => {
  if (t <= PATH[0]![0]) return 0;
  for (let i = 0; i + 1 < PATH.length; i++) {
    const [t0, x0] = PATH[i]!,
      [t1, x1] = PATH[i + 1]!;
    if (t <= t1) {
      const q = (t - t0) / (t1 - t0);
      return lerp(x0, x1, q * q * (3 - 2 * q));
    }
  }
  return 0;
};

const roll: Beat = {
  name: "roll",
  len: 4.5,
  frame(b, t) {
    const { g, f } = b;
    const curl = step(t, 0.2, 0.7) * (1 - step(t, 3.45, 3.85));
    b.state.curl = curl;
    g.focusAt = "cloak";
    g.focus = curl;
    g.pin = curl;
    const x = posAt(t);
    g.ox = x;
    g.oy = -0.12 * curl;
    g.roll = -x / R;
    g.face = 0.3 * (1 - curl) * hold(t, 0, 4.4, 0.3);
    f.still = curl;
    f.lid = curl > 0.3 ? 1 : blink(t, 4.2);
    // the dark cloak rolled outward would vanish: lighten the ball while curled
    g.tint = BALL;
    g.tintK = 0.55 * curl;
    // dizzy after: the head wobbles in circles
    const dz = hold(t, 3.8, 4.35, 0.15);
    f.tilt = 0.25 * Math.sin(t * 9);
    f.tiltW = dz;
    f.lx = 0.6 * Math.cos(t * 9);
    f.ly = 0.6 * Math.sin(t * 9);
    f.gaze = dz;
    // rolling: a trail on the floor; the bumps at the edge
    if (curl > 0.8 && Math.floor(t / 0.1) !== Math.floor((t - b.dt) / 0.1)) b.fx.ripple(x / 0.85, -0.85, 0.15, 0.06);
    if (b.crossed(1.5)) {
      b.fx.ripple(1.02, -0.3, 0.6, 0.14);
      b.fx.rim(0.3);
    }
    if (b.crossed(2.75)) {
      b.fx.ripple(-1.02, -0.3, 0.6, 0.14);
      b.fx.rim(0.3);
    }
    if (b.crossed(0.1)) b.mood("surprised");
    if (b.crossed(0.5)) b.mood("idle");
    if (b.crossed(4.1)) b.mood("happy");
    if (b.crossed(4.42)) b.mood("idle");
  },
  post(b, t) {
    tuck(b, step(t, 0.2, 0.7) * (1 - step(t, 3.45, 3.85)));
  },
};

// ---------------------------------------------------------------- eat: an apple
const APPLE = "#ff6b6b";
const STEM = "#b6eb8c";
// three bites' worth: the apple in three slices (eaten right to left)
const SLICES: Array<Array<[number, number]>> = [];
{
  const cut = [-0.5, -0.17, 0.17, 0.5];
  for (let i = 0; i < 3; i++) {
    const pts: Array<[number, number]> = [];
    const a0 = cut[i]!,
      a1 = cut[i + 1]!;
    for (let k = 0; k <= 4; k++) {
      const x = lerp(a0, a1, k / 4);
      pts.push([x, -Math.sqrt(Math.max(0, 0.25 - x * x))]);
    }
    for (let k = 4; k >= 0; k--) {
      const x = lerp(a0, a1, k / 4);
      pts.push([x, Math.sqrt(Math.max(0, 0.25 - x * x))]);
    }
    SLICES.push(pts);
  }
}
const LEAFP: Array<[number, number]> = [
  [0, 0],
  [0.25, 0.12],
  [0.4, 0.3],
  [0.15, 0.25],
];
const BITE = [2.95, 3.35, 3.75];

const eat: Beat = {
  name: "eat",
  len: 4.5,
  start(b) {
    b.g.marks = [
      ...SLICES.map((pts) => ({ kind: "poly" as const, x: 0, y: 0, size: 0, rot: 0, color: APPLE, pts })),
      { kind: "poly", x: 0, y: 0, size: 0, rot: 0, color: STEM, pts: LEAFP },
    ];
  },
  frame(b, t) {
    const { g, f } = b;
    const curl = step(t, 0.85, 1.1) * (1 - step(t, 1.6, 2.0));
    b.state.curl = curl;
    g.face = 0.35 * hold(t, 0, 4.4, 0.4);
    g.tint = BALL;
    g.tintK = 0.55 * curl;
    // the apple: rolls in from the right, bonks it (0.85), sticks on top of its spikes while
    // it is curled, rides there, is shaken down in front (2.5) and eaten
    const top = b.where("cloak") ?? [0, 0.2];
    let ax: number, ay: number, rot: number;
    if (t < 0.85) {
      const q = easeOut(t / 0.85);
      ax = lerp(1.1, 0.42, q);
      ay = -0.5;
      rot = 3 * (1 - q);
    } else if (t < 2.45) {
      const q = step(t, 0.85, 1.2);
      ax = lerp(0.42, top[0] * 0.85, q);
      ay = lerp(-0.5, top[1] * 0.85 + 0.32, q) + 0.15 * bump(t, 0.85, 1.2);
      rot = 0.15 * Math.sin(t * 6);
    } else {
      const q = clamp01((t - 2.45) / 0.35);
      ax = lerp(top[0] * 0.85, 0.05, q);
      ay = lerp(top[1] * 0.85 + 0.32, -0.48, q * q);
      rot = 0;
    }
    let left = 0;
    for (let i = 0; i < 3; i++) {
      const m = g.marks[i]!;
      m.x = ax;
      m.y = ay;
      m.rot = rot;
      m.size = t < BITE[2 - i]! ? 0.46 * step(t, 0, 0.1) : 0;
      if (m.size > 0) left++;
    }
    const lf = g.marks[3]!;
    lf.x = ax + 0.23 * Math.sin(rot);
    lf.y = ay + 0.2;
    lf.rot = rot;
    lf.size = left > 1 ? 0.42 : 0;
    // the hedgehog: bonked, curls, uncurls proud (eyes up at its hat), a shake, munching
    f.gaze = 1;
    f.ly = hold(t, 1.9, 2.4, 0.15) * 0.8 - 0.5 * hold(t, 2.7, 3.9, 0.15);
    f.lx = t < 0.85 ? 0.6 : 0;
    f.lid = curl > 0.3 ? 1 : blink(t, 4.15);
    f.turn = 0.4 * Math.sin((t - 2.25) * Math.PI * 2 * 4) * bump(t, 2.25, 2.55);
    let chomp = 0;
    for (const at of BITE) chomp = Math.max(chomp, bump(t, at - 0.22, at + 0.12));
    f.mouth = chomp > 0.05 ? 0.7 * chomp : 0;
    f.nod = 0.15 * chomp;
    if (b.crossed(0.85)) {
      b.mood("surprised");
      b.fx.ripple(0.45, -0.45, 0.4, 0.1);
    }
    if (b.crossed(1.2)) b.mood("idle");
    if (b.crossed(1.95)) b.mood("happy");
    if (b.crossed(2.4)) b.mood("idle");
    if (b.crossed(2.8)) b.fx.ripple(0.05, -0.85, 0.35, 0.12); // the apple lands
    for (const at of BITE)
      if (b.crossed(at))
        b.fx.emit({
          x: (ax + 0.1) / 0.85,
          y: ay / 0.85,
          vx: 0.4,
          vy: 0.4,
          g: 4,
          life: 0.4,
          strength: 0.16,
          radius: 0.04,
        });
    if (b.crossed(3.95)) b.mood("happy");
    if (b.crossed(4.4)) b.mood("idle");
  },
  post(b) {
    tuck(b, b.state.curl ?? 0);
  },
};

export const beats: Beat[] = [roll, eat];
