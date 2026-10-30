// Bear cub: (1) "nap" — a huge yawn with a big stretch (paws up), it nods off: a bubble
// swells and shrinks at its nose with each breath and a staircase of Z z Z drifts up, until
// the bubble pops and it jolts awake. (2) "eat" — a honey pot: it dips a paw in, licks it,
// dips again, and munches happily, rocking.
import { backOut, blink, bump, hold, step, WINDOW, type Beat, type BeatCtx } from "./api";

const WHITE = "#ffffff";
// the Zs: when each appears, where (fit units, a staircase up and right), how big
// (zig-zagging, far enough apart that they never touch)
const ZS: Array<[number, number, number, number]> = [
  [1.4, 0.42, 0.3, 0.34],
  [2.1, 0.5, 0.52, 0.46],
  [2.8, 0.42, 0.34, 0.34],
];
const RING: Array<[number, number]> = [];
for (let i = 0; i < 12; i++)
  RING.push([0.5 * Math.cos((i / 12) * Math.PI * 2), 0.5 * Math.sin((i / 12) * Math.PI * 2)]);
const WAKE = 3.5;

/** Both arms up (a stretch) or one (`side`) toward the mouth: added to the kit's arm pose. */
const arms = (b: BeatCtx, up: number, side = 0) => {
  for (const [i, g] of b.socket("side").entries()) if (!side || i === side - 1) g.rotation.x -= up;
};

const nap: Beat = {
  name: "nap",
  len: 4.4,
  start(b) {
    b.g.marks = [
      ...ZS.map(() => ({ kind: "z" as const, x: 0, y: 0, size: 0, rot: -0.15, color: WHITE })),
      { kind: "poly", x: 0, y: 0, size: 0, rot: 0, color: WHITE, pts: RING },
    ];
  },
  frame(b, t) {
    const { g, f } = b;
    g.face = hold(t, 0, 4.3, 0.4);
    g.zoom = 1 + 0.1 * hold(t, 0, 4.3, 0.4);
    g.oy = -0.1 * hold(t, 0, 4.3, 0.4); // room for the Zs
    g.ox = -0.12 * hold(t, 0, 4.3, 0.4);
    const yawn = bump(t, 0.1, 1.2),
      asleep = hold(t, 1.15, WAKE, 0.3);
    f.mouth = yawn > 0.05 ? Math.min(1, 1.3 * yawn) : 0;
    f.nod = -0.35 * yawn + 0.12 * asleep; // head back for the yawn, drooping asleep
    // breathing: slow and deep while asleep
    const breath = Math.sin((t - 1.2) * 3.4);
    f.squash = 0.1 * yawn - 0.08 * asleep + 0.04 * asleep * breath;
    f.pitch = 0.08 * asleep;
    f.tilt = 0.25;
    f.tiltW = asleep;
    f.lid = t < WAKE ? Math.max(0.7 * yawn, asleep > 0.5 ? 1 : 0) : blink(t, 3.95);
    f.bob = 0.1 * bump(t, WAKE, WAKE + 0.3); // the jolt awake
    // Z z Z: each pops in at its stair and bobs there, fading by the wake
    for (let i = 0; i < ZS.length; i++) {
      const [at, x, y, s] = ZS[i]!,
        m = g.marks[i]!;
      m.x = x + 0.04 * Math.sin(t * 2 + i);
      m.y = y + 0.05 * (t - at);
      // each lives ~1.3 s (at most two at once), all gone at the wake
      m.size =
        t < at
          ? 0
          : s *
            backOut(Math.min(1, (t - at) / 0.25)) *
            (1 - step(t, at + 0.75, at + 0.9)) *
            (1 - step(t, WAKE - 0.1, WAKE + 0.05));
      m.rot = -0.15 + 0.1 * Math.sin(t * 1.5 + i);
    }
    // the nose bubble: swells and shrinks with the breath, pops at the wake
    const nose = b.where("nose"),
      bub = g.marks[ZS.length]!;
    if (nose) {
      const k = 0.5 + 0.5 * breath;
      bub.size = t > 1.4 && t < WAKE ? (0.1 + 0.22 * k) * step(t, 1.4, 1.7) : 0;
      bub.x = nose[0] * WINDOW + 0.05 + bub.size * 0.45;
      bub.y = nose[1] * WINDOW - 0.05 - bub.size * 0.3;
    }
    if (b.crossed(1.15)) b.mood("sleepy");
    if (b.crossed(WAKE)) {
      b.mood("surprised");
      const p = nose ?? [0, 0];
      b.fx.ripple(p[0] + 0.15, p[1] - 0.1, 0.45, 0.08); // pop!
      b.fx.ripple(0, -0.95, 0.4, 0.14);
    }
    if (b.crossed(4.0)) b.mood("idle");
  },
  post(b, t) {
    arms(b, 2.2 * bump(t, 0.15, 1.2)); // the stretch, paws up high
  },
};

