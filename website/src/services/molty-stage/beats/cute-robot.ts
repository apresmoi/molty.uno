// Robot: (1) "reboot" — a glitch: jerky jumps, colour flicker, scan lines rolling down the
// window; it crashes (head droops, screen-face goes blank), three loading dots cycle on its
// face, then DING: eyes back on, antenna flash, a happy hop. (2) "eat" — a battery floats in;
// it opens its screen-mouth and swallows it; ZAP, sparks and a buzz, dials whirring; it
// charges up and hops, full.
import { backOut, blink, bump, hold, step, WINDOW, zlerp, type Beat, type BeatCtx } from "./api";

const LIGHT = "#dffcff";
const DOT: Array<[number, number]> = Array.from({ length: 8 }, (_, i) => [
  Math.cos((i / 8) * Math.PI * 2),
  Math.sin((i / 8) * Math.PI * 2),
]);
const BAR: Array<[number, number]> = [
  [-1, -0.035],
  [1, -0.035],
  [1, 0.035],
  [-1, 0.035],
];
const tick = (b: BeatCtx, t: number, every: number) => Math.floor(t / every) !== Math.floor((t - b.dt) / every);

const CRASH = 1.35,
  DING = 2.95;

const reboot: Beat = {
  name: "reboot",
  len: 4.4,
  start(b) {
    b.g.marks = [
      { kind: "poly", x: 0, y: 0, size: 0, rot: 0, color: LIGHT, pts: BAR },
      ...[0, 1, 2].map(() => ({
        kind: "poly" as const,
        x: 0,
        y: 0,
        size: 0,
        rot: 0,
        color: LIGHT,
        pts: DOT,
      })),
      { kind: "star", x: 0, y: 0, size: 0, rot: 0, color: "#ffffff" },
    ];
  },
  frame(b, t) {
    const { g, f } = b,
      L = 4.4,
      out = step(t, 3.6, L - 0.1);
    g.face = step(t, 0, 0.4) * (1 - out);
    g.pin = g.face;
    g.zoom = t < 3.6 ? zlerp(1, 1.35, step(t, 0, 0.4)) : zlerp(1.35, 1, backOut(Math.min(1, (t - 3.6) / (L - 3.7))));
    // the glitch: held random offsets that jump every 0.09 s
    const glitch = t > 0.3 && t < CRASH;
    if (glitch) {
      const q = Math.floor(t / 0.09);
      g.ox = (b.rand(q) - 0.5) * 0.16;
      g.oy = (b.rand(q + 100) - 0.5) * 0.06;
      g.roll = (b.rand(q + 200) - 0.5) * 0.3;
      g.sx = 1 + (b.rand(q + 300) > 0.75 ? 0.12 : 0);
      if (b.rand(q + 400) > 0.55) {
        g.tint = "#7ff3ff";
        g.tintK = 0.5;
      }
      f.turn = (b.rand(q + 500) - 0.5) * 0.5;
      f.lid = b.rand(q + 600) > 0.7 ? 1 : 0;
    }
    // crashed: head droops, still; then the ding
    const down = hold(t, CRASH, DING, 0.12);
    f.nod = 0.3 * down;
    f.still = Math.max(down, glitch ? 1 : 0);
    f.squash = -0.06 * down;
    f.bob = 0.1 * bump(t, DING, DING + 0.35) + 0.06 * bump(t, DING + 0.35, DING + 0.6);
    f.gaze = 1;
    if (!glitch) f.lid = blink(t, 3.4);
    // the scan line: rolls down the window twice during the glitch, once on the reboot
    const bar = g.marks[0]!;
    const sweep = (a: number, d: number) => (t > a && t < a + d ? (t - a) / d : -1);
    const s = Math.max(sweep(0.35, 0.45), sweep(0.85, 0.45), sweep(DING - 0.5, 0.45));
    bar.size = s < 0 ? 0 : 0.85;
    bar.y = (0.85 - 1.7 * Math.max(0, s)) * WINDOW;
    if (s >= 0 && tick(b, t, 0.05)) {
      b.fx.ripple(-0.5, bar.y / WINDOW, 0.14, 0.05);
      b.fx.ripple(0.5, bar.y / WINDOW, 0.14, 0.05);
    }
    // loading dots on its blank face
    const e = b.where("eyes") ?? [0, 0.05],
      e1 = b.where("eye") ?? [0.2, 0.05];
    const dx = (e1[0] - e[0]) * 0.75;
    const loading = hold(t, CRASH + 0.25, DING - 0.05, 0.05);
    for (let i = 0; i < 3; i++) {
      const m = g.marks[1 + i]!;
      m.x = (e[0] + dx * (i - 1)) * WINDOW;
      m.y = e[1] * WINDOW;
      const ph = ((((t - CRASH) * 2.6 - i * 0.33) % 1) + 1) % 1;
      m.size = loading * (0.045 + 0.04 * Math.max(0, Math.sin(ph * Math.PI * 2)));
    }
    // DING: a flash at the antenna tip
    const star = g.marks[4]!,
      lt = b.where("light") ?? [0, 0.6];
    star.x = lt[0] * WINDOW;
    star.y = (lt[1] + 0.05) * WINDOW;
    star.size = 0.3 * hold(t, DING, DING + 0.55, 0.06);
    star.rot = t * 4;
    if (b.crossed(0.3)) b.mood("surprised"); // red blinking antenna
    if (b.crossed(CRASH)) {
      b.mood("sleepy"); // dim light
      b.fx.ripple(0, -1.0, 0.45, 0.14);
    }
    if (b.crossed(DING)) {
      b.mood("happy"); // green light, ^ ^
      b.fx.rim(0.6);
      b.fx.ripple(lt[0], lt[1] + 0.15, 0.4, 0.08);
    }
    if (b.crossed(3.95)) b.mood("idle");
  },
  post(b, t) {
    // the face goes blank while it reboots
    const blank = t > CRASH && t < DING;
    for (const e of b.kit.eyes) b.node(e.g).visible = !blank;
  },
};

