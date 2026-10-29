// The glass-beat API: short scenes (2.5–4.5 s) a Molty plays at its round window on the
// landing page, now and then between its dance beats. Every creature can have its own
// ("signature") scenes in beats/<creature-id>.ts; the generic ones (generic.ts) are the
// fallback and the variety in between. registry.ts lists them; player.ts runs them.
//
// WRITING A BEAT
//   1. Create beats/<creature-id>.ts (the catalog id, e.g. cute-snail.ts) exporting
//        export const beats: Beat[] = [{ name: "shell", len: 3.6, start(b) {…}, frame(b, t) {…} }];
//      and add it to SIGNATURES in registry.ts (one import + one line).
//   2. `frame(b, t)` runs every drawn frame (~30 fps) with `t` seconds since the start. Before
//      each call `b.g` (the glass: camera-ish transform of the whole Molty) and `b.f` (the
//      face: gaze, lids, mouth, head and body offsets) are reset to neutral, so a frame sets
//      only what it changes and the end of the beat (t → len) must ease everything back to
//      neutral itself (the dance resumes from the rest pose). Write every channel as a pure
//      function of `t` (step/zlerp/blink/bump below), not as accumulated state.
//   3. One-shots (a mood switch, a fluid hit) go behind `if (b.crossed(at))`, which is true on
//      exactly one frame. `start(b)` runs once first (set up `b.g.marks`, a mood).
//   4. To move the creature's own parts (a snail's head into its shell, a whale's spout), use
//      `post(b, t)`: it runs after the kit posed the creature and before its triangles are
//      read. Get parts with `b.part(name)` / `b.socket(name)` / `b.kit` (head pivot, body
//      group…). Anything reached through `b.part` / `b.socket` / `b.node` is snapshotted and
//      restored when the beat ends; parts the kit re-poses every frame need no restore.
//      The part names are in the creature's spec (creatures/js/cute/pack-*.js).
//   5. Fluid effects (the fluid is the particle system): `b.fx` — ripples, pushes, a ring
//      thud, and particles (`emit`) that leave an impulse trail every frame: rain, spouts,
//      sparks. Coordinates are WINDOW units: (0, 0) the slot's centre, x right, y up, 1 = the
//      round window's radius. `b.where("mouth" | "eye" | "eyes" | "face" | <part>)` says where
//      a point of the Molty was drawn last frame, in window units.
//   6. Marks (`b.g.marks`, MoltyMark in moltyMesh.ts) are flat shapes on the glass: "?", "!",
//      "star", "z", "drop", "blot", "poly" (a small prop). Their x/y/size are FIT units
//      (window radius = WINDOW = 0.85); white or very light colours read best on any body.
//      Dark colours turn into solid "feature" blocks (the renderer's dark = eye/mouth ink).
//      A big light mark that hides the Molty needs `exposureAnchor()` beside it (see there).
//   7. Keep it cheap: no allocation-heavy work per frame, ≤ ~12 live particles, a few
//      ripples per second. Never put an impulse on the Molty's face (it carves a crater);
//      ripples round the rim and below read as the glass/floor answering.
//   8. Tone: curious, silly, characterful. No kisses, hearts or blush (`g.blush` defaults to
//      false at the glass).
//
// LOOKING AT A BEAT (dev-only hook, not in normal page loads)
//   Open the page with ?beats (e.g. http://127.0.0.1:4392/?beats). Then in the console:
//     __moltyBeats.slots()                       → [{ i, slot, id, beat }] (gallery = picker)
//     __moltyBeats.pick(i, "cute-snail")         → put that Molty in slot i (the gallery slot
//                                                  takes any of the 26; wait ~1.5 s to form)
//     __moltyBeats.list("cute-snail")            → its beat names (signatures, then generic)
//     __moltyBeats.play(i, "shell")              → play that beat on slot i now
//     __moltyBeats.inspect(i)                    → the slot's last polygons: count, NaNs, bounds
//   Headless capture: `node glass3-capture.mjs OUT 1440 900 <slot> <beat> [molty-id]` (in the
//   PM's scratchpad) builds frames every ~100 ms, sheet.jpg and perf.json. Slots in page order:
//   0 hero (cycles duck / octopus / bunny), 1 gallery (picker: any Molty), 2 bridge, 3 bake.
import type { CuteCreature, RigObject } from "@creatures/cute/kit.js";
import type { MoltyGlass } from "../moltyMesh";

/** The window's radius in the rig's fit units (marks, g.ox / g.oy are in fit units). */
export const WINDOW = 0.85;

/** The renderer auto-exposes a slot over its darkest..brightest covered cells, so a big
 *  flat light mark (a water band, a fog) that hides most of the Molty becomes the darkest
 *  thing and fades to nothing. Add this mark too: a tiny black square in the slot's corner,
 *  outside the round window (never drawn), that pins the dark end of the exposure. */
export const exposureAnchor = () => ({
  kind: "poly" as const,
  x: -0.8,
  y: -0.8,
  size: 0.05,
  rot: 0,
  color: "#000000",
  pts: [
    [-1, -1],
    [1, -1],
    [1, 1],
    [-1, 1],
  ] as Array<[number, number]>,
});

export type Mood = "idle" | "listening" | "thinking" | "speaking" | "happy" | "sleepy" | "surprised";

