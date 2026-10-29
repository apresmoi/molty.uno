// Owl: (1) "swivel" — it faces you, then its head turns slowly, nearly all the way round
// (the body stays put: you see the back of its head), swings all the way back, winks one
// eye and hoots twice (rings spreading through the fluid). (2) "eat" — a little mouse-shaped
// cookie drops in; the owl tips its head back, catches it and swallows it whole (the tail
// slurps in last), a gulp down its front, a contented fluff.
import { blink, bump, easeIn, hold, step, WINDOW, type Beat, type BeatCtx } from "./api";

/** Close one eye (the kit draws a shut eye as an arc ‿; it re-poses the eyes every frame). */
const wink = (b: BeatCtx, k: number) => {
  const e = b.kit.eyes[0] as unknown as Record<
    string,
    {
      visible: boolean;
      rotation: { z: number };
      position: { y: number };
      scale: { set(x: number, y: number, z: number): void };
    }
  >;
  if (k < 0.5 || !e) return;
  for (const n of ["scl", "ball", "half", "hl1", "hl2"]) if (e[n]) e[n]!.visible = false;
  const a = e.arc;
  if (!a) return;
  a.visible = true;
  a.rotation.z = Math.PI;
  a.position.y = 0.05;
  a.scale.set(0.95, 0.42, 0.35);
};

/** A hoot: a ring of particles thrown out round the beak. */
const hoot = (b: BeatCtx) => {
  const c = b.where("beak") ?? [0, 0];
  for (let i = 0; i < 6; i++) {
    const a = (i / 6) * Math.PI * 2 + 0.3;
    b.fx.emit({
      x: c[0] + 0.3 * Math.cos(a),
      y: c[1] + 0.3 * Math.sin(a),
      vx: 1.3 * Math.cos(a),
      vy: 1.3 * Math.sin(a),
      life: 0.38,
      strength: 0.22,
      radius: 0.05,
    });
  }
};

const TURN = 2.7; // rad: nearly all the way round

const swivel: Beat = {
  name: "swivel",
  len: 4.5,
  frame(b, t) {
    const { g, f } = b,
      L = 4.5;
    g.face = step(t, 0, 0.5) * (1 - step(t, 4.0, L - 0.05));
    g.zoom = 1 + 0.12 * hold(t, 0.2, 4.3, 0.4);
    // round to the right slowly, a pause looking behind, all the way back, a little overshoot
    const out = step(t, 0.5, 1.8),
      back = step(t, 2.2, 3.15);
    f.turn = TURN * out * (1 - back) - 0.25 * bump(t, 3.0, 3.5);
    f.still = 0.9;
    f.gaze = 1;
    f.lx = 0.6 * hold(t, 0.4, 2.0, 0.2) - 0.3 * bump(t, 3.0, 3.5);
    f.lid = blink(t, 0.4);
    // hoots: the beak opens round, the body puffs
    const h = bump(t, 3.85, 4.05) + bump(t, 4.12, 4.32);
    f.mouth = h > 0.05 ? 0.9 * h : 0;
    f.squash = 0.06 * h;
    f.tilt = 0.2;
    f.tiltW = hold(t, 3.4, 3.85, 0.1);
    if (b.crossed(1.85)) b.fx.rim(0.2); // the creak of a full swivel
    if (b.crossed(3.9)) hoot(b);
    if (b.crossed(4.17)) hoot(b);
  },
  post(b, t) {
    wink(b, hold(t, 3.4, 3.8, 0.02));
  },
};

// ---------------------------------------------------------------- eat: a mouse cookie
const COOKIE = "#ffd59a"; // a biscuit: warm against the lavender owl
const TAIL = "#ffc2d6";
const BODY: Array<[number, number]> = [
  [0.5, 0],
  [0.3, 0.32],
  [-0.1, 0.4],
  [-0.45, 0.2],
  [-0.5, -0.05],
  [-0.3, -0.25],
  [0.25, -0.25],
];
const EAR: Array<[number, number]> = [];
for (let i = 0; i < 8; i++) EAR.push([0.5 * Math.cos((i / 8) * Math.PI * 2), 0.5 * Math.sin((i / 8) * Math.PI * 2)]);
const TAILP: Array<[number, number]> = [
  [0, 0.08],
  [1, 0.05],
  [1, -0.05],
  [0, -0.08],
];

