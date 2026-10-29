// Cloud: a mood. It darkens and scowls, rain streaks fall out of it through the fluid, two
// little bolts crack down (the cloud flashes and jolts), then it clears and brightens.
import { backOut, blink, bump, exposureAnchor, hold, step, WINDOW, zlerp, type Beat } from "./api";

const BOLTS = [1.55, 2.5];

const storm: Beat = {
  name: "storm",
  len: 4.2,
  start(b) {
    // pin the exposure, or the grey would be stretched back to the cloud's usual white
    b.g.marks = [exposureAnchor()];
  },
  frame(b, t) {
    const { g, f } = b,
      L = 4.2;
    const grim = hold(t, 0.1, 3.5, 0.5);
    g.oy = 0.18 * step(t, 0, 0.5) * (1 - step(t, 3.6, L - 0.1)); // up a little: room for the rain
    g.zoom = 1 - 0.1 * step(t, 0, 0.5) * (1 - step(t, 3.6, L - 0.1));
    g.tint = "#59637a";
    g.tintK = 0.45 * grim;
    f.lid = Math.max(0.42 * grim, blink(t, 3.75));
    f.mouth = 0;
    f.gaze = grim;
    f.ly = -0.35;
    f.squash = -0.05 * grim;
    for (const at of BOLTS) {
      const k = bump(t, at, at + 0.14);
      if (k > 0) {
        g.tint = "#ffffff"; // the flash
        g.tintK = 0.7 * k;
        g.ox = 0.03 * Math.sin(t * 90) * k;
        f.squash = 0.08 * k;
      }
    }
    // rain: a drop every ~0.1 s from under the cloud, streaking down through the fluid
    const c = b.where("face") ?? [0, 0.2];
    const raining = t > 0.6 && t < 3.3;
    if (raining && Math.floor(t / 0.07) !== Math.floor((t - b.dt) / 0.07)) {
      const r = b.rand(Math.floor(t / 0.07));
      b.fx.emit({
        x: c[0] + (r - 0.5) * 0.85,
        y: c[1] - 0.35,
        vx: -0.15,
        vy: -2.6,
        life: 0.42,
        strength: 0.32,
        radius: 0.035,
      });
    }
    for (const [i, at] of BOLTS.entries())
      if (b.crossed(at)) {
        const x = c[0] + (i ? 0.22 : -0.18);
        b.fx.bolt(x, c[1] - 0.35, x + (i ? -0.12 : 0.15), -1.0, 0.9);
        b.fx.ripple(x + (i ? -0.12 : 0.15), -1.05, 0.5, 0.12);
      }
    if (b.crossed(0.1)) b.mood("idle");
    if (b.crossed(3.55)) b.mood("happy"); // the sun is out
    if (b.crossed(4.05)) b.mood("idle");
  },
};

// "eat": spots a puddle below, sinks down to it and slurps it up as vapour (wisps rising
// through the fluid into its mouth); the puddle shrinks, the cloud puffs up big and fluffy,
// a satisfied sigh.
const PUDDLE: Array<[number, number]> = Array.from({ length: 10 }, (_, i) => {
  const a = (i / 10) * Math.PI * 2;
  return [Math.cos(a), 0.3 * Math.sin(a)];
});
const SLURP = [1.0, 2.5];

const slurp: Beat = {
  name: "eat",
  len: 4.1,
  start(b) {
    b.g.marks = [
      {
        kind: "poly",
        x: 0,
        y: -0.7,
        size: 0,
        rot: 0,
        color: "#8fcfff",
        pts: PUDDLE,
      },
    ];
  },
  frame(b, t) {
    const { g, f } = b,
      L = 4.1,
      out = step(t, 3.3, L - 0.1);
    g.face = 0.5 * step(t, 0, 0.5) * (1 - out);
    const sink = step(t, 0.4, 1.0) * (1 - step(t, 2.6, 3.2));
    g.oy = -0.08 * sink;
    const slurping = hold(t, SLURP[0]!, SLURP[1]!, 0.15);
    const fill = step(t, SLURP[0]!, SLURP[1]!);
    // fuller and fluffier as it drinks; a sigh at the end
    g.zoom = t < 3.3 ? zlerp(1, 1.18, fill) : zlerp(1.18, 1, backOut(Math.min(1, (t - 3.3) / (L - 3.4))));
    g.sx = 1 + 0.08 * fill * (1 - out);
    f.squash = -0.04 * slurping * Math.sin(t * 18) + 0.08 * bump(t, 2.7, 3.2);
    f.gaze = 1;
    f.ly = -0.85 * hold(t, 0.1, SLURP[1]!, 0.25);
    f.mouth = slurping > 0 ? 0.25 + 0.15 * Math.sin(t * 18) : 0;
    f.lid = t > 2.7 && t < 3.25 ? 0.9 : blink(t, 0.6);
    const p = g.marks[0]!;
    p.size = t < 0.2 ? 0 : 0.42 * step(t, 0.2, 0.5) * (1 - 0.95 * fill);
    const mo = b.where("mouth") ?? [0, -0.2];
    // the vapour: wisps rising from the puddle to the mouth
    if (slurping > 0.5 && Math.floor(t / 0.09) !== Math.floor((t - b.dt) / 0.09)) {
      const r = b.rand(Math.floor(t / 0.09)) - 0.5;
      const x0 = r * 0.6 * (1 - 0.8 * fill),
        y0 = p.y / WINDOW;
      const life = 0.45;
      b.fx.emit({
        x: x0,
        y: y0,
        vx: (mo[0] - x0) / life,
        vy: (mo[1] - 0.05 - y0) / life,
        life,
        strength: 0.18,
        radius: 0.04,
      });
    }
    if (b.crossed(2.65)) b.mood("happy"); // full: the rainbow
    if (b.crossed(3.7)) b.mood("idle");
  },
};

export const beats: Beat[] = [storm, slurp];
