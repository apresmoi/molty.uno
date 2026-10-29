// Bunny: a binky. Crouch, a leap with a full twist in the air (a kick of the hind end),
// a thumping landing in the fluid, a smaller hop, then sits up proud with its ears up.
// "eat": holds up a carrot and nibbles it down bite by bite (nose twitching, crumbs into the
// fluid), pops the leafy top in last, chews, happy.
import { WINDOW, backOut, blink, bump, exposureAnchor, smooth, step, zlerp, type Beat, type BeatCtx } from "./api";

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

const binky: Beat = {
  name: "binky",
  len: 3.6,
  frame(b, t) {
    const { g, f } = b;
    // the crouch
    f.squash = -0.14 * step(t, 0, 0.4) * (1 - step(t, 0.42, 0.5));
    f.gaze = 1;
    f.ly = 0.4 * step(t, 0, 0.4) * (1 - step(t, 1.4, 1.6));
    // the leap: up, a full turn and a kick
    // (a full turn needs ~1 s: faster, the fluid smears it into a blob)
    const x = Math.min(1, Math.max(0, (t - 0.45) / 1.05));
    if (t > 0.45 && t < 1.5) {
      const h = 4 * x * (1 - x);
      f.bob = 0.42 * h;
      f.squash = 0.1 * h;
      f.pitch = 0.35 * Math.sin(Math.PI * x);
      f.lean = 0.25 * Math.sin(2 * Math.PI * x);
    }
    g.yaw = Math.PI * 2 * smooth(x);
    g.zoom = 1 - 0.14 * bump(t, 0.4, 1.55);
    // the landing squash, then a little hop with a half twist back and forth
    f.squash += -0.16 * bump(t, 1.5, 1.7) - 0.1 * bump(t, 2.25, 2.42);
    const y = Math.min(1, Math.max(0, (t - 1.8) / 0.45));
    if (t > 1.8 && t < 2.25) f.bob = 0.12 * 4 * y * (1 - y);
    g.yaw += -0.7 * Math.sin(Math.PI * y);
    // sitting up proud
    f.tilt = 0.18;
    f.tiltW = step(t, 2.5, 2.7) * (1 - step(t, 3.3, 3.5));
    f.lid = blink(t, 2.9);
    if (b.crossed(1.5)) {
      b.fx.ripple(0, -0.95, 0.9, 0.18); // thump
      b.fx.rim(0.35);
    }
    if (b.crossed(2.25)) b.fx.ripple(0, -0.95, 0.45, 0.14);
    if (b.crossed(2.4)) b.mood("happy");
    if (b.crossed(3.2)) b.mood("idle");
  },
};

// ---------------------------------------------------------------- eat: a carrot

const BITES = [0.75, 1.05, 1.35, 1.65, 1.95, 2.25];
const LEAVES = 2.7;
const LEAF: Array<[number, number]> = [
  [0, 0.1],
  [-0.8, -0.9],
  [-0.25, -0.6],
  [0, -1.1],
  [0.25, -0.6],
  [0.8, -0.9],
];

const eat: Beat & { label: string } = {
  name: "eat",
  label: "Nibbles a carrot",
  len: 4.0,
  start(b) {
    // (outlines counter-clockwise: a clockwise fan draws black) the carrot is redrawn each frame shorter (its top bitten off): its own outline array
    const carrot: Array<[number, number]> = [
      [-0.36, -1],
      [0.36, -1],
      [0.05, 1],
      [-0.05, 1],
    ];
    b.g.marks = [
      { kind: "poly", x: 0, y: -2, size: 0, rot: 0, color: "#ffad5c", pts: carrot },
      { kind: "poly", x: 0, y: -2, size: 0, rot: 0, color: "#a8ec86", pts: LEAF },
    ];
    b.mood("idle");
  },
  frame(b, t) {
    const { g, f } = b,
      L = 4.0;
    const out = step(t, 3.3, L - 0.1);
    g.face = 0.8 * step(t, 0, 0.4) * (1 - out);
    g.pin = g.face;
    g.zoom = t < 3.3 ? zlerp(1, 1.35, step(t, 0, 0.5)) : zlerp(1.35, 1, backOut(Math.min(1, (t - 3.3) / (L - 3.4))));
    const m = b.where("mouth") ?? [0, -0.2];
    // bitten: the carrot's top (y = h in its own units) comes down a step per bite
    let bitten = 0;
    for (const at of BITES) bitten += step(t, at, at + 0.06);
    const h = 1 - (1.85 * bitten) / BITES.length,
      w = 0.05 + 0.31 * ((1 - h) / 2);
    const carrot = g.marks[0]!,
      leaf = g.marks[1]!,
      pts = carrot.pts as Array<[number, number]>;
    pts[2]![0] = w;
    pts[2]![1] = h;
    pts[3]![0] = -w;
    pts[3]![1] = h;
    // held up so its bitten end stays at the mouth; the leaves go in last
    const size = 0.3 * step(t, 0, 0.35);
    const lift = step(t, 0, 0.45),
      gone = step(t, LEAVES - 0.1, LEAVES + 0.08);
    carrot.size = h > -0.83 ? size : 0;
    carrot.x = m[0] * WINDOW;
    carrot.y = (m[1] - 0.02) * WINDOW - size * h - (1 - lift) * 0.6;
    carrot.rot = 0.04 * Math.sin(t * 20) * (bitten < BITES.length ? 1 : 0);
    leaf.size = 0.17 * step(t, 0, 0.35) * (1 - gone);
    leaf.x = carrot.x;
    leaf.y = (carrot.y - size) * (1 - gone) + m[1] * WINDOW * gone;
    // the nibbles: a little nod and chomp on each bite, eyes on the carrot, then chewing
    let chomp = 0;
    for (const at of [...BITES, LEAVES]) chomp = Math.max(chomp, bump(t, at - 0.08, at + 0.12));
    f.nod = 0.25 * chomp;
    f.mouth = Math.max(0.4 * chomp, t > LEAVES + 0.1 && t < 3.25 ? 0.2 * Math.max(0, Math.sin(t * 18)) : 0);
    f.gaze = 1;
    f.ly = -0.5 * (1 - step(t, LEAVES, LEAVES + 0.2));
    f.lid = blink(t, 2.95);
    f.squash = 0.04 * bump(t, LEAVES, LEAVES + 0.3);
    for (const [i, at] of BITES.entries())
      if (b.crossed(at)) {
        const r = b.rand(i) - 0.5;
        b.fx.emit({
          x: m[0] + r * 0.2,
          y: m[1] - 0.05,
          vx: r * 1.2,
          vy: 0.3,
          g: 5,
          life: 0.5,
          strength: 0.13,
          radius: 0.04,
        }); // a crumb
      }
    if (b.crossed(LEAVES + 0.4)) b.mood("happy");
    if (b.crossed(3.5)) b.mood("idle");
  },
  post(b, t) {
    // both paws up holding it; the nose twitches through the nibbles
    const hold = step(t, 0.1, 0.4) * (1 - step(t, LEAVES, LEAVES + 0.3));
    for (const r of b.kit.sock.side ?? []) {
      const g = b.node(r.g);
      g.rotation.x -= 0.9 * hold;
      g.rotation.z -= r.side * 0.35 * hold;
    }
    const nib = step(t, 0.6, 0.7) * (1 - step(t, 2.5, 2.6));
    for (const m of b.part("nose")) m.position.y += 0.007 * Math.sin(t * 60) * nib;
  },
};

export const beats: Beat[] = [binky, rimmed(eat, 2)];
