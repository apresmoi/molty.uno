// Bee: flies a loop round the window leaving a buzzing wake in the fluid, zooms out of the
// window top-right, and comes back in from the left to land in its place.
// "eat": a flower pops up at the window's foot; the bee buzzes over, hovers and dips into it
// three times (the flower nods, pollen puffs up through the fluid), then zips home happy.
import {
  WINDOW,
  blink,
  bump,
  easeIn,
  easeOut,
  exposureAnchor,
  lerp,
  smooth,
  step,
  type Beat,
  type BeatCtx,
} from "./api";

const INK = "#0b0b10";
/** Gives a beat's first `n` marks (its props) a dark rim just behind each, so a prop reads on a
 *  light body and on the dark glass alike, and pins the exposure (a body zoomed up to fill the
 *  window would otherwise expose itself to black). */
function rimmed<B extends Beat>(beat: B, n: number): B {
  return {
    ...beat,
    start(b: BeatCtx) {
      beat.start?.(b);
      const ms = b.g.marks.filter((m) => m.color !== "#000000"); // its own anchor, if any
      b.g.marks = [
        ...ms.slice(0, n),
        ...ms.slice(0, n).map((m) => ({ ...m, color: INK })),
        ...ms.slice(n),
        exposureAnchor(),
      ];
    },
    frame(b: BeatCtx, t: number) {
      beat.frame(b, t);
      const ms = b.g.marks;
      for (let i = 0; i < n; i++) {
        const m = ms[i]!,
          o = ms[n + i]!;
        o.x = m.x;
        o.y = m.y;
        o.rot = m.rot;
        o.d = (m.d ?? 4) - 0.1;
        o.size = m.size > 0 ? m.size * 1.12 + 0.03 : 0;
      }
    },
  };
}

const flyabout: Beat = {
  name: "flyabout",
  len: 4.4,
  frame(b, t) {
    const { g, f } = b;
    let x = 0,
      y = 0,
      vx = 0;
    if (t < 1.7) {
      // take off, then a loop round the window (smaller: farther from the glass)
      const up = step(t, 0, 0.4),
        a = Math.max(0, t - 0.35) * 4.2;
      x = 0.32 * Math.sin(a) * up;
      y = 0.12 * up + 0.16 * Math.sin(a * 2) * up;
      vx = Math.cos(a);
      g.zoom = 1 - 0.3 * step(t, 0, 0.5);
    } else if (t < 2.3) {
      // out, top right
      const q = easeIn((t - 1.7) / 0.6);
      x = 0.32 * Math.sin(1.35 * 4.2) + 1.9 * q;
      y = 0.12 + 0.16 * Math.sin(2.7 * 4.2) + 1.0 * q;
      vx = 1;
      g.zoom = 0.7;
    } else if (t < 2.75) {
      x = 9; // gone
      g.zoom = 0.7;
    } else {
      // back in from the left, a swoop down to its place
      const q = easeOut((t - 2.75) / 1.25);
      x = -1.9 * (1 - q);
      y = 0.45 * Math.sin(Math.PI * q) * (1 - q) + 0.25 * (1 - q);
      vx = 1 - step(t, 3.7, 4.0);
      g.zoom = 0.7 + 0.3 * step(t, 3.3, 4.0);
    }
    g.ox = x;
    g.oy = y;
    g.yaw = 0.9 * Math.max(-1, Math.min(1, vx * 2)); // faces where it flies
    f.lean = -0.2 * vx;
    f.gaze = 1;
    f.lx = 0.6 * vx;
    f.lid = blink(t, 0.9);
    // the wake: a soft ripple left where it was, every ~0.08 s, while it is in the window
    const w = b.where("body");
    if (
      w &&
      Math.abs(w[0]) < 1.1 &&
      Math.abs(w[1]) < 1.1 &&
      t > 0.35 &&
      Math.floor(t / 0.08) !== Math.floor((t - b.dt) / 0.08)
    )
      b.fx.ripple(w[0], w[1] - 0.12, 0.12, 0.05);
    if (b.crossed(4.0)) b.fx.ripple(0, -0.95, 0.4, 0.14); // touchdown
    if (b.crossed(0.4)) b.mood("happy");
    if (b.crossed(4.1)) b.mood("idle");
  },
};

// ---------------------------------------------------------------- eat: nectar

const PETALS = 5;
const DISC: Array<[number, number]> = Array.from({ length: 8 }, (_, i) => {
  const a = (i / 8) * Math.PI * 2;
  return [Math.cos(a), Math.sin(a)] as [number, number];
});
const SIPS = [1.45, 1.9, 2.35];
const TRAVEL = 0.65,
  AT = 1.2,
  LEAVE = 2.75,
  HOMEAT = 3.6;

