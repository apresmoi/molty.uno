// Turtle: (1) "shell" — it plods up to the glass, slowly bonks it with its head, and pulls
// its head into its shell; the shell wobbles; then the eyes peek out, look one way and the
// other, and the head comes all the way out, pleased. (2) "eat" — a strawberry: it cranes
// its neck out, opens wide, slowly takes three bites (the berry gets smaller), chews each one
// for a long time, and closes its eyes in contentment.
import { WINDOW, backOut, blink, bump, clamp01, hold, lerp, smooth, step, zlerp, type Beat, type BeatCtx } from "./api";

type Labeled = Beat & { label: string };
type Pt = Array<[number, number]>;

// The head pivot's rest point and the tuck offsets from the spec (pack-e.js: head.pivot,
// TURTLE_TUCK). The kit (and the turtle's own peek hook) re-pose the pivot every frame; a beat
// sets it absolutely, so a random peek of the hook never fights the beat.
const NECK = [0, 0.26, 0.16] as const,
  TUCK = { z: -0.2, y: -0.04, s: -0.6 };
/** k: 0 out (rest) … 1 all the way in; negative cranes the neck out. */
const posHead = (b: BeatCtx, k: number) => {
  const hp = b.node(b.kit.headPivot);
  hp.position.set(NECK[0], NECK[1] + TUCK.y * k, NECK[2] + TUCK.z * k);
  const s = 1 + TUCK.s * Math.max(0, k);
  hp.scale.set(s, s, s);
};

// ---------------------------------------------------------------- shell: bonk, hide, peek

const BONK = 1.35,
  IN = 1.65, // all the way in
  PEEK = 2.45, // eyes out
  OUT = 3.2; // head out

const shell: Labeled = {
  name: "shell",
  label: "Hides in its shell",
  len: 4.6,
  start(b) {
    b.g.marks = [{ kind: "!", x: 0.42, y: 0.45, size: 0, rot: 0.15, color: "#ffffff" }];
    b.mood("idle");
  },
  frame(b, t) {
    const { g, f } = b,
      L = 4.6;
    const back = step(t, 3.55, L - 0.1);
    g.face = step(t, 0, 0.6) * (1 - back);
    g.pin = g.face;
    // a slow plod up to the glass (little bobs), a soft bonk, then it stays close
    if (t < BONK) {
      const x = t / BONK;
      g.zoom = lerp(1, 1.7, smooth(x));
      f.bob = 0.025 * Math.abs(Math.sin(t * 7)) * (1 - x);
    } else if (t < 3.55) {
      const h = t - BONK;
      g.zoom = 1.7 * (1 - 0.04 * Math.exp(-h * 6) * Math.cos(h * 14));
    } else g.zoom = zlerp(1.7, 1, backOut(clamp01((t - 3.55) / (L - 3.65))));
    g.press = 0.12 * bump(t, BONK - 0.05, BONK + 0.25);
    g.marks[0]!.size = 0.32 * hold(t, BONK + 0.05, BONK + 0.55, 0.08);
    // in the shell: it trembles twice
    const hidden = hold(t, IN, PEEK, 0.1);
    g.roll = 0.045 * Math.sin(t * 36) * hidden * (hold(t, 1.8, 2.0, 0.05) + hold(t, 2.15, 2.3, 0.05));
    f.still = 1;
    f.gaze = 1;
    f.lx = t < PEEK + 0.15 ? 0 : t < 2.75 ? -0.8 : t < 3.05 ? 0.8 : 0;
    f.lid = Math.max(blink(t, 0.7), blink(t, 3.1), t >= BONK && t < BONK + 0.25 ? 1 : 0);
    if (b.crossed(BONK)) {
      b.fx.rim(0.45);
      b.mood("surprised");
    }
    if (b.crossed(IN)) b.fx.ripple(0, -0.95, 0.35, 0.12); // the shell settles
    if (b.crossed(IN + 0.1)) b.mood("idle");
    if (b.crossed(PEEK)) b.mood("listening");
    if (b.crossed(OUT + 0.1)) b.mood("happy");
    if (b.crossed(4.0)) b.mood("idle");
  },
  post(b, t) {
    // in fast, then the eyes peek out (half way), then all the way out, slowly
    const k = step(t, BONK + 0.05, IN) * (1 - 0.45 * step(t, PEEK - 0.3, PEEK)) * (1 - step(t, OUT - 0.3, OUT + 0.15));
    posHead(b, k);
  },
};

