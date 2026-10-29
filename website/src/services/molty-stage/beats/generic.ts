// The generic glass beats: any Molty can play these (the fallback, and the variety between a
// creature's signature beats). See api.ts for how a beat is written.
import { backOut, blink, easeOut, lerp, smooth, step, zlerp, type Beat } from "./api";

const G_PRESS = 0.16; // fit units the front is pressed past the glass
const EYE_FILL = 0.5; // the eye close-up: the eye's radius in the window (radius ~0.85)
const QCOLOR = "#f2efe6";
const STAR = "#ffffff"; // white reads on any body colour (a yellow star vanished on the duck)

/** Comes up until one eye fills the window, blinks, looks round, backs off. */
const eye: Beat = {
  name: "eye",
  len: 3.8,
  frame(b, t) {
    const { g, f } = b,
      L = 3.8,
      out = step(t, 3.0, L - 0.1),
      ze = Math.min(9, Math.max(3, EYE_FILL / b.eyeR));
    g.face = g.pin = step(t, 0, 0.9) * (1 - out);
    g.focusAt = "eye";
    g.focus = step(t, 0.3, 1.4) * (1 - out);
    if (t < 0.3)
      g.zoom = lerp(1, 0.92, step(t, 0, 0.3)); // a step back first
    else if (t < 1.7)
      g.zoom = zlerp(0.92, ze, step(t, 0.3, 1.7)); // creeping right up to it
    else if (t < 3.0) g.zoom = ze * (1 + 0.025 * Math.sin(((t - 1.7) / 1.3) * Math.PI * 2));
    else g.zoom = zlerp(ze, 1, backOut(Math.min(1, (t - 3.0) / (L - 3.1))));
    // the eye looks left, then right, then back at you; blinks, then a double blink
    const left = step(t, 1.95, 2.05) * (1 - step(t, 2.3, 2.4)),
      right = step(t, 2.3, 2.4) * (1 - step(t, 2.65, 2.75));
    f.gaze = step(t, 0, 0.6) * (1 - out);
    f.lx = 0.85 * (right - left);
    f.ly = 0.25 * left - 0.1 * right;
    f.lid = Math.max(blink(t, 1.75), blink(t, 2.8), blink(t, 3.02));
    f.still = step(t, 0.3, 1.2) * (1 - out);
    f.mouth = 0;
    if (b.crossed(1.7)) b.fx.rim(0.2);
  },
};

/** Comes close (not pressed), tilts its head one way and the other, a "?", looks away and
 *  squints, shrugs, backs off. */
const confused: Beat = {
  name: "confused",
  len: 3.6,
  start(b) {
    b.g.marks = [{ kind: "?", x: 0.48, y: 0.42, size: 0, rot: 0, color: QCOLOR }];
  },
  frame(b, t) {
    const { g, f } = b,
      L = 3.6,
      out = step(t, 2.85, L - 0.1);
    g.face = step(t, 0, 0.8) * (1 - out);
    g.pin = g.face * 0.9;
    if (t < 0.25) g.zoom = lerp(1, 0.95, step(t, 0, 0.25));
    else if (t < 2.85) g.zoom = zlerp(0.95, 1.6, step(t, 0.25, 1.0));
    else g.zoom = zlerp(1.6, 1, backOut(Math.min(1, (t - 2.85) / (L - 2.95))));
    const right = step(t, 0.95, 1.25) * (1 - step(t, 1.6, 1.9)),
      left = step(t, 1.6, 1.9) * (1 - step(t, 2.25, 2.5));
    const tilt = 0.42 * (right - left);
    f.tilt = tilt;
    f.tiltW = step(t, 0.8, 1.0) * (1 - out);
    g.roll = tilt * 0.4; // the head tilt, read on screen too
    // at you first, then away (up and aside, the head turning with it), squinting
    f.gaze = step(t, 0, 0.6) * (1 - out);
    f.lx = 0.85 * left;
    f.ly = 0.45 * left;
    f.turn = 0.3 * left;
    const sq = 0.4 * step(t, 1.65, 1.85) * (1 - step(t, 2.3, 2.45));
    f.lid = sq > 0.01 ? Math.max(sq, blink(t, 1.3)) : blink(t, 1.3);
    // a small shrug
    const sh = t > 2.35 && t < 2.8 ? Math.sin(((t - 2.35) / 0.45) * Math.PI) : 0;
    f.bob = 0.06 * sh;
    f.squash = 0.07 * sh;
    const q = g.marks[0]!;
    q.size = t < 1.0 ? 0 : 0.44 * backOut(Math.min(1, (t - 1.0) / 0.3)) * (1 - step(t, 2.55, 2.8));
    q.rot = -0.25 + 0.15 * Math.sin(t * 6);
    if (b.crossed(0.9)) b.mood("thinking"); // the mouth goes to one side
    if (b.crossed(2.85)) b.mood("idle");
  },
};