const eat: Beat = {
  name: "eat",
  len: 4.2,
  start(b) {
    b.g.marks = [
      { kind: "poly", x: 0, y: 0, size: 0, rot: 0, color: TAIL, pts: TAILP },
      { kind: "poly", x: 0, y: 0, size: 0, rot: 0, color: COOKIE, pts: BODY },
      { kind: "poly", x: 0, y: 0, size: 0, rot: 0, color: COOKIE, pts: EAR },
    ];
  },
  frame(b, t) {
    const { g, f } = b;
    g.face = hold(t, 0, 4.1, 0.4);
    g.zoom = 1 + 0.08 * hold(t, 0, 4.1, 0.4);
    const mo = b.where("beak") ?? [0, 0];
    const bx = mo[0] * WINDOW,
      by = mo[1] * WINDOW - 0.04;
    // the cookie: falls from the top, caught at 1.2 nose-first in the beak, sticking out
    // sideways (over the dark edge of the head, where it reads); slides in by 2.2, the tail
    // last by 2.6
    const fall = easeIn(Math.min(1, t / 1.2)),
      swallow = step(t, 1.4, 2.2),
      tailIn = step(t, 2.15, 2.6);
    const s = 0.5 * (1 - swallow),
      reach = 0.42 * (1 - swallow);
    const cx = bx + reach,
      cy = t < 1.2 ? 1.3 + (by - 0.06 - 1.3) * fall : by - 0.06;
    const [tail, body, ear] = g.marks as [(typeof g.marks)[0], (typeof g.marks)[0], (typeof g.marks)[0]];
    body.x = cx;
    body.y = cy;
    body.size = s;
    body.rot = t < 1.2 ? Math.PI + 0.4 * Math.sin(t * 8) : Math.PI; // nose (its +x) toward the beak
    ear.x = cx - 0.06 * (1 - swallow);
    ear.y = cy + 0.19 * (1 - swallow);
    ear.size = 0.2 * (1 - swallow);
    tail.x = cx + 0.2 * (1 - swallow);
    tail.y = cy;
    tail.rot = 0.35 * Math.sin(t * 12) * (1 - tailIn);
    tail.size = 0.45 * (1 - tailIn) * step(t, 0, 0.2);
    // the head: up to catch it, gulping it down
    f.nod = -0.25 * step(t, 0.7, 1.15) * (1 - step(t, 2.6, 3.0));
    f.gaze = 1;
    f.ly = t < 1.25 ? 0.8 : 0;
    const chomp = hold(t, 1.1, 2.55, 0.1);
    f.mouth = chomp > 0.05 ? 0.25 + 0.45 * chomp * (0.5 + 0.5 * Math.sin(t * 16)) : 0;
    // the gulp: a bulge going down (squash), then a fluff
    f.squash = -0.07 * bump(t, 2.6, 2.95) + 0.08 * bump(t, 3.2, 3.6);
    f.lean = 0.12 * Math.sin(t * 30) * bump(t, 3.2, 3.6);
    f.lid = Math.max(blink(t, 2.95), t > 2.65 && t < 2.95 ? 0.5 : 0);
    if (b.crossed(1.2)) b.mood("surprised");
    if (b.crossed(1.5)) b.mood("idle");
    if (b.crossed(2.75)) b.fx.ripple(mo[0], mo[1] - 0.75, 0.3, 0.12); // gulp
    if (b.crossed(3.2)) {
      b.mood("happy");
      b.fx.rim(0.3);
    }
    if (b.crossed(4.0)) b.mood("idle");
  },
};

export const beats: Beat[] = [swivel, eat];