// ---------------------------------------------------------------- eat: a strawberry, slowly

const BERRY: Pt = [
  [0, -1],
  [0.62, -0.45],
  [0.8, 0.2],
  [0.55, 0.62],
  [0, 0.72],
  [-0.55, 0.62],
  [-0.8, 0.2],
  [-0.62, -0.45],
];
const TOP: Pt = [
  [-1, 0],
  [-0.35, -0.3],
  [0, 0.1],
  [0.35, -0.3],
  [1, 0],
  [0, 0.45],
];
const BITES = [1.05, 1.95, 2.85],
  OPEN = 0.3; // mouth opens this long before each bite

const eat: Labeled = {
  name: "eat",
  label: "Nibbles a strawberry",
  len: 4.5,
  start(b) {
    b.g.marks = [
      { kind: "poly", x: 0, y: 0, size: 0, rot: 0, color: "#ffa3ad", pts: BERRY },
      { kind: "poly", x: 0, y: 0, size: 0, rot: 0, color: "#c8f2b2", pts: TOP },
    ];
    b.mood("idle");
  },
  frame(b, t) {
    const { g, f, state: s } = b,
      L = 4.5;
    const back = step(t, 3.75, L - 0.1);
    g.face = step(t, 0, 0.5) * (1 - back);
    g.pin = g.face;
    g.zoom = t < 3.75 ? zlerp(1, 1.4, step(t, 0.1, 0.9)) : zlerp(1.4, 1, backOut(clamp01((t - 3.75) / (L - 3.85))));
    // the strawberry sits in front of the mouth (placed once, as the neck stretches out)
    if (b.crossed(0.5) || s.bx === undefined) {
      const m = b.where("mouth") ?? [0, -0.25];
      s.bx = m[0];
      s.by = m[1] - 0.2;
    }
    let left = 1;
    for (const a of BITES) left -= 0.27 * step(t, a, a + 0.08);
    const gone = t > BITES[2]! + 0.04 ? 0 : 1;
    const sz = 0.15 * Math.sqrt(Math.max(0, left)) * gone * step(t, 0, 0.25);
    const berry = g.marks[0]!,
      top = g.marks[1]!;
    berry.x = s.bx! * WINDOW;
    berry.y = s.by! * WINDOW;
    berry.size = sz;
    top.x = berry.x;
    top.y = berry.y + 0.75 * sz;
    top.size = 0.75 * sz;
    // a slow mouth: open wide, chomp, then a long slow chew; eyes on the berry
    let mouth = 0;
    for (const a of BITES)
      mouth = Math.max(mouth, 0.8 * step(t, a - OPEN, a - 0.04) * (1 - step(t, a - 0.04, a + 0.02)));
    const chew = (a: number) => 0.25 * Math.max(0, Math.sin((t - a - 0.15) * 7.5)) * hold(t, a + 0.15, a + 0.75, 0.1);
    for (const a of BITES) mouth = Math.max(mouth, chew(a));
    f.mouth = t > 0.6 && t < 3.7 ? mouth : -1;
    f.nod = 0.12 * hold(t, 0.6, 3.6, 0.3);
    f.gaze = 1;
    f.ly = -0.5 * hold(t, 0.4, 3.0, 0.3);
    // contented: slow, happy half-closed eyes after the last bite
    f.lid = t > 3.1 && t < 3.75 ? 0.9 : Math.max(blink(t, 0.45), blink(t, 2.4));
    for (const a of BITES) if (b.crossed(a)) b.fx.ripple(s.bx!, s.by! - 0.15, 0.25, 0.07);
    if (b.crossed(3.15)) b.mood("happy");
    if (b.crossed(3.8)) b.mood("idle");
  },
  post(b, t) {
    // cranes its neck out toward the berry, slowly, and draws it back at the end
    posHead(b, -0.4 * step(t, 0.2, 0.9) * (1 - step(t, 3.3, 3.9)));
  },
};

export const beats: Beat[] = [shell, eat];
