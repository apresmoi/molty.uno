// Duck: (1) "dabble" — water rises in the window (a bright surface line on top), the duck
// turns side-on and tips bottoms-up: head under (bubbles coming up), tail high and wiggling,
// big orange feet kicking in the air; it pops back up throwing water and shakes it off.
// (2) "eat" — bread crumbs rain down in front of it and it pecks them up one by one, then a
// happy quack.
import { backOut, blink, bump, exposureAnchor, hold, step, type Beat } from "./api";

/** Uniform scale (the rig types expose only `set`). */
const sc = (o: { scale: { set(x: number, y: number, z: number): unknown } }, k: number) => o.scale.set(k, k, k);

const WATER = "#a9dcff";
const FOAM = "#ffffff";
const TIP = 2.3; // rad: bottoms up, tail high
const LINE = -0.22; // the water's surface (fit units) once risen
// the water: a band below `y` (fit units), drawn on the glass in front of the duck
const BAND: Array<[number, number]> = [
  [-1.3, -1.4],
  [1.3, -1.4],
  [1.3, 0],
  [-1.3, 0],
];
const SURF: Array<[number, number]> = [
  [-1.3, -0.035],
  [1.3, -0.035],
  [1.3, 0.035],
  [-1.3, 0.035],
];
const FEET = "#ffc46b"; // light orange: the rig's own orange renders too dim here
// a webbed foot, heel at the origin, toes up
const WEB: Array<[number, number]> = [
  [0, 0],
  [-0.42, 0.62],
  [-0.15, 0.78],
  [0.15, 0.78],
  [0.42, 0.62],
];
const RING: Array<[number, number]> = [];
for (let i = 0; i < 8; i++) RING.push([0.5 * Math.cos((i / 8) * Math.PI * 2), 0.5 * Math.sin((i / 8) * Math.PI * 2)]);
// bubbles from under the water while it dabbles: [start, x offset from the bill]
const BUBBLES: Array<[number, number]> = [
  [1.35, 0],
  [1.6, 0.08],
  [1.85, -0.05],
  [2.05, 0.05],
];

const dabble: Beat = {
  name: "dabble",
  len: 3.8,
  start(b) {
    b.g.marks = [
      { kind: "poly", x: 0, y: -2, size: 1, rot: 0, color: WATER, pts: BAND },
      { kind: "poly", x: 0, y: -2, size: 1, rot: 0, color: FOAM, pts: SURF },
      ...BUBBLES.map(() => ({ kind: "poly" as const, x: 0, y: 0, size: 0, rot: 0, color: FOAM, pts: RING })),
      // the feet, drawn big over the rig's own (too small to read upside down)
      { kind: "poly", x: 0, y: 0, size: 0, rot: 0, color: FEET, pts: WEB },
      { kind: "poly", x: 0, y: 0, size: 0, rot: 0, color: FEET, pts: WEB },
      exposureAnchor(),
    ];
    b.mood("idle");
  },
  frame(b, t) {
    const { g, f } = b,
      L = 3.8;
    const water = step(t, 0, 0.5) * (1 - step(t, 3.15, L - 0.05));
    const y = -2 + (2 + LINE) * water + 0.02 * Math.sin(t * 4);
    g.marks[0]!.y = y;
    g.marks[1]!.y = y;
    g.zoom = 1 - 0.15 * hold(t, 0.1, 3.4, 0.4); // back a little: the whole tipped duck fits
    g.oy = 0.06 * hold(t, 0.1, 3.4, 0.4);
    g.yaw = 1.0 * step(t, 0.1, 0.6) * (1 - step(t, 3.1, L - 0.1)); // side-on, facing right
    // tip about the feet, then pop back up; keep the body's middle near the waterline
    const down = step(t, 0.9, 1.3),
      up = t < 2.4 ? 0 : backOut(Math.min(1, (t - 2.4) / 0.4));
    const th = TIP * down * (1 - up);
    f.pitch = th;
    f.bob = 0.45 * (1 - Math.cos(th)) + 0.03 * Math.sin(t * 5) * (1 - down);
    g.ox = -0.2 * (1 - Math.cos(th));
    // bottoms up: the tail wiggles
    f.lean = 0.25 * Math.sin(t * 15) * step(t, 1.3, 1.45) * (1 - step(t, 2.25, 2.4));
    // up again: a head shake, a blink
    f.turn = 0.45 * Math.sin((t - 2.85) * Math.PI * 2 * 4) * bump(t, 2.85, 3.3);
    f.lid = blink(t, 3.35);
    f.still = 0.6;
    const bill = b.where("bill") ?? [0.3, -0.3];
    // bubbles: rise from below the bill to the surface and pop
    for (let i = 0; i < BUBBLES.length; i++) {
      const [at, dx] = BUBBLES[i]!,
        m = g.marks[2 + i]!,
        q = (t - at) / 0.45;
      m.size = q < 0 || q > 1 ? 0 : 0.07 + 0.03 * (i % 2);
      m.x = bill[0] * 0.85 + 0.12 + dx;
      m.y = LINE - 0.3 + 0.28 * q;
      if (b.crossed(at + 0.45)) b.fx.ripple(m.x / 0.85, LINE / 0.85, 0.2, 0.05);
    }
    // the feet kick in the air over its upturned bottom
    const kick = step(t, 1.25, 1.45) * (1 - step(t, 2.3, 2.45)),
      ft = b.where("foot");
    for (let i = 0; i < 2; i++) {
      const m = g.marks[2 + BUBBLES.length + i]!;
      m.size = ft ? 0.38 * kick : 0;
      if (!ft) continue;
      m.x = ft[0] * 0.85 + (i ? 0.13 : -0.13);
      m.y = ft[1] * 0.85 + 0.08; // sticking up out of its upturned bottom
      m.rot = (i ? -0.25 : 0.25) + 0.45 * Math.sin(t * 16 + i * Math.PI);
    }
    if (b.crossed(0.5)) {
      b.fx.ripple(-0.55, LINE / 0.85, 0.3, 0.12); // the water settles round it
      b.fx.ripple(0.55, LINE / 0.85, 0.3, 0.12);
    }
    if (b.crossed(1.12)) b.fx.ripple(bill[0] + 0.15, LINE / 0.85, 0.8, 0.14); // head under: plop
    if (b.crossed(2.55)) {
      // up it comes, throwing water
      for (let i = 0; i < 6; i++) {
        const a = Math.PI * (0.15 + 0.7 * (i / 5));
        b.fx.emit({
          x: bill[0],
          y: LINE / 0.85 + 0.05,
          vx: 1.4 * Math.cos(a),
          vy: 1.6 * Math.sin(a),
          g: 5,
          life: 0.5,
          strength: 0.18,
          radius: 0.05,
        });
      }
    }
    if (b.crossed(2.9)) b.mood("happy");
    if (b.crossed(3.35)) b.mood("idle");
  },
  post(b, t) {
    // bottoms up: big feet paddling in the air, the tail cocked up (the kit re-poses sockets)
    const up = step(t, 1.15, 1.4) * (1 - step(t, 2.3, 2.45));
    if (up > 0) {
      for (const [i, g] of b.socket("foot").entries()) {
        g.rotation.x += 1.1 * up * Math.sin(t * 18 + i * Math.PI);
        sc(g, 1 + 0.6 * up);
      }
      for (const g of b.socket("tail")) {
        g.rotation.x -= 0.6 * up;
        sc(g, 1 + 0.5 * up);
      }
    }
  },
};

