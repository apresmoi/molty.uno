// Fox: (1) "pounce" — it hears something under the snow (the fluid): head tilts one way and
// the other, it crouches and wiggles, LEAPS, and dives head-first into the snow at the
// bottom of the window — stuck upside down, tail waving — then pops out, shakes the snow
// off and is very pleased with itself. (2) "eat" — a berry rolls in along the floor; it
// tracks it, snaps it up, chews and licks its lips.
import {
  WINDOW,
  backOut,
  blink,
  bump,
  clamp01,
  easeIn,
  easeOut,
  hold,
  lerp,
  smooth,
  step,
  zlerp,
  type Beat,
} from "./api";

type Labeled = Beat & { label: string };
type Pt = Array<[number, number]>;
const ring = (n: number, sx = 1, sy = 1): Pt =>
  Array.from({ length: n }, (_, i) => {
    const a = (i / n) * Math.PI * 2;
    return [sx * Math.cos(a), sy * Math.sin(a)] as [number, number];
  });
const every = (t: number, dt: number, p: number) => Math.floor(t / p) !== Math.floor((t - dt) / p);

// ---------------------------------------------------------------- pounce: head-first into the snow

const CROUCH = 1.25,
  LEAP = 1.7,
  DIVE = 2.3, // head in the snow
  POP = 3.05, // pulls itself out
  SHAKE = 3.35;

const pounce: Labeled = {
  name: "pounce",
  label: "Dives into the snow",
  len: 4.5,
  start(b) {
    b.mood("listening");
  },
  frame(b, t) {
    const { g, f } = b,
      L = 4.5;
    g.face = step(t, 0, 0.4) * (1 - step(t, 3.9, L - 0.1));
    g.pin = 1;
    // listening: eyes down at the snow, head tilts one way, then the other
    const listen = 1 - step(t, CROUCH - 0.1, CROUCH + 0.1);
    f.gaze = 1;
    f.ly = -0.7 * listen;
    f.tilt = t < 0.7 ? 0.3 : -0.3;
    f.tiltW = hold(t, 0.15, CROUCH, 0.15);
    // crouch + the bum wiggle
    const cr = step(t, CROUCH - 0.15, CROUCH + 0.1) * (1 - step(t, LEAP - 0.05, LEAP));
    f.squash = -0.16 * cr;
    f.lean = 0.12 * Math.sin(t * 30) * step(t, CROUCH + 0.1, CROUCH + 0.2) * cr;
    f.nod = 0.2 * cr;
    // the leap: up in an arc, flipping head-down (a half turn in the screen plane), then
    // straight down into the snow below the window, rear end and tail left sticking up
    let oy = 0,
      roll = 0,
      zoom = 1;
    if (t >= LEAP && t < DIVE) {
      const q = (t - LEAP) / (DIVE - LEAP);
      oy = 0.55 * Math.sin(Math.PI * Math.min(1, q * 1.35)) - 0.82 * easeIn(clamp01((q - 0.55) / 0.45));
      roll = Math.PI * smooth(q * 1.2);
      zoom = 1 - 0.12 * Math.sin(Math.PI * q);
      f.squash = 0.12 * Math.sin(Math.PI * q);
    } else if (t >= DIVE && t < POP) {
      oy = -0.82;
      roll = Math.PI;
      // stuck: legs kicking (the body wiggles), the tail waving
      f.lean = 0.16 * Math.sin((t - DIVE) * 24) * hold(t, DIVE + 0.15, POP - 0.1, 0.1);
    } else if (t >= POP && t < SHAKE) {
      const q = (t - POP) / (SHAKE - POP);
      oy = -0.82 * (1 - easeOut(q)) + 0.2 * Math.sin(Math.PI * q);
      roll = Math.PI * (1 + smooth(q)); // over the top, on round to upright
    } // from SHAKE on it is upright again (2π ≡ 0)
    g.oy = oy;
    g.roll = roll;
    g.zoom = zoom * (t > SHAKE ? zlerp(1.06, 1, backOut(clamp01((t - SHAKE) / 0.6))) : 1);
    // shaking the snow off, then smug
    const sh = hold(t, SHAKE, SHAKE + 0.55, 0.05);
    f.turn = 0.5 * Math.sin((t - SHAKE) * 38) * sh * (1 - clamp01((t - SHAKE) / 0.55));
    f.lid = t >= DIVE && t < SHAKE ? 1 : Math.max(blink(t, 0.95), blink(t, 4.05));
    f.mouth = t >= LEAP && t < DIVE ? 0.5 : -1;
    if (b.crossed(LEAP)) b.mood("happy");
    if (b.crossed(DIVE)) {
      b.mood("surprised");
      b.fx.rim(1.0, 6); // the snow takes it
      for (let i = 0; i < 6; i++) {
        const a = Math.PI * (0.15 + (0.7 * i) / 5);
        b.fx.emit({
          x: 0.1 * Math.cos(a),
          y: -0.85,
          vx: 1.2 * Math.cos(a),
          vy: 1.1 * Math.sin(a),
          g: 3.2,
          life: 0.55,
          strength: 0.2,
          radius: 0.06,
        });
      }
    }
    if (b.crossed(DIVE + 0.1)) b.mood("idle");
    if (b.crossed(POP)) b.fx.ripple(0, -0.95, 0.6, 0.16);
    if (t > SHAKE && t < SHAKE + 0.5 && every(t, b.dt, 0.12)) {
      const side = b.rand(Math.floor(t * 20)) < 0.5 ? -1 : 1;
      b.fx.emit({ x: side * 0.35, y: 0.05, vx: side * 1.6, vy: 0.5, g: 3, life: 0.35, strength: 0.14, radius: 0.045 });
    }
    if (b.crossed(SHAKE + 0.55)) b.mood("happy");
    if (b.crossed(4.1)) b.mood("idle");
  },
  post(b, t) {
    // the tail waves fast while it is stuck in the snow
    const stuck = hold(t, DIVE, POP, 0.1);
    if (stuck <= 0) return;
    for (const r of b.socket("tail")) r.rotation.z += 0.5 * Math.sin(t * 16) * stuck;
  },
};