/** Runs at the screen and hits it: splat, a thud through the fluid, recoils with a bounce,
 *  dazed (half-lidded, eyes rolling, stars, wobbling), shakes it off. */
const bonk: Beat = {
  name: "bonk",
  len: 3.6,
  start(b) {
    b.g.marks = [0, 1, 2].map(() => ({ kind: "star" as const, x: 0, y: 0, size: 0, rot: 0, color: STAR }));
  },
  frame(b, t) {
    const { g, f } = b,
      L = 3.6,
      HIT = 0.62,
      out = step(t, 2.95, L - 0.1);
    g.face = step(t, 0, 0.45) * (1 - out);
    g.pin = step(t, 0.1, 0.55) * (1 - out);
    if (t < 0.3) {
      g.zoom = lerp(1, 0.86, step(t, 0, 0.3)); // crouch back
      f.squash = -0.12 * step(t, 0, 0.3);
    } else if (t < HIT) {
      const x = (t - 0.3) / (HIT - 0.3);
      g.zoom = lerp(0.86, 2.4, x * x); // the charge, faster and faster
      f.squash = 0.08 * x;
    } else if (t < 0.8) {
      g.press = G_PRESS * (1.8 - (t - HIT) * 2); // splat
      g.zoom = 2.4;
      g.sx = 1.24;
      g.sy = 0.82;
      g.ox = 0.05 * Math.sin((t - HIT) * 70) * Math.exp(-(t - HIT) * 8); // the shudder
    } else if (t < 2.95) {
      const h = t - 0.8,
        e = Math.exp(-h * 5),
        w = Math.cos(h * 13);
      g.press = G_PRESS * 1.4 * Math.exp(-h * 30);
      g.zoom = 1.2 + 1.2 * Math.exp(-h * 6) * Math.cos(h * 8); // recoil, bouncing
      g.sx = 1 + 0.24 * e * w;
      g.sy = 1 - 0.18 * e * w;
    } else g.zoom = zlerp(1.2, 1, backOut(Math.min(1, (t - 2.95) / (L - 3.05))));
    f.still = t >= HIT && t < 1.0 ? 1 : 0;
    // dazed: wobbling, eyes rolling round, stars
    const daze = step(t, 1.0, 1.2) * (1 - step(t, 2.4, 2.55)),
      wob = Math.sin((t - 1.0) * Math.PI * 2 * 1.3);
    g.roll = 0.17 * daze * wob;
    f.tilt = -0.3 * daze * wob;
    f.tiltW = daze;
    f.gaze = step(t, 0, 0.3) * (1 - out);
    f.lx = 0.6 * daze * Math.cos(t * 9);
    f.ly = 0.5 * daze * Math.sin(t * 9);
    f.lid = t >= HIT && t < 1.0 ? 1 : daze > 0 ? 0.62 : -1; // screwed shut, then half-lidded
    f.mouth = t >= HIT && t < 1.0 ? 0.35 : 0;
    // shakes it off
    if (t > 2.45 && t < 2.95) f.turn = 0.5 * Math.sin((t - 2.45) * Math.PI * 2 * 5) * (1 - (t - 2.45) / 0.5);
    for (let i = 0; i < g.marks.length; i++) {
      const m = g.marks[i]!,
        a = t * 5 + (i * Math.PI * 2) / 3;
      m.x = 0.42 * Math.cos(a);
      m.y = 0.62 + 0.07 * Math.sin(a);
      m.size = 0.36 * daze * (0.75 + 0.25 * Math.sin(a));
      m.rot = t * 3;
    }
    if (b.crossed(HIT)) {
      b.mood("surprised");
      b.fx.rim(0.9, 6); // the thud: the glass shakes round its sides and foot
    }
    if (b.crossed(0.8)) b.fx.rim(0.45);
    if (b.crossed(1.0)) b.mood("sleepy");
    if (b.crossed(2.45)) b.mood("idle");
  },
};

/** Walks up and presses its face flat on the glass: a curious look round, a slow blink,
 *  peels off and back with a little overshoot. */
