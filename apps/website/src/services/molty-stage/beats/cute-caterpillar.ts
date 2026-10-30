// Caterpillar: (1) "inch" — an inchworm's walk: the tail bunches up under a hump that runs
// along its back, then the head stretches forward; twice across, a U-turn, twice back, a
// U-turn home (a little footprint ripple each step). (2) "eat" — a big leaf stands beside
// it and it chomps its way into it, bite after bite, the edge scalloping away.
import { blink, bump, clamp01, hold, smooth, step, type Beat } from "./api";

const CYCLE = 0.62; // s per inch
const ADV = 0.17; // fit units per inch
// [start, direction] of each inch; U-turns in between
const INCHES: Array<[number, number]> = [
  [0.15, 1],
  [0.15 + CYCLE, 1],
  [2.0, -1],
  [2.0 + CYCLE, -1],
];
// the turnarounds: a quick cartoon flip (mirrored on screen about its middle), so it keeps
// facing out of the window both ways (a real U-turn shows the back of its head)
const TURN1 = [1.55, 1.95],
  TURN2 = [3.6, 4.0];

/** Phase of the inch running at `t` (0…1), or -1. */
const inchAt = (t: number) => {
  for (const [a] of INCHES) if (t >= a && t < a + CYCLE) return (t - a) / CYCLE;
  return -1;
};

/** How far it has gone (fit units, screen x). */
const travel = (t: number) => {
  let x = 0;
  for (const [a, d] of INCHES) x += d * ADV * smooth(clamp01((t - a - CYCLE * 0.5) / (CYCLE * 0.5)));
  return x;
};

const inch: Beat = {
  name: "inch",
  len: 4.2,
  frame(b, t) {
    const { g, f } = b;
    g.ox = travel(t);
    g.zoom = 1 - 0.08 * hold(t, 0, 4.4, 0.3);
    g.focusAt = "segment1";
    g.focus = 1; // flip about its middle (no pin: moving the anchor alone moves nothing)
    g.sx = 1 - 2 * step(t, TURN1[0]!, TURN1[1]!) + 2 * step(t, TURN2[0]!, TURN2[1]!);
    f.squash = 0.08 * (bump(t, TURN1[0]!, TURN1[1]!) + bump(t, TURN2[0]!, TURN2[1]!));
    // mirrored, its triangles wind the other way and the renderer shades them darker: lift it
    g.tint = "#e4fff2";
    g.tintK = 0.45 * clamp01((1 - g.sx) / 2);
    f.still = 0.7;
    f.gaze = 1;
    f.lx = 0.5;
    f.lid = blink(t, 1.8);
    const p = inchAt(t);
    // the head: down while the tail comes up, lifting as it stretches out
    f.nod = p < 0 ? 0 : 0.15 * Math.sin(Math.PI * Math.min(1, p * 2)) - 0.12 * bump(p, 0.5, 1);
    for (const [a] of INCHES) if (b.crossed(a + CYCLE * 0.95)) b.fx.ripple(g.ox / 0.85 + 0.2 * g.sx, -0.72, 0.2, 0.07);
    if (b.crossed(3.95)) b.mood("happy");
    if (b.crossed(4.15)) b.mood("idle");
  },
  post(b, t) {
    const p = inchAt(t);
    if (p < 0) return;
    // bunch (first half): the rear comes forward under a hump; stretch (second half): back out
    const bunch = p < 0.5 ? smooth(p * 2) : 1 - smooth((p - 0.5) * 2);
    for (let i = 0; i < 4; i++) {
      const m = b.part(`segment${i}`)[0],
        toe = b.part(`toes${i}`)[0];
      if (!m) continue;
      const k = i / 3; // 0 at the front, 1 at the tail
      // the rear moves toward the head; in the stretch the glass offset carries the head
      // end forward while the rear unbunches, so the rear stays put on the floor
      const dx = 0.17 * bunch * k;
      const lift = 0.3 * bunch * Math.sin(Math.PI * Math.min(1, (i + 0.5) / 3.2));
      m.position.x += dx;
      m.position.y += lift;
      if (toe) {
        // the kit re-sets only the toes' y each frame: x from the rest pose, not +=
        const rest = (toe as unknown as { userData: { basePos: { x: number } } }).userData.basePos;
        toe.position.x = rest.x + dx;
        toe.position.y += lift * 0.8;
      }
    }
  },
};

