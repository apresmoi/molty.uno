// Octopus: gets cross (squints, puffs up, trembles), then squirts ink at the glass: a dark
// splat floods the window (the fluid thrown outward round it) while the octopus jets back;
// the ink slides down and off the glass, and the octopus is pleased with itself.
import { backOut, blink, bump, exposureAnchor, hold, lerp, step, WINDOW, zlerp, type Beat } from "./api";

// dim but not "feature" dark (that draws solid light blocks, like the eyes): with the
// exposure pinned to black it reads as a dark, dotted ink cloud over the window
const INK = "#5a4a80";
const SQUIRT = 1.0;

const ink: Beat = {
  name: "ink",
  len: 4.2,
  start(b) {
    b.g.marks = [{ kind: "blot", x: 0, y: 0, size: 0, rot: 0, color: INK }, exposureAnchor()];
  },
  frame(b, t) {
    const { g, f } = b,
      L = 4.2;
    const cross = hold(t, 0.1, SQUIRT + 0.1, 0.25);
    g.face = step(t, 0, 0.5) * (1 - step(t, 3.5, L - 0.1));
    g.pin = g.face;
    // in close and cross; jets back at the squirt; returns
    if (t < SQUIRT) g.zoom = lerp(1, 1.35, step(t, 0, 0.6));
    else if (t < 3.4) g.zoom = lerp(1.35, 0.85, step(t, SQUIRT, SQUIRT + 0.25)) + 0.1 * step(t, 2.6, 3.4);
    else g.zoom = zlerp(0.95, 1, backOut(Math.min(1, (t - 3.4) / (L - 3.5))));
    g.ox = 0.025 * Math.sin(t * 70) * step(t, 0.4, SQUIRT) * (t < SQUIRT ? 1 : 0); // trembling with rage
    f.squash = 0.07 * cross;
    f.lid = Math.max(0.45 * cross, blink(t, 3.3));
    f.mouth = 0;
    f.gaze = 1;
    // the ink: bursts over the window, holds, slides down and off as it thins
    const m = g.marks[0]!,
      burst = step(t, SQUIRT, SQUIRT + 0.18),
      slide = step(t, 1.75, 3.0);
    m.size = 2.5 * burst * (1 - 0.45 * slide) * (1 - step(t, 2.7, 3.05));
    m.y = -1.5 * slide;
    m.rot = 0.4 * t;
    if (b.crossed(SQUIRT)) {
      b.mood("surprised"); // its own ink puffs and curled tentacles
      b.fx.rim(1.0, 6);
      for (let i = 0; i < 6; i++) {
        const a = (i / 6) * Math.PI * 2;
        b.fx.push(0.7 * Math.cos(a), 0.7 * Math.sin(a), 0.5 * Math.cos(a), 0.5 * Math.sin(a), 0.3);
      }
    }
    if (b.crossed(1.8)) b.fx.ripple(0, -1.05, 0.5, 0.14); // it runs off the bottom
    if (b.crossed(2.4)) b.fx.ripple(0.2, -1.05, 0.4, 0.12);
    if (b.crossed(1.5)) b.mood("idle");
    if (b.crossed(3.0)) b.mood("happy"); // smug
    if (b.crossed(3.7)) b.mood("idle");
  },
};

// "eat": a little shrimp swims in, flicking; the octopus watches, a tentacle shoots out and
// snatches it, brings it to its mouth; three munches; a pleased wiggle.
const SNATCH = 1.55,
  BITES = [2.3, 2.65, 3.0];
const SHRIMP = "#ffb090";