// ---------------------------------------------------------------- eat: bread crumbs
const BREAD = "#fff0c8";
const CRUMB: Array<[number, number]> = [
  [-0.5, -0.4],
  [0.45, -0.5],
  [0.5, 0.35],
  [-0.1, 0.5],
  [-0.5, 0.2],
];
// crumbs: [x on the floor (fit), lands at, pecked at]
const CRUMBS: Array<[number, number, number]> = [
  [0.12, 0.45, 1.05],
  [-0.22, 0.6, 1.55],
  [0.38, 0.75, 2.05],
  [-0.05, 0.9, 2.55],
];
const FLOOR = -0.6;

const eat: Beat = {
  name: "eat",
  len: 3.9,
  start(b) {
    b.g.marks = CRUMBS.map((_, i) => ({
      kind: "poly" as const,
      x: 0,
      y: 0,
      size: 0,
      rot: i * 1.3,
      color: BREAD,
      pts: CRUMB,
    }));
  },
  frame(b, t) {
    const { g, f } = b;
    g.face = 0.6 * hold(t, 0, 3.8, 0.4);
    g.oy = 0.12 * hold(t, 0, 3.8, 0.4); // up: the crumbs lie on the floor in front of it
    g.zoom = 1 - 0.08 * hold(t, 0, 3.8, 0.4);
    let peck = 0,
      aim = 0;
    for (let i = 0; i < CRUMBS.length; i++) {
      const [x, land, at] = CRUMBS[i]!,
        m = g.marks[i]!;
      const fall = Math.min(1, Math.max(0, (t - (land - 0.45)) / 0.45));
      m.x = x;
      m.y = FLOOR + (1.1 - FLOOR) * (1 - fall * fall) + 0.04 * bump(t, land, land + 0.15);
      m.size = t < land - 0.45 || t >= at ? 0 : 0.2;
      const p = bump(t, at - 0.22, at + 0.1);
      if (p > peck) {
        peck = p;
        aim = x;
      }
      if (b.crossed(land)) b.fx.ripple(x / 0.85, FLOOR / 0.85 - 0.05, 0.2, 0.05);
      if (b.crossed(at)) b.fx.ripple(x / 0.85, FLOOR / 0.85 - 0.05, 0.35, 0.07); // peck!
    }
    // each peck: a quick bob down, turned toward the crumb
    f.pitch = 0.5 * peck;
    f.nod = 0.35 * peck;
    f.turn = 0.6 * aim * peck;
    f.mouth = peck > 0.6 ? 0.5 : 0;
    f.gaze = 1;
    f.ly = t < 2.7 ? -0.8 : 0;
    f.lx = t < 2.7 ? aim : 0;
    // the happy quack, wings flapping (happy flaps them)
    const quack = bump(t, 3.0, 3.25) + bump(t, 3.3, 3.55);
    if (quack > 0.05) f.mouth = 0.8 * quack;
    f.bob = 0.05 * quack;
    f.lid = blink(t, 0.3);
    if (b.crossed(2.8)) b.mood("happy");
    if (b.crossed(3.0) || b.crossed(3.3)) b.fx.rim(0.25);
    if (b.crossed(3.65)) b.mood("idle");
  },
};

export const beats: Beat[] = [dabble, eat];
