// Ghost: (1) "boo" — a sly look, then it fades away to nothing; the empty window, a faint
// something drifting through the fluid… BOO! it pops up huge against the glass, mouth wide,
// arms up, the glass shudders; then it giggles and settles back. (2) "eat" — an ice-cream
// cone floats in; it licks it twice, gulps it whole, and you can see the scoop sink down
// inside its see-through body (it glows pink), then a happy shiver.
import type { RigObject } from "@creatures/cute/kit.js";
import { backOut, blink, bump, exposureAnchor, hold, step, WINDOW, zlerp, type Beat } from "./api";

/** A tiny white square outside the window: pins the bright end of the exposure, so a ghost
 *  tinted toward the dark really looks dimmer (with only the dark anchor, the auto-exposure
 *  stretches a uniformly dimmed ghost back to full). */
const brightAnchor = () => ({ ...exposureAnchor(), x: 0.8, color: "#ffffff" });

const GONE = 1.2,
  BOO = 2.0;

const boo: Beat = {
  name: "boo",
  len: 4.3,
  start(b) {
    b.g.marks = [
      { kind: "!", x: -0.62, y: 0.5, size: 0, rot: -0.2, color: "#ffffff" },
      exposureAnchor(),
      brightAnchor(),
    ];
  },
  frame(b, t) {
    const { g, f } = b,
      L = 4.3,
      out = step(t, 3.4, L - 0.1);
    // the fade: dimmer and thinner, then not there at all
    const fade = step(t, 0.35, GONE);
    g.tint = "#3c3850";
    g.tintK = 0.85 * fade * (t < GONE ? 1 : 0);
    g.sx = 1 - 0.25 * fade;
    if (t < BOO) {
      g.face = 0.5 * step(t, 0, 0.4);
      g.zoom = 1 - 0.15 * fade;
      if (t >= GONE) g.ox = 9; // gone
      f.gaze = 1;
      f.lx = 0.8 * hold(t, 0, 0.6, 0.15);
      f.lid = 0.35 * hold(t, 0, 0.6, 0.15);
    } else {
      // BOO: pops up big from below, a shudder; giggles; back to its place
      const pop = Math.min(1, (t - BOO) / 0.22);
      g.face = 1 - out;
      g.pin = 1 - out;
      g.zoom =
        t < 3.4
          ? zlerp(0.5, 2.3, backOut(pop)) * (1 - 0.1 * step(t, 2.75, 3.1))
          : zlerp(2.07, 1, backOut(Math.min(1, (t - 3.4) / (L - 3.5))));
      g.oy = -0.6 * (1 - pop);
      g.ox = 0.035 * Math.sin(t * 70) * hold(t, BOO + 0.15, BOO + 0.55, 0.05);
      g.roll = 0.08 * Math.sin((t - 2.75) * 14) * hold(t, 2.75, 3.35, 0.1);
      f.bob = 0.04 * Math.abs(Math.sin((t - 2.75) * 14)) * hold(t, 2.75, 3.35, 0.1);
      f.gaze = 1;
      f.mouth = t < 2.75 ? 0.95 : -1;
      f.lid = blink(t, 3.6);
    }
    // something unseen drifts through the empty window
    if (t > GONE + 0.05 && t < BOO - 0.1 && Math.floor(t / 0.1) !== Math.floor((t - b.dt) / 0.1)) {
      const q = (t - GONE) / (BOO - GONE);
      b.fx.ripple(-0.7 + 1.4 * q, -0.2 + 0.25 * Math.sin(q * 6), 0.16, 0.08);
    }
    const ex = g.marks[0]!;
    ex.size = t < BOO + 0.05 ? 0 : 0.42 * backOut(Math.min(1, (t - BOO - 0.05) / 0.2)) * (1 - step(t, 2.6, 2.8));
    if (b.crossed(0.4)) b.mood("thinking"); // the sly sideways smile
    if (b.crossed(GONE)) b.mood("surprised"); // ready for the pop
    if (b.crossed(BOO)) {
      b.fx.rim(0.95, 6);
      b.fx.ripple(0, -1.05, 0.6, 0.16);
    }
    if (b.crossed(2.75)) b.mood("happy"); // giggles
    if (b.crossed(3.9)) b.mood("idle");
  },
  post(b, t) {
    // arms up for the BOO
    const up = hold(t, BOO, 2.75, 0.1);
    if (up <= 0) return;
    for (const r of b.kit.sock["side"] ?? []) b.node(r.g).rotation.z += r.side * 0.9 * up;
    // a big round "O" of a mouth (the spec's is tiny)
    const mouth = (b.kit as unknown as { mouth?: { g: RigObject } }).mouth;
    if (mouth) {
      const g = b.node(mouth.g),
        st = b.state;
      if (st.mx === undefined) {
        st.mx = g.scale.x;
        st.my = g.scale.y;
      }
      const k = 1 + 1.6 * up;
      g.scale.set(st.mx * k, st.my! * k, g.scale.z);
    }
  },
};