// ---------------------------------------------------------------- eat: a berry

// light colours: a mid-dark red fades out under the slot's auto-exposure
const BERRY = "#ffadbb",
  LEAF = "#c8f2b2";
const ROLL = 1.1, // the berry stops in front
  SNAP = 1.85,
  CHEW = 2.15;

const eat: Labeled = {
  name: "eat",
  label: "Snaps up a berry",
  len: 4.2,
  start(b) {
    b.g.marks = [
      { kind: "poly", x: 0, y: 0, size: 0, rot: 0, color: BERRY, pts: ring(9) },
      { kind: "poly", x: 0, y: 0, size: 0, rot: 0, color: LEAF, pts: ring(5, 1, 0.45) },
    ];
    b.mood("idle");
    b.state.side = b.rand(3) < 0.5 ? -1 : 1;
  },
  frame(b, t) {
    const { g, f, state: s } = b,
      L = 4.2,
      side = s.side!;
    const out = step(t, 3.55, L - 0.1);
    g.face = step(t, 0, 0.4) * (1 - out);
    g.pin = g.face;
    g.zoom = t < 3.55 ? zlerp(1, 1.25, step(t, 0.9, 1.6)) : zlerp(1.25, 1, backOut(clamp01((t - 3.55) / (L - 3.6))));
    // the berry rolls in along the floor from one side, bounces to a stop in front
    const m = b.where("mouth") ?? [0, -0.2];
    const q = easeOut(clamp01(t / ROLL));
    let bx = side * lerp(1.3, 0.5, q),
      by = -0.5 + 0.06 * Math.abs(Math.sin(q * Math.PI * 3)) * (1 - q);
    // snapped up: it jumps to the mouth and is gone
    const snap = step(t, SNAP - 0.12, SNAP);
    bx = lerp(bx, m[0], snap);
    by = lerp(by, m[1], snap);
    const berry = g.marks[0]!,
      leaf = g.marks[1]!;
    const gone = t > SNAP ? 0 : 1;
    berry.x = bx * WINDOW;
    berry.y = by * WINDOW;
    berry.size = 0.13 * gone * step(t, 0, 0.05);
    berry.rot = -side * t * 6 * (1 - q);
    leaf.x = berry.x + 0.12 * Math.sin(berry.rot);
    leaf.y = berry.y + 0.12 * Math.cos(berry.rot);
    leaf.rot = berry.rot;
    leaf.size = 0.065 * gone * step(t, 0, 0.05);
    // tracks it, a curious tilt, then the snap: a quick dip forward
    f.gaze = 1;
    f.lx = t < SNAP ? clamp01(Math.abs(bx)) * Math.sign(bx) : 0;
    f.ly = t < SNAP ? -0.6 : 0;
    f.tilt = 0.25 * side;
    f.tiltW = hold(t, ROLL, SNAP - 0.2, 0.15);
    const dip = bump(t, SNAP - 0.25, SNAP + 0.2);
    f.nod = 0.9 * dip;
    f.pitch = 0.2 * dip;
    f.mouth =
      t > SNAP - 0.25 && t < SNAP ? 0.7 : t >= CHEW && t < 3.0 ? 0.3 * Math.max(0, Math.sin((t - CHEW) * 15)) : -1;
    f.bob = 0.02 * Math.max(0, Math.sin((t - CHEW) * 15)) * hold(t, CHEW, 3.0, 0.1);
    f.lid = Math.max(blink(t, 0.6), blink(t, 3.15));
    if (b.crossed(ROLL)) b.fx.ripple(bx, -0.8, 0.25, 0.08);
    if (b.crossed(SNAP)) b.fx.rim(0.4);
    // licks its lips (two quick flicks of an open mouth)
    if (t > 3.05 && t < 3.5) f.mouth = 0.45 * (bump(t, 3.05, 3.22) + bump(t, 3.28, 3.45));
    if (b.crossed(3.1)) b.mood("happy");
    if (b.crossed(3.8)) b.mood("idle");
  },
};

export const beats: Beat[] = [pounce, eat];