// ---------------------------------------------------------------- eat: a big leaf
const LEAF = "#9fe86e"; // a deeper green than its mint body
const STRIPS: Array<Array<[number, number]>> = [];
{
  // a tall leaf (point up), cut into strips across its width, left (nearest it) to right
  const ys = (x: number) => 0.5 * Math.sqrt(Math.max(0, 1 - (x / 0.3) ** 2)) + 0.004;
  const xs = [-0.3, -0.18, -0.06, 0.06, 0.18, 0.3];
  for (let i = 0; i + 1 < xs.length; i++) {
    const a = xs[i]!,
      c = xs[i + 1]!;
    STRIPS.push([
      [a, -ys(a)],
      [c, -ys(c)],
      [c, ys(c)],
      [a, ys(a)],
    ]);
  }
}
const STEM: Array<[number, number]> = [
  [-0.02, -0.5],
  [0.02, -0.5],
  [0.02, -0.75],
  [-0.02, -0.75],
];
const BITES = [0.9, 1.4, 1.9, 2.4, 2.9];
const LX = 0.52,
  LY = -0.08,
  LS = 0.85;

const eat: Beat = {
  name: "eat",
  len: 4.2,
  start(b) {
    b.g.marks = [
      ...STRIPS.map((pts) => ({ kind: "poly" as const, x: LX, y: LY, size: 0, rot: 0.08, color: LEAF, pts })),
      { kind: "poly", x: LX, y: LY, size: 0, rot: 0.08, color: LEAF, pts: STEM },
    ];
  },
  frame(b, t) {
    const { g, f } = b;
    g.face = 0.5 * hold(t, 0, 4.1, 0.4);
    g.ox = -0.25 * hold(t, 0, 4.1, 0.4);
    const grow = step(t, 0, 0.5);
    let eaten = 0;
    for (let i = 0; i < STRIPS.length; i++) {
      const m = g.marks[i]!;
      m.size = (i < BITES.length && t >= BITES[i]! ? 0 : LS) * grow;
      // the last strip (the far edge) stays, and the stem: the leaf's skeleton
      if (i < BITES.length && t >= BITES[i]!) eaten++;
    }
    // keep the far edge (the last bite only nibbles to it)
    g.marks[STRIPS.length - 1]!.size = LS * grow * (1 - step(t, 3.6, 4.0));
    g.marks[STRIPS.length]!.size = LS * grow * (1 - step(t, 3.6, 4.0));
    if (t >= BITES[4]!) g.marks[STRIPS.length - 1]!.size = LS * grow * (1 - step(t, 3.6, 4.0));
    let chomp = 0;
    for (const at of BITES) chomp = Math.max(chomp, bump(t, at - 0.25, at + 0.12));
    f.mouth = chomp > 0.05 ? 0.8 * chomp : 0;
    f.nod = 0.12 * chomp;
    // it leans further in as the edge recedes
    f.px = 0.03 * eaten;
    f.pitch = 0.1 * chomp;
    f.gaze = 1;
    f.lx = t < 3.1 ? 0.7 : 0;
    f.lid = blink(t, 3.3);
    f.squash = 0.06 * bump(t, 3.2, 3.6); // full
    for (const at of BITES)
      if (b.crossed(at)) {
        const x = (LX + (-0.24 + 0.12 * BITES.indexOf(at)) * LS) / 0.85;
        b.fx.emit({ x, y: 0.1, vx: 0.15, vy: 0.2, g: 4, life: 0.5, strength: 0.15, radius: 0.04 });
      }
    if (b.crossed(3.2)) b.mood("happy");
    if (b.crossed(4.0)) b.mood("idle");
  },
};

export const beats: Beat[] = [inch, eat];