const LICKS = [1.2, 1.7],
  GULP = 2.25;
const DISC: Array<[number, number]> = Array.from({ length: 10 }, (_, i) => [
  Math.cos((i / 10) * Math.PI * 2),
  Math.sin((i / 10) * Math.PI * 2),
]);
const CONE: Array<[number, number]> = [
  [0, -0.75],
  [0.38, 0.1],
  [-0.38, 0.1],
];

const eat: Beat = {
  name: "eat",
  len: 4.2,
  start(b) {
    b.g.marks = [
      {
        kind: "poly",
        x: 0,
        y: 0,
        size: 0,
        rot: 0,
        color: "#ffd890",
        pts: CONE,
      },
      {
        kind: "poly",
        x: 0,
        y: 0,
        size: 0,
        rot: 0,
        color: "#ffc0e0",
        pts: DISC,
      },
    ];
  },
  frame(b, t) {
    const { g, f } = b,
      L = 4.2,
      out = step(t, 3.45, L - 0.1);
    g.face = step(t, 0, 0.5) * (1 - out);
    g.pin = g.face;
    g.zoom = zlerp(1, 1.05, step(t, 0, 0.6) * (1 - out));
    g.ox = -0.3 * step(t, 0, 0.6) * (1 - out);
    const mo = b.where("mouth") ?? [0, -0.1],
      body = b.where("face") ?? [0, 0];
    const [cone, scoop] = g.marks as [(typeof g.marks)[0], (typeof g.marks)[0]];
    // the cone floats in from the right, to beside the mouth; the licks pull it closer
    const inn = step(t, 0.1, 0.9);
    let lick = 0;
    for (const at of LICKS) lick = Math.max(lick, bump(t, at - 0.2, at + 0.15));
    const cx = 0.45 - 0.12 * lick + 0.8 * (1 - inn),
      cy = mo[1] + 0.05;
    cone.x = cx * WINDOW;
    cone.y = (cy - 0.17) * WINDOW;
    cone.rot = 0.35;
    cone.size = t < GULP ? 0.36 : 0;
    // the scoop: on the cone, a bit smaller each lick; at the gulp it goes in and sinks down
    // inside the body, fading
    let left = 1;
    for (const at of LICKS) if (t > at) left -= 0.2;
    const sink = step(t, GULP, GULP + 0.9);
    scoop.x = (t < GULP ? cx - 0.05 : mo[0] + (body[0] - mo[0]) * sink) * WINDOW;
    scoop.y = (t < GULP ? cy + 0.04 : mo[1] - 0.25 * sink) * WINDOW;
    scoop.size = t < GULP ? 0.12 * left : 0.09 * (1 - step(t, GULP + 0.6, GULP + 1.2));
    f.gaze = 1;
    f.lx = t < GULP ? 0.7 : 0;
    f.ly = t < GULP ? -0.2 : -0.7 * hold(t, GULP, GULP + 1.0, 0.2); // watching it go down
    f.mouth = lick > 0.05 ? 0.55 * lick : t > GULP - 0.15 && t < GULP + 0.12 ? 0.9 : 0;
    f.turn = 0.2 * lick;
    f.squash = 0.06 * bump(t, GULP, GULP + 0.35);
    // a pink glow inside, then a brain-freeze shiver
    g.tint = "#ffb0d8";
    g.tintK = 0.3 * hold(t, GULP + 0.2, 3.3, 0.3);
    g.ox += 0.02 * Math.sin(t * 60) * hold(t, 2.9, 3.25, 0.05);
    f.lid = t > 2.9 && t < 3.25 ? 0.85 : blink(t, 3.6);
    if (b.crossed(0.9)) b.mood("happy"); // licking: its tongue shows
    if (b.crossed(GULP)) {
      b.mood("surprised");
      b.fx.ripple(mo[0], mo[1] - 0.4, 0.35, 0.08);
    }
    if (b.crossed(GULP + 0.5)) b.mood("idle");
    if (b.crossed(3.25)) b.mood("happy");
    if (b.crossed(3.95)) b.mood("idle");
  },
};

export const beats: Beat[] = [boo, eat];