/** What a beat does to the mood pose this frame (all neutral after the per-frame reset). */
export interface Face {
  /** 0…1 how far the eyes take (lx, ly) over the mood's own look (-1…1, x right, y up). */
  gaze: number;
  lx: number;
  ly: number;
  /** Lid (0 open … 1 shut; ≥ 0.8 draws ‿ ‿) and mouth (0…1) overrides; < 0 leaves the mood's. */
  lid: number;
  mouth: number;
  /** 0…1 how much of the mood's own bob / squash / nod is removed (holding still). */
  still: number;
  /** Head roll (radians, + counter-clockwise) taking over by `tiltW` (0…1). */
  tilt: number;
  tiltW: number;
  /** Added: head turn (yaw, radians), head nod (+ down), body bob (fit-ish units of creature
   *  height), squash (+ taller), body pitch (+ forward), lean (body roll), sideways shift. */
  turn: number;
  nod: number;
  bob: number;
  squash: number;
  pitch: number;
  lean: number;
  px: number;
}

/** A particle: moves (window units / s, gravity `g` pulls y down), leaves a ripple each frame. */
export interface Particle {
  x: number;
  y: number;
  vx: number;
  vy: number;
  /** Gravity (window units / s²); 0 for rain at constant speed. */
  g?: number;
  /** Seconds it lives. */
  life: number;
  /** Ripple strength per frame (0.08–0.3) and radius (window units, ~0.04–0.1). */
  strength: number;
  radius?: number;
}

export interface Fx {
  /** A ripple (a bump in the fluid's height) at a window point. */
  ripple(x: number, y: number, strength?: number, radius?: number): void;
  /** Push the flow at a window point by (dx, dy) window units (it carries the ink with it). */
  push(x: number, y: number, dx: number, dy: number, radius?: number): void;
  /** Ripples just outside the window, round its sides and foot (never above: the headings). */
  rim(strength: number, n?: number): void;
  /** A particle that trails ripples (capped; the oldest is dropped). */
  emit(p: Particle): void;
  /** A jagged bolt of ripples from (x0, y0) to (x1, y1), all at once. */
  bolt(x0: number, y0: number, x1: number, y1: number, strength?: number): void;
}

export interface BeatCtx {
  /** The glass (whole-Molty transform, marks, tint); reset before each frame but `marks`. */
  readonly g: MoltyGlass;
  /** The face/pose overrides; reset before each frame. */
  readonly f: Face;
  /** Seconds since the last frame. */
  readonly dt: number;
  /** True on the one frame that crosses `at` seconds. */
  crossed(at: number): boolean;
  /** Switch the mood engine (eye shapes: happy ^ ^, sleepy half-lids, surprised O O,
   *  listening wide; thinking moves the mouth aside). */
  mood(m: Mood): void;
  /** The posed creature (scene graph), for `post`. */
  readonly kit: CuteCreature;
  /** The creature's parts / socket groups by name, snapshotted for restore at the end. */
  part(name: string): RigObject[];
  socket(name: string): RigObject[];
  /** Snapshot any other node (e.g. `b.kit.bodyG`) before moving it in `post`. */
  node(o: RigObject): RigObject;
  /** Where a point of the Molty was drawn last frame (window units), or null. */
  where(at: string): [number, number] | null;
  /** The eye's radius in fit units (zoom so the eye fills the window: 0.5 / eyeR). */
  readonly eyeR: number;
  readonly fx: Fx;
  /** Scratch for a beat's own one-shot state (a random side picked at start, …). */
  readonly state: Record<string, number>;
  /** A deterministic 0…1 number per beat play and `k` (variety without Math.random). */
  rand(k: number): number;
}

export interface Beat {
  /** Unique within a creature; the generic ones are eye, confused, bonk, press, peek. */
  name: string;
  /** Seconds (2.5–4.5). */
  len: number;
  /** Once before the first frame (marks, a mood). The beat starts from mood "listening". */
  start?(b: BeatCtx): void;
  /** Every frame. */
  frame(b: BeatCtx, t: number): void;
  /** Every frame, after the kit posed the creature (move parts here). */
  post?(b: BeatCtx, t: number): void;
}

// ---------------------------------------------------------------- timeline helpers

export const clamp01 = (x: number) => (x < 0 ? 0 : x > 1 ? 1 : x);
export const lerp = (a: number, b: number, x: number) => a + (b - a) * x;
/** Smoothstep on 0…1. */
export const smooth = (x: number) => (x <= 0 ? 0 : x >= 1 ? 1 : x * x * (3 - 2 * x));
/** 0 → 1 over [a, b], smoothed. */
export const step = (t: number, a: number, b: number) => smooth((t - a) / (b - a));
/** 1 inside [a, b] with smoothed edges of `e` seconds, 0 outside. */
export const hold = (t: number, a: number, b: number, e = 0.15) => step(t, a, a + e) * (1 - step(t, b - e, b));
/** A sine bump over [a, b] (0 → 1 → 0). */
export const bump = (t: number, a: number, b: number) =>
  t <= a || t >= b ? 0 : Math.sin((Math.PI * (t - a)) / (b - a));
/** Overshoots ~4 % before settling at 1. */
export const backOut = (x: number) => 1 + 1.9 * Math.pow(x - 1, 3) + 0.9 * Math.pow(x - 1, 2);
export const easeOut = (x: number) => 1 - Math.pow(1 - clamp01(x), 3);
export const easeIn = (x: number) => Math.pow(clamp01(x), 3);
/** Zoom is felt in ratios: interpolate it in log space. */
export const zlerp = (a: number, b: number, x: number) => a * Math.pow(b / a, x);
/** A blink at `at`: shut in 0.08 s, open by 0.22 s. */
export const blink = (t: number, at: number) => {
  const h = t - at;
  return h < 0 || h > 0.22 ? 0 : h < 0.08 ? h / 0.08 : 1 - (h - 0.08) / 0.14;
};
/** The usual approach / leave envelope: 0 → 1 over [0, inn], 1 → 0 over [out, len - 0.1]. */
export const presence = (t: number, len: number, inn: number, out: number) =>
  step(t, 0, inn) * (1 - step(t, out, len - 0.1));
