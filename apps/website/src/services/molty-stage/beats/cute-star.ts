// Star: (1) "shooting" — it twinkles, brighter and brighter (sparkles popping round it), winds
// up, streaks across the window as a shooting star (a sparkle trail through the fluid), and
// drifts back down into its place. (2) "eat" — stardust motes swirl round it and it gulps
// them down one by one, glows, and hiccups a sparkle.
import { backOut, blink, bump, easeIn, easeOut, hold, step, WINDOW, type Beat } from "./api";

const WHITE = "#ffffff";
// sparkles round the star (fit units) and when each pops (s)
const POPS: Array<[number, number, number]> = [
  [0.5, 0.42, 0.25],
  [-0.52, 0.3, 0.55],
  [0.42, -0.4, 0.8],
  [-0.4, -0.45, 1.0],
  [0.05, 0.62, 1.15],
];
const TRAIL = 5; // sparkles left along the streak

const shooting: Beat = {
  name: "shooting",
  len: 4.4,
  start(b) {
    b.g.marks = [];
    for (let i = 0; i < POPS.length + TRAIL; i++)
      b.g.marks.push({ kind: "star", x: 0, y: 0, size: 0, rot: 0, color: WHITE });
  },
  frame(b, t) {
    const { g, f } = b;
    // twinkling: three flashes, each brighter
    const tw = 0.25 * bump(t, 0.1, 0.45) + 0.45 * bump(t, 0.5, 0.85) + 0.75 * bump(t, 0.9, 1.35);
    g.tint = WHITE;
    g.tintK = tw;
    g.face = 0.5 * hold(t, 0, 1.4, 0.3);
    g.zoom = 1 + 0.12 * tw;
    for (let i = 0; i < POPS.length; i++) {
      const [x, y, at] = POPS[i]!,
        m = g.marks[i]!;
      m.x = x;
      m.y = y;
      m.size = 0.26 * backOut(Math.min(1, Math.max(0, (t - at) / 0.2))) * (t > at ? 1 : 0) * (1 - step(t, 1.35, 1.6));
      m.rot = t * 2 + i;
    }
    // wind-up (shrinks to bottom-left), the streak (out top-right), gone, then back down
    let x = 0,
      y = 0,
      z = 1;
    if (t < 1.4) {
      // twinkling in place
    } else if (t < 1.75) {
      const q = step(t, 1.4, 1.75);
      x = -0.45 * q;
      y = -0.35 * q;
      z = 1 - 0.35 * q;
    } else if (t < 2.55) {
      const q = easeIn((t - 1.75) / 0.8);
      x = -0.45 + 2.6 * q;
      y = -0.35 + 1.45 * q;
      z = 0.65 - 0.25 * q;
      g.roll = -2.2 * q;
    } else if (t < 2.85) {
      x = 9; // gone
    } else {
      // drifting back in from the top left, a slow falling-leaf sway
      const q = easeOut((t - 2.85) / 1.3);
      x = -1.6 * (1 - q) + 0.12 * Math.sin(q * 7) * (1 - q);
      y = 1.2 * (1 - q);
      z = 0.7 + 0.3 * q;
      g.roll = 0.6 * (1 - q) * Math.sin(q * 5);
    }
    g.ox = x;
    g.oy = y;
    if (t > 1.4) g.zoom = z;
    f.gaze = 1;
    f.lx = t > 1.6 && t < 2.6 ? 0.8 : 0;
    f.ly = t > 1.6 && t < 2.6 ? 0.5 : 0;
    f.lid = Math.max(blink(t, 3.9), t > 1.45 && t < 1.75 ? 0.5 : 0); // squints, ready
    // the trail: sparkles dropped behind it while it streaks, each fading
    for (let i = 0; i < TRAIL; i++) {
      const at = 1.9 + i * 0.12,
        m = g.marks[POPS.length + i]!,
        q = easeIn((at - 1.75) / 0.8);
      m.x = (-0.45 + 2.6 * q) * WINDOW * 0.75;
      m.y = (-0.35 + 1.45 * q) * WINDOW * 0.75 - 0.05;
      m.size = t < at ? 0 : 0.3 * (1 - step(t, at + 0.15, at + 1.0));
      m.rot = t * 3 + i;
    }
    const w = b.where("face");
    if (t > 1.8 && t < 2.5 && w && Math.abs(w[0]) < 1.1 && Math.floor(t / 0.05) !== Math.floor((t - b.dt) / 0.05))
      b.fx.emit({ x: w[0] - 0.12, y: w[1] - 0.1, vx: -0.9, vy: -0.6, life: 0.4, strength: 0.25, radius: 0.05 });
    if (t > 2.9 && t < 3.9 && w && Math.floor(t / 0.14) !== Math.floor((t - b.dt) / 0.14))
      b.fx.ripple(w[0], w[1] + 0.25, 0.1, 0.05);
    if (b.crossed(1.75)) b.fx.rim(0.4);
    if (b.crossed(0.9)) b.mood("happy");
    if (b.crossed(1.4)) b.mood("idle");
    if (b.crossed(3.6)) {
      b.mood("happy");
      b.fx.ripple(0, -0.95, 0.4, 0.14); // lands
    }
    if (b.crossed(4.2)) b.mood("idle");
  },
};

