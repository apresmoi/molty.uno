// Baby whale: breathes in (puffs up, eyes to the sky), then blows a spout: its own water
// column and drops, and a jet up through the fluid that rains back down either side.
import { backOut, blink, bump, hold, step, WINDOW, zlerp, type Beat } from "./api";

const spout: Beat = {
  name: "spout",
  len: 3.8,
  frame(b, t) {
    const { g, f } = b;
    g.face = 0.4 * step(t, 0, 0.6) * (1 - step(t, 3.2, 3.7));
    g.oy = -0.12 * hold(t, 0.1, 3.6, 0.5); // down a little: room for the jet
    f.gaze = 1;
    f.ly = 0.6 * hold(t, 0.3, 2.6, 0.3);
    f.squash = -0.1 * step(t, 0.3, 0.95) * (1 - step(t, 0.95, 1.05)) + 0.12 * bump(t, 0.98, 1.4);
    f.mouth = 0;
    f.lid = blink(t, 2.9);
    const top = b.where("spout") ?? [0, 0.3];
    // the jet: drops thrown up from the blowhole, falling back either side
    if (t > 1.0 && t < 2.4 && Math.floor(t / 0.06) !== Math.floor((t - b.dt) / 0.06)) {
      const r = b.rand(Math.floor(t / 0.06)) - 0.5;
      b.fx.emit({
        x: top[0],
        y: top[1] + 0.1,
        vx: r * 1.4,
        vy: 3.2,
        g: 5.5,
        life: 0.95,
        strength: 0.32,
        radius: 0.05,
      });
    }
    // and a push up the column now and then (a push touches the whole field: keep them rare)
    if (t > 1.0 && t < 2.2 && Math.floor(t / 0.2) !== Math.floor((t - b.dt) / 0.2))
      b.fx.push(top[0], top[1] + 0.35, 0, 0.6, 0.2);
    if (b.crossed(1.0)) b.fx.ripple(top[0], top[1] + 0.5, 0.7, 0.12);
    if (b.crossed(0.95)) b.mood("happy"); // the whale's own spout shows while happy
    if (b.crossed(2.5)) b.mood("idle");
  },
};

// "eat": a swarm of krill drifts in, twinkling through the fluid; the whale opens wide and
// lunges, gulps the lot (cheeks puffed), swallows, and blows a happy little spout.
const KRILL = Array.from({ length: 6 }, (_, i) => ({
  dx: 0.2 * Math.cos(i * 2.4),
  dy: 0.16 * Math.sin(i * 2.4 + 1),
}));
const LUNGE = 1.7,
  SWALLOW = 2.45;

const eat: Beat = {
  name: "eat",
  len: 4.1,
  start(b) {
    b.g.marks = KRILL.map(() => ({
      kind: "star" as const,
      x: 0,
      y: 0,
      size: 0,
      rot: 0,
      color: "#ffc8a8",
    }));
  },
  frame(b, t) {
    const { g, f } = b,
      L = 4.1,
      out = step(t, 3.4, L - 0.1);
    const mo = b.where("mouth") ?? [0, -0.1];
    // the swarm: drifts in from the upper right and swirls; at the lunge it is sucked in
    const inn = step(t, 0, 1.4),
      suck = step(t, LUNGE, LUNGE + 0.3);
    const sx = 1.3 + (0.55 - 1.3) * inn,
      sy = 0.6 + (0.15 - 0.6) * inn;
    const lunge = hold(t, LUNGE - 0.1, SWALLOW, 0.2);
    g.face = 0.7 * step(t, 0, 0.5) * (1 - out);
    g.pin = g.face;
    g.ox = -0.3 * step(t, 0, 0.5) * (1 - lunge) * (1 - out);
    g.zoom =
      t < 3.4
        ? zlerp(1, 0.85, step(t, 0, 0.5)) * (1 + 0.25 * lunge)
        : zlerp(0.85, 1, backOut(Math.min(1, (t - 3.4) / (L - 3.5))));
    for (let i = 0; i < KRILL.length; i++) {
      const k = KRILL[i]!,
        m = g.marks[i]!;
      const a = t * 2.2 + i;
      const x = sx + k.dx + 0.04 * Math.cos(a),
        y = sy + k.dy + 0.04 * Math.sin(a);
      m.x = (x + (mo[0] - x) * suck) * WINDOW;
      m.y = (y + (mo[1] - y) * suck) * WINDOW;
      m.size = t > LUNGE + 0.3 ? 0 : 0.17 * (0.7 + 0.3 * Math.sin(t * 9 + i * 2)) * step(t, 0.05, 0.4);
      m.rot = a;
      if (t < LUNGE && Math.floor(t / 0.15) !== Math.floor((t - b.dt) / 0.15) && i % 3 === Math.floor(t / 0.15) % 3)
        b.fx.ripple(x, y, 0.1, 0.04);
    }
    f.gaze = 1;
    f.lx = t < LUNGE ? 0.8 : 0;
    f.ly = t < LUNGE ? 0.3 : 0;
    f.mouth = t > LUNGE - 0.35 && t < LUNGE + 0.3 ? 0.95 : 0;
    // cheeks full; the swallow; the spout
    const full = hold(t, LUNGE + 0.3, SWALLOW, 0.1);
    g.sx = 1 + 0.12 * full;
    f.squash = -0.06 * full + 0.1 * bump(t, SWALLOW, SWALLOW + 0.3);
    f.lid = Math.max(0.5 * full, blink(t, 3.5));
    if (b.crossed(LUNGE)) {
      b.fx.push(0.45, 0.1, -0.5, -0.1, 0.25); // the water rushes in
      b.fx.rim(0.4);
    }
    if (b.crossed(SWALLOW)) b.fx.ripple(mo[0], mo[1] - 0.45, 0.35, 0.1);
    if (b.crossed(SWALLOW + 0.25)) b.mood("happy"); // its spout shows
    if (b.crossed(3.6)) b.mood("idle");
  },
};

export const beats: Beat[] = [spout, eat];
