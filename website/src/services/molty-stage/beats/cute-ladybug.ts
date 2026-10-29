// Ladybug: (1) "fly" — it lifts its spotted shell, its little wings whirr out, it takes off,
// buzzes a loop round the window (a wake in the fluid), lands with a bump and folds the shell
// shut. (2) "eat" — a leaf drifts down in front of it and it munches its way across, a bite
// at a time (the leaf's edge scalloped away), crumbs dropping into the fluid.
import { blink, bump, easeOut, hold, step, type Beat, type BeatCtx } from "./api";

/** Uniform scale (the rig types expose only `set`). */
const sc = (o: { scale: { set(x: number, y: number, z: number): unknown } }, k: number) => o.scale.set(k, k, k);

/** The shell halves open and the wings out (the kit sets them from `happy` every frame). */
const wings = (b: BeatCtx, open: number, t: number) => {
  for (const s of [1, -1]) {
    const shell = b.part(`shell${s}`)[0];
    if (shell) {
      shell.rotation.z = s * 0.95 * open;
      shell.rotation.x = -0.25 * open;
    }
    for (const w of b.part(`fly${s}`)) {
      w.visible = open > 0.05;
      sc(w, 0.001 + 1.25 * open);
      w.rotation.z =
        (w as unknown as { userData: { baseRot: { z: number } } }).userData.baseRot.z -
        s * 0.45 * open * Math.sin(t * 52);
    }
  }
};

const fly: Beat = {
  name: "fly",
  len: 4.4,
  frame(b, t) {
    const { g, f } = b;
    let x = 0,
      y = 0,
      vx = 0;
    const up = step(t, 0.55, 1.0),
      down = step(t, 3.0, 3.55);
    if (t < 3.0) {
      // a loop round the window once airborne
      const a = Math.max(0, t - 1.0) * 3.0;
      x = 0.42 * Math.sin(a) * up;
      y = 0.3 * up + 0.14 * Math.sin(a * 2) * up;
      vx = Math.cos(a) * up;
    } else {
      const a = 2.0 * 3.0;
      x = 0.42 * Math.sin(a) * (1 - down);
      y = (0.3 + 0.14 * Math.sin(a * 2)) * (1 - down) + 0.04 * bump(t, 3.55, 3.75);
    }
    g.ox = x;
    g.oy = y;
    g.zoom = 1 - 0.28 * up * (1 - down);
    g.face = 0.4 * hold(t, 0, 4.3, 0.4);
    g.yaw = 0.7 * Math.max(-1, Math.min(1, vx * 2));
    f.lean = -0.2 * vx;
    f.squash = -0.08 * bump(t, 0.35, 0.6) - 0.1 * bump(t, 3.5, 3.75); // crouch to launch, the landing
    f.gaze = 1;
    f.lx = 0.6 * vx;
    f.ly = 0.5 * hold(t, 0, 0.6, 0.2);
    f.lid = blink(t, 0.25);
    const w = b.where("face");
    if (w && t > 1.0 && t < 3.4 && Math.floor(t / 0.08) !== Math.floor((t - b.dt) / 0.08))
      b.fx.ripple(w[0], w[1] - 0.22, 0.12, 0.05);
    if (b.crossed(0.6)) b.fx.ripple(0, -0.95, 0.35, 0.12); // lift-off
    if (b.crossed(3.55)) b.fx.ripple(0, -0.95, 0.45, 0.14); // touchdown
    if (b.crossed(3.7)) b.mood("happy");
    if (b.crossed(4.2)) b.mood("idle");
  },
  post(b, t) {
    wings(b, step(t, 0.1, 0.5) * (1 - step(t, 3.6, 4.0)), t);
  },
};

