// Baby dragon: (1) "fire" — walks up, takes a big breath… and only a sad little puff of smoke
// comes out. It squints, tries again with a bigger breath: a ball of fire fills the window
// (the fluid thrown out round it), drifts up and clears; pleased with itself. (2) "eat" — a
// marshmallow floats down; it toasts it with a small flame (it turns golden), then eats it in
// two bites.
import { backOut, blink, bump, exposureAnchor, hold, step, WINDOW, zlerp, type Beat, type BeatCtx } from "./api";

const FIRE = "#ffa040";
const FIRE2 = "#fff0a0";
const FIZZ = 1.05,
  BLAST = 2.15;

/** Flame particles out of the mouth (direction dx, dy in window units / s). */
function flame(b: BeatCtx, t: number, every: number, dx: number, dy: number, spread: number, s = 0.3) {
  if (Math.floor(t / every) === Math.floor((t - b.dt) / every)) return;
  const mo = b.where("mouth") ?? [0, -0.1],
    r = b.rand(Math.floor(t / every)) - 0.5;
  b.fx.emit({
    x: mo[0],
    y: mo[1] - 0.05,
    vx: dx + r * spread,
    vy: dy + r * spread * 0.6,
    g: -1.2,
    life: 0.5,
    strength: s,
    radius: 0.05,
  });
}

const fire: Beat = {
  name: "fire",
  len: 4.4,
  start(b) {
    b.g.marks = [
      { kind: "blot", x: 0, y: 0, size: 0, rot: 0, color: FIRE },
      { kind: "blot", x: 0, y: 0, size: 0, rot: 1, color: FIRE2 },
      exposureAnchor(),
    ];
  },
  frame(b, t) {
    const { g, f } = b,
      L = 4.4,
      out = step(t, 3.6, L - 0.1);
    g.face = step(t, 0, 0.5) * (1 - out);
    g.pin = g.face;
    // in close; a kick back at the blast
    const kick = step(t, BLAST, BLAST + 0.12) * (1 - step(t, BLAST + 0.5, BLAST + 1.1));
    g.zoom =
      t < 3.6
        ? zlerp(1, 1.6, step(t, 0, 0.6)) * (1 - 0.18 * kick)
        : zlerp(1.6, 1, backOut(Math.min(1, (t - 3.6) / (L - 3.7))));
    // breath one (small) and breath two (big): head back, chest up
    const in1 = step(t, 0.55, FIZZ) * (1 - step(t, FIZZ, FIZZ + 0.15)),
      in2 = step(t, 1.6, BLAST - 0.05) * (1 - step(t, BLAST, BLAST + 0.1));
    f.nod = -0.2 * in1 - 0.32 * in2 + 0.12 * bump(t, FIZZ, FIZZ + 0.35) + 0.18 * kick * (t < BLAST + 0.6 ? 1 : 0);
    f.squash = 0.07 * in1 + 0.12 * in2 - 0.08 * bump(t, BLAST, BLAST + 0.3);
    g.sx = 1 + 0.06 * in2;
    f.gaze = 1;
    f.ly = 0.4 * Math.max(in1, in2);
    // the fizzle: a disappointed squint and a look down at itself
    const meh = hold(t, FIZZ + 0.2, 1.6, 0.12);
    f.lid = Math.max(0.4 * meh, 0.55 * hold(t, BLAST - 0.2, BLAST + 0.55, 0.08), blink(t, 3.4));
    f.ly -= 0.6 * meh;
    f.mouth = t > BLAST - 0.05 && t < BLAST + 0.55 ? 0.9 : t > FIZZ - 0.05 && t < FIZZ + 0.25 ? 0.35 : 0;
    // the fireball: bursts out of the mouth, fills the window, rises and thins out
    const mo = b.where("mouth") ?? [0, -0.1];
    const burst = step(t, BLAST, BLAST + 0.25),
      rise = step(t, BLAST + 0.55, 3.35);
    const [ball, core] = g.marks as [(typeof g.marks)[0], (typeof g.marks)[0]];
    ball.size = t < BLAST ? 0 : 1.7 * burst * (1 - step(t, 2.95, 3.4));
    ball.x = mo[0] * WINDOW * (1 - burst);
    ball.y = (mo[1] * (1 - burst) + 1.1 * rise) * WINDOW;
    ball.rot = 1.2 * t;
    core.size = t < BLAST ? 0 : 0.85 * burst * (1 - step(t, 2.55, 2.95));
    core.x = ball.x * 0.8;
    core.y = ball.y - 0.1;
    core.rot = -1.6 * t;
    if (b.crossed(FIZZ)) {
      b.mood("surprised"); // its own nostril smoke puffs
      b.fx.ripple(mo[0], mo[1] - 0.25, 0.25, 0.06);
    }
    if (b.crossed(FIZZ + 0.2)) b.mood("thinking"); // the mouth goes aside: meh
    if (b.crossed(1.6)) b.mood("listening");
    if (t > BLAST && t < BLAST + 0.45) flame(b, t, 0.05, 0, 1.6, 2.6, 0.25);
    if (b.crossed(BLAST)) {
      b.fx.rim(1.0, 6);
      for (let i = 0; i < 6; i++) {
        const a = (i / 6) * Math.PI * 2 + 0.5;
        b.fx.push(0.65 * Math.cos(a), 0.65 * Math.sin(a), 0.5 * Math.cos(a), 0.5 * Math.sin(a), 0.3);
      }
    }
    if (b.crossed(BLAST + 0.7)) b.fx.push(0, 0.4, 0, 0.6, 0.35); // the heat rises
    if (b.crossed(3.1)) b.mood("happy"); // ta-da (its tail spade wiggles)
    if (b.crossed(3.9)) b.mood("idle");
  },
};

