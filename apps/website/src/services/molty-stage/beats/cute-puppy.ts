// Puppy: (1) "boop" — it trots up to the glass nose first, boops it (a wet nose print stays
// on the glass), sniffs, gives the glass two big licks (streaks on the glass), backs off and
// tilts its head at the mess, wagging. (2) "eat" — a bone is tossed in; it snaps it out of
// the air, gnaws it with big side-to-side head shakes (crumbs flying), crunches the last
// bit, and pants happily with its tongue out.
import { WINDOW, backOut, blink, bump, clamp01, hold, lerp, smooth, step, zlerp, type Beat } from "./api";

type Labeled = Beat & { label: string };
type Pt = Array<[number, number]>;
const ring = (n: number, sx = 1, sy = 1): Pt =>
  Array.from({ length: n }, (_, i) => {
    const a = (i / n) * Math.PI * 2;
    return [sx * Math.cos(a), sy * Math.sin(a)] as [number, number];
  });
const every = (t: number, dt: number, p: number) => Math.floor(t / p) !== Math.floor((t - dt) / p);

// ---------------------------------------------------------------- boop: nose on the glass

const SMUDGE = "#e9eef7"; // a breath-fogged nose print: light, so it never reads as a feature
const HIT = 0.95,
  LICK1 = 1.55,
  LICK2 = 2.05,
  OFF = 2.55;

const boop: Labeled = {
  name: "boop",
  label: "Boops the glass",
  len: 4.4,
  start(b) {
    b.g.marks = [
      { kind: "blot", x: 0, y: 0, size: 0, rot: 0.4, color: SMUDGE },
      { kind: "poly", x: 0, y: 0, size: 0, rot: 0, color: SMUDGE, pts: ring(8, 0.55, 1) },
      { kind: "poly", x: 0, y: 0, size: 0, rot: 0, color: SMUDGE, pts: ring(8, 0.55, 1) },
    ];
    b.mood("listening");
  },
  frame(b, t) {
    const { g, f, state: s } = b,
      L = 4.4,
      ZOOM = 2.0;
    const out = smooth((t - 3.3) / (L - 3.4));
    g.face = smooth(t / 0.7) * (1 - out);
    g.pin = smooth(t / 0.8) * (1 - out);
    g.focusAt = "nose";
    g.focus = step(t, 0.2, 0.9) * (1 - step(t, OFF, OFF + 0.4));
    // trots up (a bouncy approach), boop, two licks, backs off a little, then home
    let p = 0;
    if (t < HIT) {
      const x = t / HIT;
      g.zoom = lerp(1, ZOOM, x * x);
      f.bob = 0.05 * Math.abs(Math.sin(t * 14)) * (1 - x);
    } else if (t < OFF) {
      const h = t - HIT,
        k = Math.exp(-h * 9) * Math.cos(h * 22);
      p = 0.16 * (0.55 + 0.6 * k);
      g.zoom = ZOOM * (1 + 0.03 * k);
    } else if (t < 3.3) g.zoom = zlerp(ZOOM, 1.25, smooth((t - OFF) / 0.45));
    else g.zoom = zlerp(1.25, 1, backOut(out));
    g.press = p;
    // sniff sniff at the glass (quick little nods), then the licks: mouth open, head sweeps up
    const sniff = hold(t, HIT + 0.1, LICK1 - 0.1, 0.05);
    f.nod = 0.06 * Math.sin(t * 34) * sniff;
    const lick = (a: number) => bump(t, a, a + 0.38);
    const lk = lick(LICK1) + lick(LICK2);
    f.mouth = t >= HIT && t < OFF ? 0.75 * lk : -1;
    f.bob = f.bob + 0.06 * lk;
    f.still = (0.8 * p) / 0.16;
    // after: a head tilt at the smudge, eyes on it
    const tilt = step(t, OFF + 0.2, OFF + 0.45) * (1 - step(t, 3.5, 3.8));
    f.tilt = 0.32;
    f.tiltW = tilt;
    f.gaze = 1;
    f.lx = -0.25 * tilt;
    f.ly = 0.1 * tilt;
    f.lid = Math.max(blink(t, 0.5), blink(t, 3.05));
    // the marks stay where they were made (the glass does not move); they fade at the end
    if (b.crossed(HIT)) {
      const n = b.where("nose") ?? [0, -0.1];
      s.nx = n[0];
      s.ny = n[1];
      b.fx.rim(0.55);
      b.mood("idle");
    }
    const fade = 1 - step(t, 3.4, L - 0.15);
    const nose = g.marks[0]!;
    nose.x = (s.nx ?? 0) * WINDOW;
    nose.y = (s.ny ?? 0) * WINDOW;
    nose.size = 0.17 * step(t, HIT, HIT + 0.08) * fade;
    for (const [i, a] of [LICK1, LICK2].entries()) {
      const m = g.marks[1 + i]!,
        q = step(t, a + 0.05, a + 0.35);
      if (b.crossed(a)) {
        const mo = b.where("mouth") ?? [0, -0.25];
        s[`lx${i}`] = mo[0] + (i ? 0.07 : -0.07);
        s[`ly${i}`] = mo[1];
      }
      m.x = (s[`lx${i}`] ?? 0) * WINDOW;
      m.y = ((s[`ly${i}`] ?? 0) + 0.12 * q) * WINDOW; // the streak grows upward with the lick
      m.size = 0.16 * q * fade;
      m.rot = i ? -0.15 : 0.15;
      m.pts = ring(8, 0.4, 1 + 0.8 * q);
    }
    if (b.crossed(LICK1 + 0.2) || b.crossed(LICK2 + 0.2)) b.fx.rim(0.25);
    if (b.crossed(OFF + 0.25)) b.mood("happy"); // the wag (and the tongue) come with it
    if (b.crossed(3.9)) b.mood("idle");
  },
};

