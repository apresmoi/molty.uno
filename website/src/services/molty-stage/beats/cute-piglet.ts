// Piglet: pushes its big snout flat against the glass, snorts twice (nostrils flare, two
// puffs into the fluid), eyes up at you over it, then peels off.
// "eat": an apple drops and bounces; the piglet dives on it snout-first and chomps it down in
// messy bites (bits flying everywhere, tail spinning), comes up with a bit stuck on its
// snout and snorts it off.
import {
  WINDOW,
  backOut,
  blink,
  bump,
  exposureAnchor,
  lerp,
  smooth,
  step,
  zlerp,
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

const PRESS = 0.2;

const snout: Beat = {
  name: "snout",
  len: 3.5,
  frame(b, t) {
    const { g, f } = b,
      HIT = 0.9,
      OFF = 2.6,
      L = 3.5,
      ZOOM = 2.7;
    const out = smooth((t - OFF) / (L - OFF - 0.1));
    g.face = step(t, 0, HIT) * (1 - out);
    g.pin = g.face;
    g.focusAt = "snout";
    g.focus = step(t, 0.2, HIT) * (1 - out);
    let p = 0;
    if (t < HIT) {
      const x = step(t, 0.15, HIT);
      g.zoom = lerp(1, ZOOM, x * x);
      p = PRESS * Math.max(0, (x - 0.85) / 0.15);
    } else if (t < OFF) {
      const h = t - HIT,
        k = Math.exp(-h * 7) * Math.cos(h * 18);
      // pressed in; each snort pushes it in a little harder
      p = PRESS * (1 + 0.5 * k + 0.35 * (bump(t, 1.35, 1.55) + bump(t, 1.85, 2.05)));
      g.zoom = ZOOM * (1 + 0.03 * k);
      g.sx = 1.1;
      g.sy = 0.95;
    } else {
      const x = Math.min(1, (t - OFF) / (L - OFF));
      p = PRESS * (1 - smooth(x / 0.2));
      g.zoom = lerp(ZOOM, 1, backOut(x));
      g.sx = lerp(1.1, 1, smooth(x / 0.3));
      g.sy = lerp(0.95, 1, smooth(x / 0.3));
    }
    g.press = p;
    f.still = (0.8 * p) / PRESS;
    f.gaze = 1;
    f.ly = 0.55 * step(t, 1.0, 1.3) * (1 - out);
    f.lid = blink(t, 2.25);
    f.mouth = 0;
    for (const at of [1.35, 1.85])
      if (b.crossed(at)) {
        // a snort out of each nostril, just below the snout
        const s = b.where("snout") ?? [0, -0.2];
        b.fx.ripple(s[0] - 0.35, s[1] - 0.55, 0.45, 0.12);
        b.fx.ripple(s[0] + 0.35, s[1] - 0.55, 0.45, 0.12);
      }
    if (b.crossed(HIT)) b.fx.rim(0.6);
    if (b.crossed(HIT + 0.2)) b.mood("idle");
  },
  post(b, t) {
    // the nostrils flare on each snort
    const k = 1 + 0.5 * (bump(t, 1.3, 1.55) + bump(t, 1.8, 2.05));
    for (const m of b.part("nostril")) m.scale.set(0.75 * k, 1.15 * k, 1);
  },
};

// ---------------------------------------------------------------- eat: an apple

const APPLE: Array<[number, number]> = Array.from({ length: 9 }, (_, i) => {
  const a = (i / 9) * Math.PI * 2;
  return [Math.cos(a) * (1 - 0.12 * Math.max(0, Math.sin(a))), Math.sin(a) * 0.9] as [number, number];
});
const APPLE_LEAF: Array<[number, number]> = [
  [0, 0],
  [0.6, 0.5],
  [0.15, 1],
  [-0.25, 0.55],
];
const CHOMPS = [1.25, 1.6, 1.95, 2.3];

const eat: Beat & { label: string } = {
  name: "eat",
  label: "Messy apple",
  len: 4.2,
  start(b) {
    b.g.marks = [
      { kind: "poly", x: 0, y: 2, size: 0, rot: 0, color: "#c8f293", pts: APPLE },
      { kind: "poly", x: 0, y: 2, size: 0, rot: 0, color: "#8fe070", pts: APPLE_LEAF },
      { kind: "blot", x: 0, y: 0, size: 0, rot: 0.5, color: "#e4f9c4" },
    ];
    b.mood("idle");
  },
  frame(b, t) {
    const { g, f } = b,
      L = 4.2;
    const out = step(t, 3.45, L - 0.1);
    g.face = 0.7 * step(t, 0, 0.4) * (1 - out);
    g.zoom =
      t < 3.45 ? zlerp(1, 1.25, step(t, 0.6, 1.1)) : zlerp(1.25, 1, backOut(Math.min(1, (t - 3.45) / (L - 3.55))));
    g.oy = 0.1 * step(t, 0, 0.5) * (1 - out);
    const sn = b.where("snout") ?? [0, -0.15];
    // the apple: drops, bounces, then sits under the snout while it is eaten
    const FLOOR = -0.62;
    let ay: number;
    if (t < 0.45) ay = lerp(1.2, FLOOR, (t / 0.45) ** 2);
    else if (t < 0.75) ay = FLOOR + 0.18 * Math.sin((Math.PI * (t - 0.45)) / 0.3);
    else ay = lerp(FLOOR, Math.max(FLOOR, sn[1] - 0.2), step(t, 0.95, 1.2));
    let eaten = 0;
    for (const at of CHOMPS) eaten += step(t, at, at + 0.08);
    const sz = 0.17 * (1 - eaten / CHOMPS.length) * step(t, 0, 0.1);
    const apple = g.marks[0]!,
      leaf = g.marks[1]!;
    apple.x = 0.05 * WINDOW;
    apple.y = ay * WINDOW;
    apple.size = sz;
    apple.rot = 0.4 * Math.sin(t * 13) * (eaten > 0 && eaten < CHOMPS.length ? 1 : 0);
    leaf.x = apple.x + 0.02;
    leaf.y = apple.y + sz * 0.8;
    leaf.size = sz * 0.55;
    leaf.rot = apple.rot;
    // diving in snout-first, chomping (happy: the tail spins), up again
    const dive = step(t, 0.8, 1.15) * (1 - step(t, 2.5, 2.8));
    let chomp = 0;
    for (const at of CHOMPS) chomp = Math.max(chomp, bump(t, at - 0.1, at + 0.12));
    f.nod = 1.2 * dive + 0.5 * chomp;
    f.pitch = 0.22 * dive;
    f.mouth = 0;
    f.gaze = 1;
    f.ly = -0.6 * step(t, 0.2, 0.5) * (1 - step(t, 2.6, 2.8));
    f.lid = blink(t, 3.3);
    for (const [i, at] of CHOMPS.entries())
      if (b.crossed(at)) {
        for (let j = 0; j < 3; j++) {
          // bits fly everywhere
          const r = b.rand(i * 3 + j) - 0.5;
          b.fx.emit({
            x: apple.x / WINDOW,
            y: ay + 0.05,
            vx: r * 2.4,
            vy: 0.8 + Math.abs(r),
            g: 6,
            life: 0.5,
            strength: 0.16,
            radius: 0.045,
          });
        }
        b.fx.ripple(0, -0.95, 0.3, 0.12);
      }
    // a bit stuck on its snout, snorted off
    const bit = g.marks[2]!;
    bit.x = (sn[0] + 0.1) * WINDOW;
    bit.y = (sn[1] + 0.06) * WINDOW;
    bit.size = 0.09 * step(t, 2.5, 2.6) * (1 - step(t, 3.2, 3.25));
    if (b.crossed(3.2)) {
      b.fx.ripple(sn[0] - 0.3, sn[1] - 0.4, 0.4, 0.1);
      b.fx.ripple(sn[0] + 0.3, sn[1] - 0.4, 0.4, 0.1);
      b.fx.emit({ x: sn[0] + 0.1, y: sn[1], vx: 1.2, vy: 0.6, g: 5, life: 0.5, strength: 0.15, radius: 0.05 });
    }
    if (b.crossed(1.05)) b.mood("happy");
    if (b.crossed(2.6)) b.mood("idle");
    if (b.crossed(3.35)) b.mood("happy");
    if (b.crossed(3.9)) b.mood("idle");
  },
  post(b, t) {
    // the snort: nostrils flare
    const k = 1 + 0.6 * bump(t, 3.1, 3.35);
    for (const m of b.part("nostril")) m.scale.set(m.scale.x * k, m.scale.y * k, 1);
  },
};

export const beats: Beat[] = [snout, rimmed(eat, 3)];
