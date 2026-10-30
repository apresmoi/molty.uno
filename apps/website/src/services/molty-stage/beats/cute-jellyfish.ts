// Jellyfish: (1) "zap" — pulses up to the glass in three beats of its bell, touches it and
// gets a static shock: the window flashes, bolts crackle out to the rim, sparks fly, its
// tentacles stand on end; frazzled, it sinks back and giggles. (2) "eat" — plankton specks
// drift in; it pulses after each one, catches them in turn (a little gulp each) and glows.
import type { MoltyMark } from "../moltyMesh";
import { backOut, blink, bump, hold, step, WINDOW, zlerp, type Beat } from "./api";

const SPARK = "#ffffff";
const PLANKTON = "#fff08a";
const OCT: Array<[number, number]> = Array.from({ length: 8 }, (_, i) => {
  const a = (i / 8) * Math.PI * 2;
  return [Math.cos(a), Math.sin(a)];
});
const ZAP = 1.35;

/** A zigzag lightning bolt as quads (marks are convex polys): `n` segments from (x0, y0) to
 *  (x1, y1), fit units, written into marks[at…at+n). */
function zig(marks: MoltyMark[], at: number, n: number, x0: number, y0: number, x1: number, y1: number, w = 0.03) {
  const dx = x1 - x0,
    dy = y1 - y0,
    l = Math.hypot(dx, dy) || 1,
    nx = -dy / l,
    ny = dx / l;
  let px = x0,
    py = y0;
  for (let i = 1; i <= n; i++) {
    const q = i / n,
      j = i < n ? (i % 2 ? 0.09 : -0.09) : 0;
    const qx = x0 + dx * q + nx * j,
      qy = y0 + dy * q + ny * j;
    const m = marks[at + i - 1]!;
    m.x = m.y = m.rot = 0;
    // counter-clockwise, like every mark (the other winding is culled)
    m.pts = [
      [px - nx * w, py - ny * w],
      [qx - nx * w, qy - ny * w],
      [qx + nx * w, qy + ny * w],
      [px + nx * w, py + ny * w],
    ];
    px = qx;
    py = qy;
  }
}
const BOLTS = 3,
  SEGS = 3;

const zap: Beat = {
  name: "zap",
  len: 4.2,
  start(b) {
    b.g.marks = [
      ...[0, 1, 2].map(() => ({
        kind: "star" as const,
        x: 0,
        y: 0,
        size: 0,
        rot: 0,
        color: SPARK,
      })),
      ...Array.from({ length: BOLTS * SEGS }, () => ({
        kind: "poly" as const,
        x: 0,
        y: 0,
        size: 0,
        rot: 0,
        color: SPARK,
        pts: [],
      })),
    ];
  },
  frame(b, t) {
    const { g, f } = b,
      L = 4.2,
      out = step(t, 2.6, L - 0.1);
    // three pulses up and toward the glass
    const rise = 0.06 * (step(t, 0.1, 0.4) + step(t, 0.5, 0.8) + step(t, 0.9, 1.2));
    g.face = 0.6 * step(t, 0, 0.6) * (1 - out);
    g.pin = g.face;
    g.oy = rise * (1 - out);
    g.zoom =
      t < 2.6
        ? zlerp(1, 1.3, step(t, 0.1, 1.3)) - 0.12 * step(t, ZAP, ZAP + 0.2)
        : zlerp(1.18, 1, backOut(Math.min(1, (t - 2.6) / (L - 2.7))));
    f.squash =
      -0.09 * (bump(t, 0.05, 0.35) + bump(t, 0.45, 0.75) + bump(t, 0.85, 1.15)) + 0.1 * bump(t, ZAP, ZAP + 0.3);
    f.gaze = 1;
    // the shock: two flashes, a buzz
    const fl = Math.max(bump(t, ZAP, ZAP + 0.12), bump(t, ZAP + 0.22, ZAP + 0.34));
    if (fl > 0) {
      g.tint = "#ffffff";
      g.tintK = 0.75 * fl;
    }
    const fraz = hold(t, ZAP, 2.5, 0.1);
    g.ox = 0.03 * Math.sin(t * 95) * hold(t, ZAP, ZAP + 0.6, 0.05);
    g.roll = 0.08 * fraz * Math.sin((t - ZAP) * 9);
    f.lid = t > 1.95 && t < 2.5 ? 0.5 : Math.max(blink(t, 0.7), blink(t, 3.3));
    f.mouth = t > ZAP && t < 1.9 ? 0.5 : -1;
    // crackling sparks round the bell
    const c = b.where("face") ?? [0, 0];
    for (let i = 0; i < 3; i++) {
      const m = g.marks[i]!,
        a = i * 2.1 + Math.floor(t * 14) * 1.7;
      const on = hold(t, ZAP, 2.1, 0.05) * (Math.floor(t * 14 + i) % 2 ? 1 : 0.4);
      m.x = (c[0] + 0.62 * Math.cos(a)) * WINDOW;
      m.y = (c[1] + 0.1 + 0.5 * Math.sin(a)) * WINDOW;
      m.size = 0.2 * on;
      m.rot = a;
    }
    // the lightning bolts: crackling for half a second, re-drawn (a new zigzag) every 0.15 s
    const lit = t > ZAP && t < ZAP + 0.6;
    for (let i = 0; i < BOLTS * SEGS; i++) g.marks[3 + i]!.size = lit ? 1 : 0;
    if ((lit && Math.floor((t - ZAP) / 0.15) !== Math.floor((t - b.dt - ZAP) / 0.15)) || b.crossed(ZAP)) {
      const k = Math.floor((t - ZAP) / 0.15) % 2;
      const ends = k
        ? [
            [-0.82, 0.1],
            [0.75, -0.45],
            [-0.2, -0.82],
          ]
        : [
            [-0.75, -0.45],
            [0.82, 0.05],
            [0.25, -0.82],
          ];
      ends.forEach(([x1, y1], i) =>
        zig(g.marks, 3 + i * SEGS, SEGS, (c[0] + (i - 1) * 0.2) * WINDOW, (c[1] - 0.25) * WINDOW, x1!, y1!, 0.045),
      );
    }
    if (b.crossed(ZAP)) {
      b.mood("surprised");
      for (const [x1, y1] of [
        [-0.95, -0.35],
        [0.95, -0.4],
        [-0.3, -1.0],
        [0.4, -1.0],
      ] as const)
        b.fx.bolt(c[0] + x1 * 0.25, c[1] - 0.35, x1, y1, 0.85);
      b.fx.rim(0.8, 6);
      for (let i = 0; i < 8; i++) {
        const a = (i / 8) * Math.PI * 2 + 0.3;
        b.fx.emit({
          x: c[0] + 0.3 * Math.cos(a),
          y: c[1] + 0.3 * Math.sin(a),
          vx: 1.8 * Math.cos(a),
          vy: 1.8 * Math.sin(a) + 0.6,
          g: 3,
          life: 0.45,
          strength: 0.3,
          radius: 0.04,
        });
      }
    }
    if (b.crossed(ZAP + 0.25)) {
      b.fx.bolt(c[0] - 0.2, c[1] - 0.4, -0.9, -0.85, 0.6);
      b.fx.bolt(c[0] + 0.2, c[1] - 0.4, 0.95, 0.1, 0.6);
    }
    if (b.crossed(1.95)) b.mood("idle");
    if (b.crossed(2.9)) b.mood("happy"); // giggles it off
    if (b.crossed(3.8)) b.mood("idle");
  },
  post(b, t) {
    // tentacles on end: splayed out and buzzing
    const fraz = hold(t, ZAP, 2.5, 0.12);
    if (fraz <= 0) return;
    b.part("tentacle").forEach((m, i) => {
      m.rotation.x -= fraz * (0.9 + 0.12 * Math.sin(t * 60 + i * 2));
    });
    for (const m of b.part("arm")) m.rotation.x -= 0.6 * fraz;
  },
};