const SWALLOW = 1.45,
  ZAP = 1.75;
const CELL: Array<[number, number]> = [
  [-0.5, -0.25],
  [0.5, -0.25],
  [0.5, 0.25],
  [-0.5, 0.25],
];
const NUB: Array<[number, number]> = [
  [0.5, -0.1],
  [0.62, -0.1],
  [0.62, 0.1],
  [0.5, 0.1],
];

const eat: Beat = {
  name: "eat",
  len: 4.3,
  start(b) {
    b.g.marks = [
      {
        kind: "poly",
        x: 0,
        y: 0,
        size: 0,
        rot: 0,
        color: "#7cf29a",
        pts: CELL,
      },
      { kind: "poly", x: 0, y: 0, size: 0, rot: 0, color: "#ffffff", pts: NUB },
      ...[0, 1].map(() => ({
        kind: "star" as const,
        x: 0,
        y: 0,
        size: 0,
        rot: 0,
        color: "#ffffff",
      })),
    ];
  },
  frame(b, t) {
    const { g, f } = b,
      L = 4.3,
      out = step(t, 3.5, L - 0.1);
    g.face = step(t, 0, 0.4) * (1 - out);
    g.pin = g.face;
    g.zoom = t < 3.5 ? zlerp(1, 1.15, step(t, 0, 0.5)) : zlerp(1.15, 1, backOut(Math.min(1, (t - 3.5) / (L - 3.6))));
    const mo = b.where("mouth") ?? [0, -0.15];
    // the battery: in from the left, turns to point into the mouth, slides in and shrinks
    const inn = step(t, 0.1, 0.9),
      gulp = step(t, 1.1, SWALLOW);
    const x = -1.2 + (mo[0] - 0.6 + 1.2) * inn + 0.55 * gulp,
      y = mo[1] - 0.08 + 0.3 * (1 - inn);
    const [cell, nub, s1, s2] = g.marks as [
      (typeof g.marks)[0],
      (typeof g.marks)[0],
      (typeof g.marks)[0],
      (typeof g.marks)[0],
    ];
    for (const m of [cell, nub]) {
      m.x = x * WINDOW;
      m.y = y * WINDOW;
      m.rot = 0.3 * Math.sin(t * 4) * (1 - inn);
      m.size = t > SWALLOW ? 0 : 0.36 * (1 - 0.8 * gulp);
    }
    f.gaze = 1;
    f.lx = t < SWALLOW ? Math.max(-1, x - mo[0]) : 0;
    f.ly = t < SWALLOW ? -0.3 : 0;
    f.mouth = t > 0.9 && t < SWALLOW + 0.05 ? 0.8 : 0;
    // ZAP: buzz, sparks, a flash; then charging (light pulses, dials whirr); then the hop
    const buzz = hold(t, ZAP, 2.6, 0.05);
    g.ox = 0.03 * Math.sin(t * 80) * buzz;
    f.still = buzz;
    const fl = Math.max(bump(t, ZAP, ZAP + 0.12), bump(t, ZAP + 0.25, ZAP + 0.35));
    if (fl > 0) {
      g.tint = "#ffffff";
      g.tintK = 0.7 * fl;
    }
    f.squash = 0.1 * step(t, 2.0, 2.7) * (1 - step(t, 2.7, 2.8)) - 0.05 * bump(t, ZAP, ZAP + 0.3);
    f.bob = 0.12 * bump(t, 2.8, 3.2);
    f.lid = Math.max(0.5 * hold(t, ZAP, 2.0, 0.05), blink(t, 3.55));
    const c = b.where("face") ?? [0, 0.1];
    [s1, s2].forEach((m, i) => {
      const a = Math.floor(t * 12) * 2.3 + i * 3;
      m.x = (c[0] + 0.55 * Math.cos(a)) * WINDOW;
      m.y = (c[1] + 0.45 * Math.sin(a)) * WINDOW;
      m.size = 0.2 * buzz * (Math.floor(t * 12 + i) % 2 ? 1 : 0.3);
      m.rot = a;
    });
    if (b.crossed(SWALLOW)) b.fx.ripple(mo[0], mo[1] - 0.35, 0.3, 0.07);
    if (b.crossed(ZAP)) {
      b.mood("surprised");
      for (const [x1, y1] of [
        [-0.95, 0.2],
        [0.95, 0.1],
        [-0.5, -0.95],
        [0.55, -0.95],
      ] as const)
        b.fx.bolt(c[0] + x1 * 0.3, c[1] + y1 * 0.3, x1, y1, 0.75);
      b.fx.rim(0.7, 6);
    }
    if (t > ZAP && t < 2.5 && tick(b, t, 0.12)) {
      const r = b.rand(Math.floor(t / 0.12)) * Math.PI * 2;
      b.fx.emit({
        x: c[0] + 0.4 * Math.cos(r),
        y: c[1] + 0.4 * Math.sin(r),
        vx: 1.4 * Math.cos(r),
        vy: 1.4 * Math.sin(r) + 0.5,
        g: 3,
        life: 0.4,
        strength: 0.25,
        radius: 0.04,
      });
    }
    if (b.crossed(2.05)) b.mood("thinking"); // dials whirr: charging
    if (b.crossed(2.8)) {
      b.mood("happy"); // full: green light
      b.fx.ripple(0, -1.0, 0.45, 0.14);
    }
    if (b.crossed(3.9)) b.mood("idle");
  },
};

export const beats: Beat[] = [reboot, eat];
