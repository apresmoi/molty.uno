// Mushroom: (1) "spores" — crouches, boings up and POOF: a cloud of sparkling spores drifts
// up and round the window; it watches them, one tickles its nose… a big sneeze (a second
// puff, the cap pops), then a giggle. (2) "eat" — a rain drop falls into the window; it looks
// up, catches it on its cap (splash), soaks it up: the cap swells, it grows a little taller,
// and it beams.
import { backOut, blink, bump, hold, step, WINDOW, zlerp, type Beat, type BeatCtx } from "./api";

const SPORE = "#fff08a";
const POOF = 0.75,
  SNEEZE = 2.95;
// the sparkles: per puff, how they fan out (dx, rise) in window units
const FAN: ReadonlyArray<readonly [number, number, number]> = [
  [-0.55, 0.55, POOF],
  [0.5, 0.65, POOF],
  [-0.15, 0.85, POOF],
  [0.25, 0.4, POOF],
  [-0.4, 0.2, SNEEZE],
  [0.45, 0.25, SNEEZE],
];

function puff(b: BeatCtx, n: number, up: number) {
  const c = b.where("cap") ?? [0, 0.3];
  for (let i = 0; i < n; i++) {
    const a = Math.PI * (0.15 + (0.7 * i) / Math.max(1, n - 1));
    b.fx.emit({
      x: c[0] + 0.45 * Math.cos(a),
      y: c[1] + 0.1,
      vx: 0.9 * Math.cos(a),
      vy: up * Math.sin(a),
      g: 1.2,
      life: 1.1,
      strength: 0.1,
      radius: 0.045,
    });
  }
}

const spores: Beat = {
  name: "spores",
  len: 4.3,
  start(b) {
    b.g.marks = FAN.map(() => ({
      kind: "star" as const,
      x: 0,
      y: 0,
      size: 0,
      rot: 0,
      color: SPORE,
    }));
  },
  frame(b, t) {
    const { g, f } = b,
      L = 4.3,
      out = step(t, 3.6, L - 0.1);
    g.face = 0.6 * step(t, 0, 0.5) * (1 - out);
    g.oy = -0.28 * step(t, 0, 0.5) * (1 - out);
    g.zoom = t < 3.6 ? zlerp(1, 0.8, step(t, 0, 0.5)) : zlerp(0.8, 1, backOut(Math.min(1, (t - 3.6) / (L - 3.7))));
    // crouch, boing
    const crouch = step(t, 0.1, POOF - 0.1) * (1 - step(t, POOF - 0.1, POOF));
    f.squash = -0.14 * crouch + 0.14 * bump(t, POOF, POOF + 0.35);
    f.bob = 0.12 * bump(t, POOF, POOF + 0.45);
    // watching the spores float; the tickle; the sneeze
    const watch = hold(t, POOF + 0.2, 2.3, 0.2);
    f.gaze = 1;
    f.ly = 0.75 * watch;
    f.lx = 0.5 * Math.sin((t - POOF) * 3) * watch;
    const wind = step(t, 2.35, SNEEZE - 0.05) * (1 - step(t, SNEEZE, SNEEZE + 0.05));
    f.nod = -0.3 * wind + 0.35 * bump(t, SNEEZE, SNEEZE + 0.35);
    f.lid =
      t > 2.4 && t < SNEEZE + 0.3 ? 0.4 + 0.5 * wind + 0.5 * bump(t, SNEEZE - 0.05, SNEEZE + 0.3) : blink(t, 3.75);
    f.mouth = wind > 0.3 ? 0.3 + 0.3 * wind : t > SNEEZE && t < SNEEZE + 0.25 ? 0.5 : -1;
    f.still = 0.6 * hold(t, 2.35, SNEEZE + 0.4, 0.1);
    g.oy -= 0.04 * bump(t, SNEEZE, SNEEZE + 0.3);
    // the sparkles drift up and sway, then fade; one comes back down to its nose
    const c = b.where("cap") ?? [0, 0.3],
      nose = b.where("mouth") ?? [0, -0.2];
    for (let i = 0; i < FAN.length; i++) {
      const [dx, up, at] = FAN[i]!,
        m = g.marks[i]!;
      const q = (t - at) / 1.8;
      if (q < 0 || q > 1) {
        m.size = 0;
        continue;
      }
      let x = c[0] + 1.4 * dx * Math.sqrt(q) + 0.07 * Math.sin(t * 3 + i),
        y = Math.min(0.75, c[1] + 0.1 + 0.75 * up * Math.sqrt(q) - 0.25 * q * q);
      if (i === 3 && at === POOF) {
        // the tickler: up, then down onto its nose
        const d = step(t, 1.6, 2.35);
        x = x + (nose[0] + 0.08 - x) * d;
        y = y + (nose[1] + 0.12 - y) * d;
        m.size = t > SNEEZE ? 0 : 0.2;
      } else m.size = 0.2 * step(q, 0, 0.1) * (1 - step(q, 0.7, 1));
      m.x = x * WINDOW;
      m.y = y * WINDOW;
      m.rot = t * 2 + i;
    }
    if (b.crossed(POOF)) {
      b.mood("happy"); // its own spores come out too
      puff(b, 8, 1.6);
      b.fx.ripple(0, -0.95, 0.3, 0.12);
    }
    if (b.crossed(2.35)) b.mood("idle");
    if (b.crossed(SNEEZE)) {
      b.mood("surprised"); // the cap pops up
      puff(b, 6, 2.0);
      b.fx.rim(0.4);
    }
    if (b.crossed(SNEEZE + 0.45)) b.mood("happy");
    if (b.crossed(4.0)) b.mood("idle");
  },
};