// ---------------------------------------------------------------- eat: stardust
const MOTES = 5;
const DUST = "#bff4ff"; // icy blue: reads against the yellow star
const eat: Beat = {
  name: "eat",
  len: 4.2,
  start(b) {
    b.g.marks = [];
    for (let i = 0; i < MOTES; i++) b.g.marks.push({ kind: "star", x: 0, y: 0, size: 0, rot: 0, color: DUST });
  },
  frame(b, t) {
    const { g, f } = b;
    g.face = hold(t, 0, 4.1, 0.4);
    g.zoom = 1 - 0.18 * hold(t, 0, 4.1, 0.4); // back a little: the motes orbit round it
    const mo = b.where("mouth") ?? [0, -0.1];
    const mx = mo[0] * WINDOW,
      my = mo[1] * WINDOW;
    let gulp = 0;
    for (let i = 0; i < MOTES; i++) {
      const m = g.marks[i]!,
        eatAt = 1.1 + i * 0.5, // each one is gulped here
        a = i * 1.26 + t * 1.6, // swirling round
        r = 0.6 - 0.04 * Math.sin(t * 3 + i);
      const sx = r * Math.cos(a),
        sy = 0.05 + 0.55 * Math.sin(a);
      // pulled in over the last 0.35 s
      const q = easeIn((t - (eatAt - 0.35)) / 0.35);
      m.x = sx + (mx - sx) * q;
      m.y = sy + (my - sy) * q;
      m.size = t < eatAt ? 0.34 * step(t, 0.1 + 0.08 * i, 0.4 + 0.08 * i) * (1 - 0.6 * q) : 0;
      m.rot = t * 4 + i;
      gulp = Math.max(gulp, bump(t, eatAt - 0.25, eatAt + 0.12));
      if (b.crossed(eatAt)) b.fx.ripple(mo[0] + (i % 2 ? 0.3 : -0.3), mo[1] - 0.3, 0.15, 0.06);
    }
    f.mouth = t < 3.6 ? 0.85 * gulp : 0;
    f.squash = 0.06 * gulp;
    f.gaze = 1;
    f.lx = t < 3.6 ? 0.3 * Math.sin(t * 2) : 0;
    f.ly = -0.2;
    // full: it glows, then hiccups a sparkle
    const glow = bump(t, 3.5, 4.0);
    g.tint = WHITE;
    g.tintK = 0.5 * glow;
    f.bob = 0.06 * bump(t, 3.55, 3.75);
    f.lid = blink(t, 0.7);
    if (b.crossed(3.6)) {
      b.mood("happy");
      b.fx.emit({ x: mo[0], y: mo[1] + 0.1, vx: 0.4, vy: 1.4, g: 1.5, life: 0.6, strength: 0.2, radius: 0.05 });
    }
    if (b.crossed(4.05)) b.mood("idle");
  },
};

export const beats: Beat[] = [shooting, eat];