const DROP = 0.8,
  TOAST = [1.1, 2.0],
  BITES = [2.55, 3.0];
const MALLOW: Array<[number, number]> = [
  [-0.5, -0.35],
  [-0.38, -0.5],
  [0.38, -0.5],
  [0.5, -0.35],
  [0.5, 0.35],
  [0.38, 0.5],
  [-0.38, 0.5],
  [-0.5, 0.35],
];
const BITTEN: Array<[number, number]> = [
  [-0.5, -0.35],
  [-0.38, -0.5],
  [0.38, -0.5],
  [0.5, -0.35],
  [0.5, 0.05],
  [0.1, 0.3],
  [-0.38, 0.5],
  [-0.5, 0.35],
];
const mix = (a: string, c: string, k: number) => {
  const p = (s: string, i: number) => parseInt(s.slice(1 + 2 * i, 3 + 2 * i), 16);
  let o = "#";
  for (let i = 0; i < 3; i++)
    o += Math.round(p(a, i) + (p(c, i) - p(a, i)) * k)
      .toString(16)
      .padStart(2, "0");
  return o;
};

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
        color: "#ffffff",
        pts: MALLOW,
      },
      { kind: "drop", x: 0, y: 0, size: 0, rot: 0, color: FIRE },
      { kind: "drop", x: 0, y: 0, size: 0, rot: 0, color: FIRE2 },
    ];
  },
  frame(b, t) {
    const { g, f } = b,
      L = 4.2,
      out = step(t, 3.4, L - 0.1);
    g.face = step(t, 0, 0.5) * (1 - out);
    g.pin = g.face;
    g.zoom = zlerp(1, 1.1, step(t, 0, 0.6) * (1 - out));
    g.ox = -0.22 * step(t, 0, 0.6) * (1 - step(t, 2.0, 2.5));
    const mo = b.where("mouth") ?? [0, -0.1];
    // it floats down to the right of the dragon, held there for toasting, then eaten
    const m = g.marks[0]!;
    const hx = 0.5,
      hy = mo[1] + 0.05;
    const down = step(t, 0, DROP),
      toMouth = step(t, 2.25, BITES[0]! - 0.05);
    m.x = (hx + (mo[0] - hx) * toMouth) * WINDOW;
    m.y = (hy + 1.1 * (1 - down) + (mo[1] - 0.02 - hy) * toMouth) * WINDOW;
    m.rot = 0.3 * Math.sin(t * 3) * (1 - toMouth);
    m.size = t > BITES[1]! ? 0 : 0.26;
    m.pts = t > BITES[0]! ? BITTEN : MALLOW;
    const roast = step(t, TOAST[0]!, TOAST[1]!);
    m.color = mix("#ffffff", "#e8963c", roast);
    // a small, careful flame at it
    const blow = hold(t, TOAST[0]!, TOAST[1]!, 0.1);
    if (blow > 0) flame(b, t, 0.08, 1.5, 0.35, 0.4, 0.2);
    // the little flame itself: a flickering tongue of fire from the mouth to the marshmallow
    const fl = g.marks[1]!,
      fc = g.marks[2]!,
      flick = 1 + 0.25 * Math.sin(t * 37) + 0.15 * Math.sin(t * 23);
    fl.x = ((mo[0] + 0.12 + hx) / 2) * WINDOW;
    fl.y = ((mo[1] + hy) / 2 - 0.02) * WINDOW;
    fl.rot = -Math.PI / 2 + 0.12 * Math.sin(t * 19); // the drop's tip points along the flame
    fl.size = 0.32 * blow * flick;
    fc.x = fl.x - 0.03;
    fc.y = fl.y;
    fc.rot = fl.rot;
    fc.size = 0.16 * blow * flick;
    f.gaze = 1;
    f.lx = Math.max(-1, Math.min(1, (m.x / WINDOW - mo[0]) * 2.5));
    f.ly = Math.max(-1, Math.min(1, (m.y / WINDOW - mo[1]) * 1.5));
    f.mouth = blow > 0 ? 0.35 : 0;
    let chomp = 0;
    for (const at of BITES) chomp = Math.max(chomp, bump(t, at - 0.15, at + 0.12));
    if (chomp > 0.05) f.mouth = 0.7 * chomp;
    f.squash = 0.06 * chomp + 0.05 * blow;
    f.turn = 0.25 * blow;
    f.lid = Math.max(0.3 * blow, blink(t, 3.55));
    for (const at of BITES) if (b.crossed(at)) b.fx.ripple(mo[0] + 0.15, mo[1] - 0.35, 0.25, 0.06);
    if (b.crossed(0.5)) b.mood("listening");
    if (b.crossed(BITES[1]! + 0.15)) b.mood("happy");
    if (b.crossed(3.9)) b.mood("idle");
  },
};

export const beats: Beat[] = [fire, eat];