const LAND = 1.45;

const eat: Beat = {
  name: "eat",
  len: 4.0,
  start(b) {
    b.g.marks = [{ kind: "drop", x: 0, y: 0, size: 0, rot: 0, color: "#b0e0ff" }];
  },
  frame(b, t) {
    const { g, f } = b,
      L = 4.0,
      out = step(t, 3.3, L - 0.1);
    g.face = 0.6 * step(t, 0, 0.5) * (1 - out);
    g.oy = -0.2 * step(t, 0, 0.5) * (1 - out);
    const c = b.where("cap") ?? [0, 0.3];
    // the drop: falls from above the window onto the cap (a wobble left and right), then
    // soaks in
    const d = g.marks[0]!,
      fall = step(t, 0.2, LAND);
    const fx = c[0] + 0.15 * Math.sin(fall * 5) * (1 - fall);
    d.x = fx * WINDOW;
    d.y = (0.95 + (c[1] + 0.22 - 0.95) * fall * fall) * WINDOW;
    d.size = t < LAND ? 0.3 : 0.3 * (1 - step(t, LAND, LAND + 0.5)) * (1 - 0.5 * step(t, LAND, LAND + 0.08));
    d.rot = 0;
    // looks up and shuffles under it; the splash; drinks; grows
    const under = hold(t, 0.2, LAND + 0.1, 0.25);
    f.gaze = 1;
    f.ly = 0.85 * under;
    f.lx = 0.6 * Math.sin(fall * 5) * (1 - fall) * under;
    f.px = 0.05 * Math.sin(fall * 5) * (1 - fall);
    f.mouth = under > 0.5 ? 0.35 : -1;
    const grow = step(t, LAND + 0.3, 2.6) * (1 - step(t, 3.2, 3.8));
    f.squash = -0.1 * bump(t, LAND, LAND + 0.3) + 0.12 * grow;
    f.lid = Math.max(0.9 * hold(t, LAND + 0.3, 2.3, 0.12), blink(t, 3.3)); // eyes shut, savouring
    if (b.crossed(LAND)) {
      b.fx.ripple(c[0] - 0.35, c[1] + 0.05, 0.4, 0.07);
      b.fx.ripple(c[0] + 0.35, c[1] + 0.05, 0.4, 0.07);
      for (const s of [-1, 1])
        b.fx.emit({
          x: c[0] + s * 0.25,
          y: c[1] + 0.25,
          vx: s * 0.9,
          vy: 1.2,
          g: 5,
          life: 0.5,
          strength: 0.25,
          radius: 0.04,
        });
    }
    if (b.crossed(LAND + 0.3)) b.mood("sleepy"); // the nightcap droop: blissful
    if (b.crossed(2.4)) b.mood("happy"); // and the happy spores
    if (b.crossed(3.6)) b.mood("idle");
  },
  post(b, t) {
    // the cap swells as it soaks the drop up
    const k = 1 + 0.18 * step(t, LAND, LAND + 0.6) * (1 - step(t, 3.0, 3.6)) + 0.1 * bump(t, LAND, LAND + 0.25);
    for (const m of b.part("cap")) m.scale.set(m.scale.x * k, m.scale.y * (1 + 0.4 * (k - 1)), m.scale.z * k);
  },
};

export const beats: Beat[] = [spores, eat];