// the plankton: where each speck comes from (window units), when it appears and when it is caught
const SPECKS = [
  { x: -1.0, y: 0.55, a: 0.0, c: 1.15 },
  { x: 1.0, y: 0.25, a: 0.5, c: 2.0 },
  { x: -0.85, y: -0.55, a: 1.2, c: 2.75 },
];

const eat: Beat = {
  name: "eat",
  len: 4.0,
  start(b) {
    b.g.marks = SPECKS.map(() => ({
      kind: "poly" as const,
      x: 0,
      y: 0,
      size: 0,
      rot: 0,
      color: PLANKTON,
      pts: OCT,
    }));
  },
  frame(b, t) {
    const { g, f } = b,
      L = 4.0,
      out = step(t, 3.2, L - 0.1);
    g.face = step(t, 0, 0.5) * (1 - out);
    const m = b.where("mouth") ?? [0, -0.1];
    // which speck it is after: lean / drift toward it, eyes on it
    let tx = 0,
      ty = 0;
    for (const s of SPECKS)
      if (t < s.c) {
        const p = Math.min(1, Math.max(0, (t - s.a) / (s.c - s.a)));
        tx = s.x + (m[0] - s.x) * p;
        ty = s.y + (m[1] - s.y) * p;
        break;
      }
    const chase = hold(t, 0.2, 3.0, 0.4);
    g.ox = 0.18 * Math.sign(tx) * Math.min(1, Math.abs(tx)) * chase;
    g.oy = 0.08 * ty * chase;
    f.gaze = chase;
    f.lx = Math.max(-1, Math.min(1, tx - m[0]));
    f.ly = Math.max(-1, Math.min(1, ty - m[1]));
    // a gulp at each catch: a squeeze of the bell, the mouth snaps
    let gulp = 0;
    for (const s of SPECKS) gulp = Math.max(gulp, bump(t, s.c - 0.05, s.c + 0.3));
    f.squash = -0.1 * gulp;
    f.mouth = gulp > 0.05 ? 0.45 * gulp : 0;
    f.lid = blink(t, 3.4);
    for (let i = 0; i < SPECKS.length; i++) {
      const s = SPECKS[i]!,
        mk = g.marks[i]!;
      const p = Math.min(1, Math.max(0, (t - s.a) / (s.c - s.a)));
      const x = s.x + (m[0] - s.x) * p + 0.06 * Math.sin(t * 5 + i) * (1 - p),
        y = s.y + (m[1] - s.y) * p + 0.06 * Math.cos(t * 4 + i * 2) * (1 - p);
      mk.x = x * WINDOW;
      mk.y = y * WINDOW;
      mk.size = t < s.a || t > s.c ? 0 : 0.08 * step(t, s.a, s.a + 0.2) * (1 - 0.6 * step(t, s.c - 0.15, s.c));
      mk.rot = t * 2;
      if (t > s.a && t < s.c && Math.floor(t / 0.12) !== Math.floor((t - b.dt) / 0.12)) b.fx.ripple(x, y, 0.12, 0.045);
      if (b.crossed(s.c)) b.fx.ripple(m[0], m[1] - 0.3, 0.3, 0.08);
    }
    // full: a happy glow
    if (t > 2.9 && t < 3.6) {
      g.tint = "#fff4c0";
      g.tintK = 0.3 * bump(t, 2.9, 3.6);
    }
    if (b.crossed(2.85)) b.mood("happy");
    if (b.crossed(3.7)) b.mood("idle");
  },
};

export const beats: Beat[] = [zap, eat];
