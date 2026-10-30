// Frog: (1) "croak" — chin up at the glass, its throat sac balloons bigger with each of three
// croaks, every croak a ring of sound rippling out through the fluid. (2) "eat" — a fly
// buzzes round the window; the frog's eyes follow it; it lands on the glass, the frog
// freezes, then — thwip — the tongue shoots out, snatches it and reels it in; a gulp
// (eyes squeezed shut, the way frogs swallow) and a pleased little hop.
import type { RigObject } from "@creatures/cute/kit.js";
import {
  WINDOW,
  backOut,
  blink,
  bump,
  clamp01,
  exposureAnchor,
  lerp,
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

type Labeled = Beat & { label: string };
const every = (t: number, dt: number, p: number) => Math.floor(t / p) !== Math.floor((t - dt) / p);

const CROAKS = [0.75, 1.55, 2.35];
const SAC_Y = -0.19; // the sac's rest height (pack-a.js)
const croakEnv = (t: number) => {
  let k = 0;
  for (const [i, at] of CROAKS.entries()) k = Math.max(k, (0.55 + 0.25 * i) * bump(t, at - 0.2, at + 0.45));
  return k;
};

const croak: Labeled = {
  name: "croak",
  label: "Big croak",
  len: 3.8,
  frame(b, t) {
    const { g, f } = b,
      L = 3.8;
    const out = step(t, 3.1, L - 0.1);
    g.face = step(t, 0, 0.5) * (1 - out);
    g.pin = g.face;
    g.focusAt = "sac";
    g.focus = 0.25 * step(t, 0.1, 0.6) * (1 - out);
    g.zoom = t < 3.1 ? zlerp(1, 1.25, step(t, 0, 0.6)) : zlerp(1.25, 1, backOut(Math.min(1, (t - 3.1) / (L - 3.2))));
    const k = croakEnv(t);
    f.pitch = -0.12 * step(t, 0.3, 0.6) * (1 - step(t, 2.8, 3.1)); // chin up
    f.squash = 0.05 * k;
    f.still = 0.7;
    // the glass stands a little further out, or the ballooning sac is pressed flat against it
    g.press = -0.35;
    f.gaze = 1;
    f.ly = 0.3;
    f.lid = Math.max(0.55 * k, blink(t, 3.0));
    f.mouth = 0;
    for (const [i, at] of CROAKS.entries())
      if (b.crossed(at)) {
        // a ring of sound out of the sac
        const s = b.where("sac") ?? [0, -0.3],
          n = 8,
          v = 1.1 + 0.3 * i;
        for (let j = 0; j < n; j++) {
          const a = (j / n) * Math.PI * 2 + i * 0.3;
          b.fx.emit({
            x: s[0] + 0.25 * Math.cos(a),
            y: s[1] + 0.25 * Math.sin(a),
            vx: v * Math.cos(a),
            vy: v * Math.sin(a),
            life: 0.5,
            strength: 0.26 + 0.06 * i,
            radius: 0.05,
          });
        }
        b.fx.rim(0.25 + 0.15 * i);
      }
    if (b.crossed(2.9)) b.mood("happy");
    if (b.crossed(3.5)) b.mood("idle");
  },
  post(b, t) {
    const k = croakEnv(t);
    for (const m of b.part("sac")) {
      const s = m.scale.x * (1 + 2.6 * k);
      m.scale.set(s, s * 0.95, m.scale.z * (1 + 1.6 * k));
      m.position.z += 0.14 * k; // swells out in front of the chin
      m.position.y = SAC_Y - 0.03 * k; // the kit re-sets z but not y: set it outright
    }
  },
};

// ---------------------------------------------------------------- eat: the fly

const FLY: Array<[number, number]> = Array.from({ length: 6 }, (_, i) => {
  const a = (i / 6) * Math.PI * 2;
  return [Math.cos(a), Math.sin(a)] as [number, number];
});
const SNAP = 2.0;
const TONGUE_LEN = 0.144; // the capsule's full length at scale 1 (len 0.1 + two 0.022 caps)

const eat: Labeled = {
  name: "eat",
  label: "Catches a fly",
  len: 4.0,
  start(b) {
    b.g.marks = [{ kind: "poly", x: 0, y: 2, size: 0, rot: 0, color: "#ffffff", pts: FLY }];
    b.state.side = b.rand(1) < 0.5 ? -1 : 1;
    b.state.L = 0.3; // the tongue's head-space length this frame (measured while hidden)
  },
  frame(b, t) {
    const { g, f, state: s } = b,
      L = 4.0;
    const out = step(t, 3.3, L - 0.1);
    g.face = step(t, 0, 0.5) * (1 - out);
    g.zoom = 1 + 0.2 * step(t, 0, 0.6) * (1 - out);
    const m = b.where("mouth") ?? [0, -0.2];
    if (s.tx === undefined && b.where("mouth")) {
      // where it will land: up and to one side of the mouth, inside the window
      let tx = m[0] + s.side! * 0.5,
        ty = m[1] + 0.12; // beside the mouth, clear of the eyes
      const r = Math.hypot(tx, ty);
      if (r > 0.75) ((tx *= 0.75 / r), (ty *= 0.75 / r));
      s.tx = tx;
      s.ty = ty;
    }
    const tx = s.tx ?? 0.4,
      ty = s.ty ?? 0.3;
    // measure head units → window units from the (hidden) tongue drawn last frame
    const tc = b.where("tongue");
    if (tc && s.Lp) {
      const k = Math.hypot(tc[0] - m[0], tc[1] - m[1]) / (s.Lp / 2);
      if (k > 0.05) s.k = s.k ? lerp(s.k, k, 0.5) : k;
    }
    const reach = s.k ? Math.hypot(tx - m[0], ty - m[1]) / s.k : 0.4;
    const ext = step(t, SNAP, SNAP + 0.07),
      ret = step(t, SNAP + 0.26, SNAP + 0.46);
    const shot = ext * (1 - ret);
    s.shot = shot;
    s.L = shot > 0.01 ? reach * shot : reach; // hidden at full reach: keeps the measure fresh
    s.dx = tx - m[0];
    s.dy = ty - m[1];
    // the fly: buzzes round in loops, settles on the glass at (tx, ty), then rides the tongue in
    let fx: number, fy: number;
    if (t < SNAP + 0.07) {
      const a = t * 3.4,
        land = step(t, 1.15, 1.6);
      const bx = 0.55 * Math.sin(a * 1.3) + 0.06 * Math.sin(t * 31),
        by = 0.25 + 0.3 * Math.sin(a * 2.1) + 0.06 * Math.cos(t * 27);
      const enter = step(t, 0, 0.5);
      fx = lerp(lerp(-1.2 * s.side!, bx, enter), tx, land);
      fy = lerp(lerp(0.9, by, enter), ty, land);
    } else {
      fx = m[0] + (tx - m[0]) * (1 - ret);
      fy = m[1] + (ty - m[1]) * (1 - ret);
    }
    const fly = g.marks[0]!;
    fly.x = fx * WINDOW;
    fly.y = fy * WINDOW;
    fly.size =
      0.075 * (1 - step(t, SNAP + 0.4, SNAP + 0.46)) * (t < 1.6 ? 0.85 + 0.25 * Math.abs(Math.sin(t * 40)) : 1);
    if (t > 0.2 && t < 1.6 && every(t, b.dt, 0.1)) b.fx.ripple(fx, fy - 0.05, 0.08, 0.04); // buzz
    // the frog: eyes on the fly, head following; frozen and crouched once it lands
    f.gaze = 1;
    f.lx = clamp01(Math.abs(fx - m[0])) * Math.sign(fx - m[0]);
    f.ly = Math.max(-1, Math.min(1, (fy - m[1]) * 1.2));
    f.turn = 0.25 * f.lx * (1 - step(t, SNAP + 0.3, SNAP + 0.5));
    const crouch = step(t, 1.6, 1.85) * (1 - step(t, SNAP, SNAP + 0.05));
    f.squash = -0.08 * crouch;
    f.still = Math.max(crouch, step(t, SNAP, SNAP + 0.05) * (1 - step(t, 2.5, 2.7)));
    f.mouth = t > SNAP - 0.03 && t < SNAP + 0.46 ? 0.6 : 0;
    // the gulp: eyes squeezed shut, the sac swells, a pleased hop
    const gulp = bump(t, SNAP + 0.5, SNAP + 0.95);
    f.lid = gulp > 0.2 ? 1 : blink(t, 3.25);
    f.squash += -0.06 * gulp;
    f.bob = 0.12 * bump(t, 3.0, 3.35);
    s.gulp = gulp;
    if (b.crossed(SNAP + 0.07)) b.fx.ripple(tx, ty, 0.45, 0.1); // thwip: the tongue hits the glass
    if (b.crossed(3.35)) b.fx.ripple(0, -0.95, 0.4, 0.14);
    if (b.crossed(1.6)) b.mood("listening");
    if (b.crossed(2.9)) b.mood("happy");
    if (b.crossed(3.6)) b.mood("idle");
  },
  post(b) {
    const s = b.state,
      mouth = (b.kit as unknown as { mouth?: { g: RigObject } }).mouth?.g;
    for (const m of b.part("sac")) {
      const k = 1 + 0.8 * (s.gulp ?? 0);
      m.scale.set(m.scale.x * k, m.scale.y * k, m.scale.z * k);
    }
    const tg = b.part("tongue")[0];
    if (!tg || !mouth) return;
    // the tongue: from the mouth toward the fly, s.L head units long (the head faces the
    // glass, so head x/y line up with the screen's)
    const Lh = Math.max(0.05, s.L ?? 0.3),
      d = Math.hypot(s.dx ?? 0.3, s.dy ?? 0.3) || 1,
      ux = (s.dx ?? 0.3) / d,
      uy = (s.dy ?? 0.3) / d;
    tg.visible = (s.shot ?? 0) > 0.01;
    tg.rotation.set(0, 0, Math.atan2(-ux, uy));
    tg.scale.set(2.6, Lh / TONGUE_LEN, 2.6);
    tg.position.set(mouth.position.x + (ux * Lh) / 2, mouth.position.y + (uy * Lh) / 2, mouth.position.z + 0.06);
    s.Lp = Lh;
  },
};

export const beats: Beat[] = [rimmed(croak, 0), rimmed(eat, 1)];