const eat: Beat & { label: string } = {
  name: "eat",
  label: "Sips nectar",
  len: 4.2,
  start(b) {
    b.g.marks = [
      ...Array.from({ length: PETALS }, () => ({
        kind: "poly" as const,
        x: 0,
        y: -2,
        size: 0,
        rot: 0,
        color: "#ffffff",
        pts: DISC,
      })),
      { kind: "poly", x: 0, y: -2, size: 0, rot: 0, color: "#ffc84a", pts: DISC },
    ];
    b.state.side = b.rand(2) < 0.5 ? -1 : 1;
    b.mood("idle");
  },
  frame(b, t) {
    const { g, f, state: s } = b,
      L = 4.2,
      side = s.side!;
    // the flower: pops up at the foot of the window, to one side; nods when sipped
    const grow = step(t, 0, 0.45) * (1 - step(t, 3.5, 4.0));
    let sip = 0;
    for (const at of SIPS) sip = Math.max(sip, bump(t, at - 0.15, at + 0.2));
    const cx = 0.42 * side,
      cy = -0.55 - 0.05 * sip;
    for (let i = 0; i < PETALS; i++) {
      const a = (i / PETALS) * Math.PI * 2 + 0.3 + 0.15 * sip,
        p = g.marks[i]!;
      p.x = (cx + 0.14 * Math.cos(a) * grow) * WINDOW;
      p.y = (cy + 0.14 * Math.sin(a) * grow) * WINDOW;
      p.size = 0.07 * grow;
    }
    const c = g.marks[PETALS]!;
    c.x = cx * WINDOW;
    c.y = cy * WINDOW;
    c.size = 0.065 * grow;
    // the bee: flies over so its mouth hangs just above the flower, dips to sip, flies home
    const m = b.where("mouth"),
      ox0 = s.ox ?? 0,
      oy0 = s.oy ?? 0;
    if (m && t < AT) {
      // where it would be with no offset (window units), measured while it travels
      s.mx = m[0] - ox0 / WINDOW;
      s.my = m[1] - oy0 / WINDOW;
    }
    const zoom = lerp(1, 0.8, step(t, 0.3, 0.9)) * (1 - step(t, LEAVE, HOMEAT)) + step(t, LEAVE, HOMEAT);
    const tx = (cx - (s.mx ?? 0) * 0.8) * WINDOW,
      ty = (cy + 0.38 - (s.my ?? 0) * 0.8) * WINDOW; // its mouth just over the flower: the bee above it, not on it
    const go = smooth((t - TRAVEL) / (AT - TRAVEL)),
      back = smooth((t - LEAVE) / (HOMEAT - LEAVE));
    const dip = 0.1 * sip;
    g.zoom = zoom;
    g.ox = lerp(0, tx, go) * (1 - back) + 0.05 * Math.sin(t * 11) * (1 - sip) * go * (1 - back);
    g.oy = (lerp(0, ty, go) - dip) * (1 - back) + 0.15 * bump(t, LEAVE, HOMEAT);
    s.ox = g.ox;
    s.oy = g.oy;
    const vx = t < AT ? side * bump(t, TRAVEL, AT) : t > LEAVE ? -side * bump(t, LEAVE, HOMEAT) : 0;
    g.yaw = 0.8 * vx;
    f.lean = -0.2 * vx;
    f.gaze = 1;
    f.lx =
      0.6 * side * step(t, 0.3, 0.5) * (1 - step(t, AT, AT + 0.2)) -
      0.6 * side * (t > LEAVE ? bump(t, LEAVE, HOMEAT) : 0);
    f.ly = -0.6 * step(t, 0.3, 0.6) * (1 - step(t, LEAVE, LEAVE + 0.2));
    f.lid = sip > 0.5 ? 0.85 : blink(t, 3.8);
    f.mouth = 0;
    for (const [i, at] of SIPS.entries())
      if (b.crossed(at)) {
        // a puff of pollen up out of the flower
        for (let j = 0; j < 2; j++) {
          const r = b.rand(i * 2 + j) - 0.5;
          b.fx.emit({ x: cx + r * 0.3, y: cy + 0.05, vx: r * 0.8, vy: 0.7, life: 0.5, strength: 0.12, radius: 0.04 });
        }
      }
    if (b.crossed(0.3)) b.fx.ripple(cx, cy - 0.15, 0.35, 0.1); // the flower pops up
    if (b.crossed(LEAVE)) b.mood("happy");
    if (b.crossed(L - 0.3)) b.mood("idle");
  },
};

export const beats: Beat[] = [flyabout, rimmed(eat, PETALS + 1)];