// ---------------------------------------------------------------- eat: a leaf
const LEAF = "#9fe86e";
const VEIN = "#f1ffe0";
// the leaf in strips across (fit units, at size 1), eaten from the right (nearest the mouth)
const STRIPS: Array<Array<[number, number]>> = [];
{
  const xs = [-0.5, -0.3, -0.1, 0.1, 0.3, 0.5];
  const hw = (x: number) => 0.24 * Math.sqrt(Math.max(0, 1 - (x / 0.5) ** 2)) + 0.004;
  for (let i = 0; i + 1 < xs.length; i++) {
    const a = xs[i]!,
      c = xs[i + 1]!;
    STRIPS.push([
      [a, -hw(a)],
      [c, -hw(c)],
      [c, hw(c)],
      [a, hw(a)],
    ]);
  }
}
const BITES = [1.3, 1.75, 2.2, 2.65, 3.05]; // one strip each, right to left

const eat: Beat = {
  name: "eat",
  len: 4.2,
  start(b) {
    b.g.marks = STRIPS.map((pts) => ({ kind: "poly" as const, x: 0, y: 0, size: 0, rot: 0, color: LEAF, pts }));
    b.g.marks.push({
      kind: "poly",
      x: 0,
      y: 0,
      size: 0,
      rot: 0,
      color: VEIN,
      pts: [
        [-0.5, -0.02],
        [0.5, -0.02],
        [0.5, 0.02],
        [-0.5, 0.02],
      ],
    });
  },
  frame(b, t) {
    const { g, f } = b;
    const k = hold(t, 0, 4.1, 0.4);
    g.face = k;
    g.zoom = 1 - 0.12 * k;
    g.ox = -0.22 * k; // aside and up: the leaf lies down to its right
    g.oy = 0.1 * k;
    // the leaf drifts down (rocking) to beside its mouth
    const land = easeOut(Math.min(1, t / 0.9));
    const lx = 0.4,
      ly = -0.5 + 0.9 * (1 - land),
      rot = 0.3 * Math.sin(t * 5) * (1 - land) - 0.1;
    let eaten = 0;
    for (let i = 0; i < STRIPS.length; i++) {
      const m = g.marks[i]!,
        at = BITES[STRIPS.length - 1 - i]!;
      m.x = lx;
      m.y = ly;
      m.rot = rot;
      m.size = t < at ? 0.62 : 0;
      if (t >= at) eaten++;
    }
    const v = g.marks[STRIPS.length]!;
    v.x = lx;
    v.y = ly;
    v.rot = rot;
    // the vein goes as the strips go: shrink it toward the left end
    v.size = eaten >= STRIPS.length ? 0 : 0.62 * (1 - eaten / STRIPS.length);
    v.x = lx - 0.31 * (eaten / STRIPS.length) * Math.cos(rot);
    // each bite: lean in and down toward the leaf's edge, chomp
    let chomp = 0;
    for (const at of BITES) chomp = Math.max(chomp, bump(t, at - 0.2, at + 0.15));
    const edge = lx + 0.31 - 0.124 * eaten; // fit x of the leaf's right edge, roughly
    f.nod = 0.25 * chomp * hold(t, 1.0, 3.3, 0.2);
    f.px = 0.04 * Math.max(-1, Math.min(1, edge - 0.4)) * hold(t, 1.0, 3.3, 0.3);
    f.mouth = chomp > 0.05 ? 0.7 * chomp : 0;
    f.gaze = 1;
    f.ly = t < 3.3 ? -0.6 : 0;
    f.lx = t < 3.3 ? 0.3 : 0;
    f.lid = blink(t, 3.5);
    for (const at of BITES)
      if (b.crossed(at)) {
        // crumbs off the bitten edge
        const ex = (lx + 0.31 - 0.124 * BITES.indexOf(at)) / 0.85;
        b.fx.emit({ x: ex, y: ly / 0.85, vx: 0.2, vy: 0.3, g: 4, life: 0.45, strength: 0.16, radius: 0.04 });
      }
    if (b.crossed(3.35)) b.mood("happy");
    if (b.crossed(4.0)) b.mood("idle");
  },
};

export const beats: Beat[] = [fly, eat];