const press: Beat = {
  name: "press",
  len: 3.3,
  frame(b, t) {
    const { g, f } = b,
      BACK = 0.3, // anticipation (leans back, starts to turn)
      HIT = 0.95, // contact
      OFF = 2.35, // starts to peel off
      L = 3.3,
      ZOOM = 2.1;
    const out = smooth((t - OFF) / (L - OFF - 0.15));
    g.face = smooth(t / HIT) * (1 - out);
    g.pin = smooth(t / (HIT * 0.85)) * (1 - out);
    let p = 0;
    if (t < BACK) g.zoom = lerp(1, 0.92, smooth(t / BACK));
    else if (t < HIT) {
      const x = (t - BACK) / (HIT - BACK);
      g.zoom = lerp(0.92, ZOOM, x * x * x); // accelerating into the glass
      p = G_PRESS * 0.25 * Math.max(0, (x - 0.9) / 0.1);
    } else if (t < OFF) {
      const h = t - HIT;
      // the bump: pressed in past the hold, springing back, then breathing against the glass
      const bmp = Math.exp(-h * 7) * Math.cos(h * 18);
      p = G_PRESS * (1 + 0.6 * bmp + 0.12 * Math.sin((h / 1.1) * Math.PI * 2));
      g.zoom = ZOOM * (1 + 0.04 * bmp);
      g.sx = 1 + 0.07 + 0.05 * bmp;
      g.sy = 1 - 0.04 - 0.04 * bmp;
    } else {
      // peel off (the press lets go first), back to its place with a small undershoot
      const x = Math.min(1, (t - OFF) / (L - OFF));
      p = G_PRESS * (1 - smooth(x / 0.18));
      g.zoom = lerp(ZOOM, 1, backOut(x));
      g.sx = lerp(1.07, 1, smooth(x / 0.3));
      g.sy = lerp(0.96, 1, smooth(x / 0.3));
    }
    g.press = p;
    // eyes on you, then a look along the glass and back; a small "oh" at the bump, then the
    // mouth shut (scaled 2x, an open mouth or bill reads as a hole in the face)
    f.gaze = smooth(t / 0.6) * (1 - out);
    const along = step(t, 1.5, 1.7) * (1 - step(t, 2.05, 2.2));
    f.lx = -0.7 * along;
    f.ly = 0.15 * along;
    f.lid = blink(t, 1.35);
    f.mouth = t >= HIT && t < HIT + 0.45 ? 0.3 : t >= HIT + 0.45 && t < OFF + 0.3 ? 0 : -1;
    f.still = (0.75 * p) / G_PRESS;
    // the bump on the glass shakes the fluid round the window's rim (never at its centre: an
    // impulse there carves a crater out of the face)
    if (b.crossed(HIT)) b.fx.rim(0.75);
    if (b.crossed(HIT + 0.45)) b.mood("idle");
    if (b.crossed(OFF)) b.fx.rim(0.3); // and a softer one as it peels off
  },
};

/** Steps out of the window to the right, peeks back in round its edge (half a face, one
 *  eye), looks, ducks out, walks back in. */
const peek: Beat = {
  name: "peek",
  len: 3.4,
  frame(b, t) {
    const { g, f } = b,
      L = 3.4,
      OUT = 1.9, // fit units: clear of the window
      TURN = 0.95; // rad: facing its way
    if (t < 0.55) {
      const x = t / 0.55;
      g.ox = OUT * x * x;
      g.yaw = TURN * step(t, 0, 0.2); // turned to its way out
      f.bob = 0.05 * Math.abs(Math.sin(x * Math.PI * 2));
    } else if (t < 2.55) {
      g.face = g.pin = 1;
      g.zoom = 1.9;
      const x = Math.min(1, Math.max(0, (t - 2.22) / 0.25)),
        inn = easeOut((t - 0.8) / 0.45) * (1 - x * x);
      g.ox = OUT - 1.15 * inn;
      g.oy = 0.06;
      g.roll = 0.24 * inn; // leaning in round the edge
      f.gaze = 1;
      f.lx = t < 1.55 ? -0.7 : 0; // into the window, then at you
      f.lid = blink(t, 1.9);
      f.still = 1;
    } else {
      const x = Math.min(1, (t - 2.55) / (L - 2.6));
      g.ox = OUT * (1 - easeOut(x));
      g.yaw = -TURN * (1 - step(x, 0.6, 1)); // facing its way back, then you
      f.bob = 0.05 * Math.abs(Math.sin(x * Math.PI * 3)) * (1 - x);
    }
    if (b.crossed(0.01)) b.mood("idle");
    if (b.crossed(0.55)) b.mood("listening");
    if (b.crossed(0.8)) b.fx.ripple(1.08, 0, 0.35, 0.15); // round the edge
    if (b.crossed(2.22)) b.fx.ripple(1.08, 0, 0.25, 0.15);
    if (b.crossed(2.55)) b.mood("idle");
  },
};

/** The generic beats, in rotation order. */
export const GENERIC: Beat[] = [eye, confused, bonk, press, peek];