const eat: Beat = {
  name: "eat",
  len: 4.1,
  start(b) {
    b.g.marks = [
      { kind: "drop", x: 0, y: 0, size: 0, rot: 0, color: SHRIMP },
      { kind: "drop", x: 0, y: 0, size: 0, rot: 0, color: SHRIMP },
      // the snatching tentacle, drawn flat: a tapered strip from under the mouth to the shrimp
      { kind: "poly", x: 0, y: 0, size: 0, rot: 0, color: "#e3d3ff", pts: [] },
    ];
  },
  frame(b, t) {
    const { g, f } = b,
      L = 4.1,
      out = step(t, 3.35, L - 0.1);
    g.face = step(t, 0, 0.5) * (1 - out);
    g.pin = g.face;
    g.zoom = zlerp(1, 1.05, step(t, 0, 0.6) * (1 - out));
    g.ox = -0.2 * step(t, 0, 0.6) * (1 - out);
    const mo = b.where("mouth") ?? [0, -0.1];
    // the shrimp: swims in from the right in little flicks, hangs there; snatched to the mouth
    const swim = step(t, 0, 1.3),
      flick = Math.sin(t * 14) * (t < SNATCH ? 1 : 0);
    const hx = mo[0] + 0.8,
      hy = mo[1] + 0.05;
    const grab = step(t, SNATCH, SNATCH + 0.4);
    const x = (1.3 + (hx - 1.3) * swim) * (1 - grab) + (mo[0] + 0.05) * grab,
      y = (hy + 0.15 * Math.sin(t * 3) * (1 - swim)) * (1 - grab) + (mo[1] - 0.05) * grab;
    let left = 1;
    for (const at of BITES) if (t > at) left -= 1 / BITES.length;
    const [body, tail] = g.marks as [(typeof g.marks)[0], (typeof g.marks)[0]];
    body.x = x * WINDOW;
    body.y = y * WINDOW;
    body.rot = Math.PI / 2 + 0.15 * flick; // a drop on its side: head left, tapering right
    body.size = 0.28 * Math.max(0, left);
    tail.x = (x + 0.24 * left) * WINDOW;
    tail.y = (y + 0.03 + 0.02 * flick) * WINDOW;
    tail.rot = -Math.PI / 2 + 0.6 * flick;
    tail.size = t > BITES[1]! ? 0 : 0.17;
    // eyes on it; the snatch; munching
    f.gaze = 1;
    f.lx = t < SNATCH ? Math.max(-1, Math.min(1, (x - mo[0]) * 1.6)) : 0;
    f.ly = t < SNATCH ? 0.1 : -0.3;
    f.lid = t > 0.9 && t < SNATCH ? 0.35 : blink(t, 3.4); // stalking it
    f.squash = -0.06 * hold(t, 0.9, SNATCH, 0.1) + 0.08 * bump(t, SNATCH, SNATCH + 0.3);
    let chomp = 0;
    for (const at of BITES) chomp = Math.max(chomp, bump(t, at - 0.12, at + 0.12));
    f.mouth = chomp > 0.05 ? 0.6 * chomp : 0;
    // the tentacle: shoots out to the shrimp, then pulls it in (its tip on the shrimp)
    const arm = g.marks[2]!,
      out2 = step(t, SNATCH - 0.2, SNATCH),
      on = t > SNATCH - 0.2 && t < SNATCH + 0.45;
    arm.size = on ? 1 : 0;
    if (on) {
      const x0 = (mo[0] + 0.15) * WINDOW,
        y0 = (mo[1] - 0.18) * WINDOW,
        x1 = (x0 / WINDOW + (x - x0 / WINDOW) * out2) * WINDOW,
        y1 = (y0 / WINDOW + (y - y0 / WINDOW) * out2) * WINDOW;
      const dx = x1 - x0,
        dy = y1 - y0,
        l = Math.hypot(dx, dy) || 1,
        nx = -dy / l,
        ny = dx / l;
      arm.pts = [
        [x0 - nx * 0.06, y0 - ny * 0.06],
        [x1 - nx * 0.03, y1 - ny * 0.03],
        [x1 + nx * 0.03, y1 + ny * 0.03],
        [x0 + nx * 0.06, y0 + ny * 0.06],
      ]; // counter-clockwise (the other winding is culled)
    }
    if (t < SNATCH && Math.floor(t / 0.25) !== Math.floor((t - b.dt) / 0.25)) b.fx.ripple(x + 0.15, y, 0.15, 0.05);
    if (b.crossed(SNATCH)) b.fx.push(hx, hy, -0.4, 0, 0.15);
    for (const at of BITES) if (b.crossed(at)) b.fx.ripple(mo[0] + 0.2, mo[1] - 0.35, 0.22, 0.05);
    if (b.crossed(3.15)) b.mood("happy");
    if (b.crossed(3.85)) b.mood("idle");
  },
};

export const beats: Beat[] = [ink, eat];