// ---------------------------------------------------------------- eat: a bone

const BONE = "#f3e7cf";
const SHAFT: Pt = [
  [-1, -0.22],
  [1, -0.22],
  [1, 0.22],
  [-1, 0.22],
];
const KNOB = ring(8);
const CATCH = 0.75,
  GNAW1 = 2.55,
  GULP = 2.85;
// the bone: a shaft and four knobs (marks are convex fans), in fit units round (cx, cy)
const boneMarks = () => [
  { kind: "poly" as const, x: 0, y: 0, size: 0, rot: 0, color: BONE, pts: SHAFT },
  ...[0, 1, 2, 3].map(() => ({ kind: "poly" as const, x: 0, y: 0, size: 0, rot: 0, color: BONE, pts: KNOB })),
];

const eat: Labeled = {
  name: "eat",
  label: "Gnaws a bone",
  len: 4.3,
  start(b) {
    b.g.marks = boneMarks();
    b.mood("listening");
  },
  frame(b, t) {
    const { g, f } = b,
      L = 4.3;
    const out = step(t, 3.55, L - 0.1);
    g.face = step(t, 0, 0.4) * (1 - out);
    g.pin = step(t, 0.2, 0.6) * (1 - out);
    g.zoom =
      t < 3.55 ? zlerp(1, 1.35, step(t, 0.3, 0.8)) : zlerp(1.35, 1, backOut(Math.min(1, (t - 3.55) / (L - 3.6))));
    // where the bone is: tossed in on an arc from the upper right, then held in the mouth
    const m = b.where("mouth") ?? [0, -0.2];
    let cx: number, cy: number, rot: number;
    if (t < CATCH) {
      const q = clamp01((t - 0.05) / (CATCH - 0.05));
      cx = lerp(1.25, m[0], q);
      cy = lerp(0.5, m[1] - 0.12, q) + 0.55 * Math.sin(Math.PI * q);
      rot = 5 * (1 - q);
    } else {
      cx = m[0];
      cy = m[1] - 0.12; // below the mouth line, so the eyes stay clear
      rot = 0;
    }
    // gnawing: big side-to-side head shakes, the bone shaking with it; crumbs fly off
    const gnaw = hold(t, CATCH + 0.15, GNAW1, 0.15);
    const shake = Math.sin((t - CATCH) * 11);
    f.turn = 0.38 * shake * gnaw;
    f.tilt = 0.2 * shake;
    f.tiltW = gnaw;
    f.mouth =
      t < CATCH - 0.25 ? -1 : t < CATCH ? 0.7 : t < GULP ? 0.12 + 0.18 * Math.max(0, Math.sin(t * 16)) * gnaw : -1;
    f.gaze = 1;
    f.lx = t < CATCH ? clamp01(cx) : 0;
    f.ly = t < CATCH ? 0.6 : -0.2;
    f.squash = -0.08 * bump(t, GULP, GULP + 0.25); // gulp
    f.lid = Math.max(blink(t, 3.25), gnaw * 0.35);
    rot += 0.45 * shake * gnaw;
    // it gets chewed down: shorter after every few shakes, and gone at the gulp
    const left = (1 - 0.18 * step(t, 1.4, 1.5) - 0.2 * step(t, 2.1, 2.2)) * (1 - step(t, GULP - 0.05, GULP + 0.05));
    const S = 0.13,
      half = 0.13 * left,
      c = Math.cos(rot),
      sn = Math.sin(rot);
    const shaft = g.marks[0]!;
    shaft.x = cx * WINDOW;
    shaft.y = cy * WINDOW;
    shaft.size = S * (left > 0.02 ? 1 : 0) * step(t, 0, 0.05);
    shaft.rot = rot;
    shaft.pts = SHAFT.map(([x, y]) => [x * (half / S), y]) as Pt;
    for (let i = 0; i < 4; i++) {
      const k = g.marks[1 + i]!,
        ex = (i < 2 ? -1 : 1) * half,
        ey = (i % 2 ? 1 : -1) * 0.035;
      k.x = cx * WINDOW + ex * c - ey * sn;
      k.y = cy * WINDOW + ex * sn + ey * c;
      k.size = 0.05 * (left > 0.02 ? 1 : 0) * step(t, 0, 0.05);
    }
    if (b.crossed(CATCH)) {
      b.mood("surprised");
      b.fx.rim(0.6);
    }
    if (b.crossed(CATCH + 0.3)) b.mood("idle");
    if (gnaw > 0.5 && every(t, b.dt, 0.3)) {
      const r = b.rand(Math.floor(t * 10)) - 0.5,
        side = shake > 0 ? 1 : -1;
      b.fx.emit({
        x: m[0] + side * 0.2,
        y: m[1] - 0.05,
        vx: side * (0.9 + r),
        vy: 0.6,
        g: 4,
        life: 0.45,
        strength: 0.14,
        radius: 0.04,
      });
    }
    if (b.crossed(GULP + 0.2)) b.fx.ripple(0, -0.95, 0.35, 0.12);
    if (b.crossed(GULP + 0.3)) b.mood("happy"); // tongue out, panting, wagging
    if (b.crossed(3.85)) b.mood("idle");
  },
};

export const beats: Beat[] = [boop, eat];