// ---------------------------------------------------------------- eat: the honey pot
const POT = "#ffd27a";
const RIM = "#fff1c4";
const HONEY = "#ffb43c";
const POTP: Array<[number, number]> = [
  [0, -0.5],
  [0.38, -0.42],
  [0.5, -0.1],
  [0.42, 0.25],
  [-0.42, 0.25],
  [-0.5, -0.1],
  [-0.38, -0.42],
];
const RIMP: Array<[number, number]> = [
  [-0.36, 0.22],
  [0.36, 0.22],
  [0.4, 0.36],
  [-0.4, 0.36],
];
// the drip on the pot's side
const DRIPP: Array<[number, number]> = [
  [-0.3, 0.22],
  [-0.1, 0.22],
  [-0.16, 0.02],
  [-0.22, -0.06],
  [-0.28, 0.02],
];
// the pot stands to its right (screen right), where its right paw can reach
const PX = 0.42,
  PY = -0.52,
  PS = 0.5;
// dips: paw into the pot, up to the mouth, licked
const DIPS = [0.7, 2.0];

const eat: Beat = {
  name: "eat",
  len: 4.4,
  start(b) {
    b.g.marks = [
      { kind: "poly", x: PX, y: PY, size: 0, rot: 0, color: POT, pts: POTP },
      { kind: "poly", x: PX, y: PY, size: 0, rot: 0, color: RIM, pts: RIMP },
      { kind: "poly", x: PX, y: PY, size: 0, rot: 0, color: HONEY, pts: DRIPP },
      { kind: "drop", x: 0, y: 0, size: 0, rot: Math.PI, color: HONEY },
    ];
  },
  frame(b, t) {
    const { g, f } = b;
    g.face = hold(t, 0, 4.3, 0.4);
    g.oy = 0.08 * hold(t, 0, 4.3, 0.4);
    g.ox = -0.14 * hold(t, 0, 4.3, 0.4); // aside: the pot stands beside it
    const pot = step(t, 0, 0.4) * (1 - step(t, 4.0, 4.3));
    for (let i = 0; i < 3; i++) g.marks[i]!.size = PS * backOut(pot);
    g.marks[2]!.size *= 1 + 0.25 * step(t, 1.0, 3.0); // the drip runs
    // the paw: up to the mouth, a lick (tongue = mouth open + nod), honey on it
    let lick = 0,
      up = 0;
    for (const at of DIPS) {
      up = Math.max(up, hold(t, at + 0.3, at + 1.05, 0.25));
      lick = Math.max(lick, bump(t, at + 0.55, at + 0.75), bump(t, at + 0.8, at + 1.0));
    }
    b.state.up = up;
    const dip = DIPS.reduce((a, at) => Math.max(a, bump(t, at - 0.35, at + 0.35)), 0);
    b.state.dip = dip;
    f.mouth = lick > 0.05 ? 0.55 + 0.4 * lick : up > 0.5 ? 0.3 : 0;
    f.nod = 0.12 * lick - 0.08 * up + 0.12 * step(t, 0.3, 0.6) * (1 - up) * (t < 3.0 ? 1 : 0);
    f.gaze = 1;
    f.ly = up > 0.3 ? 0 : -0.7; // eyes on the pot
    f.lx = up > 0.3 ? 0 : 0.6;
    // each dip: it leans right over to the pot, nose down to the rim
    f.lean = -0.32 * dip;
    f.px = 0.1 * dip;
    f.turn = 0.35 * dip;
    f.nod += 0.3 * dip;
    f.lid = Math.max(0.5 * lick, blink(t, 3.95));
    // the happy munch: rocking side to side, chewing
    const munch = hold(t, 3.0, 4.1, 0.2);
    f.lean += 0.12 * Math.sin(t * 9) * munch;
    if (munch > 0.2) f.mouth = 0.3 + 0.3 * Math.abs(Math.sin(t * 10));
    // honey on its nose after each dip, licked off
    const nose = b.where("nose"),
      d = g.marks[3]!;
    d.size = nose && t > DIPS[0]! + 0.2 && t < 3.2 && up > 0.05 ? 0.2 * (1 - 0.6 * lick) : 0;
    if (nose) {
      d.x = nose[0] * WINDOW + 0.02;
      d.y = nose[1] * WINDOW - 0.1;
    }
    for (const at of DIPS) if (b.crossed(at + 0.1)) b.fx.ripple(PX / WINDOW, (PY + 0.2) / WINDOW, 0.3, 0.08); // splosh
    if (b.crossed(1.4)) b.mood("happy");
    if (b.crossed(1.9)) b.mood("idle");
    if (b.crossed(3.0)) b.mood("happy");
    if (b.crossed(4.15)) b.mood("idle");
  },
  post(b) {
    // one paw: down into the pot, then up to the mouth
    arms(b, 1.35 * (b.state.up ?? 0) - 0.35 * (b.state.dip ?? 0), 1);
  },
};

export const beats: Beat[] = [nap, eat];
