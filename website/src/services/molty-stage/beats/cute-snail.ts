// Snail: (1) "shell" — startled, it pulls its head and foot into its shell, the shell
// trembles, the eyes peek out and look round, then it comes out again. (2) "stalks" — turned
// three-quarters on, its eye stalks stretch out toward the glass, the eyes growing as they
// come, look at you, swing apart goofily, and spring back with a boing. (3) "eat" — a
// lettuce leaf, munched slowly, bite by bite.
import { backOut, blink, bump, clamp01, hold, lerp, step, zlerp, type Beat } from "./api";

/** Uniform scale (the rig types expose only `set`). */
const sc = (o: { scale: { set(x: number, y: number, z: number): unknown } }, k: number) => o.scale.set(k, k, k);

// the stalks' rest pose in the spec (pack-d.js): tilt, cup depth; the kit's update re-sets
// the rest every frame except these
const STALK_RX = 0.18,
  CUP_Z = 0.13;

const shell: Beat = {
  name: "shell",
  len: 4.2,
  start(b) {
    b.g.marks = [{ kind: "!", x: 0.42, y: 0.5, size: 0, rot: 0.15, color: "#ffffff" }];
    b.mood("surprised");
  },
  frame(b, t) {
    const { g, f } = b;
    g.face = 0.35 * step(t, 0, 0.4) * (1 - step(t, 3.6, 4.1));
    g.zoom = 1.12 - 0.12 * step(t, 3.6, 4.1);
    g.marks[0]!.size = 0.36 * hold(t, 0.05, 0.6, 0.08);
    // hidden: the shell trembles twice
    const hidden = hold(t, 0.55, 2.2, 0.1);
    g.roll = 0.05 * Math.sin(t * 40) * hidden * (hold(t, 0.9, 1.2, 0.05) + hold(t, 1.55, 1.8, 0.05));
    // peeking: looks one way, the other, at you
    f.gaze = 1;
    f.lx = t < 2.3 ? 0 : t < 2.7 ? -0.8 : t < 3.1 ? 0.8 : 0;
    f.lid = blink(t, 3.15);
    f.still = 1;
    if (b.crossed(0.5)) b.mood("idle");
    if (b.crossed(0.6)) b.fx.ripple(-0.2, -0.75, 0.35, 0.12); // the shell settles
    if (b.crossed(3.4)) b.mood("happy");
    if (b.crossed(3.9)) b.mood("idle");
  },
  post(b, t) {
    const k = b.kit;
    // how far it is inside: in by 0.55 s, eyes out at 2.2, all out from 3.3
    const tuck = clamp01(step(t, 0.25, 0.55) * (1 - 0.55 * step(t, 2.1, 2.35)) * (1 - step(t, 3.25, 3.7)));
    // the head shrinks back toward the shell's mouth; the kit re-poses the head pivot every frame
    const hp = k.headPivot;
    hp.position.set(lerp(hp.position.x, -0.02, tuck), lerp(hp.position.y, 0.3, tuck), lerp(hp.position.z, 0.02, tuck));
    const s = lerp(1, 0.04, tuck);
    hp.scale.set(hp.scale.x * s, hp.scale.y * s, hp.scale.z * s);
    // the foot pulls in under the shell
    for (const m of b.part("body")) {
      m.scale.set(lerp(1, 0.45, tuck), lerp(1, 0.8, tuck), lerp(1, 0.7, tuck));
      m.position.x = lerp(0, -0.14, tuck);
    }
  },
};

const stalks: Beat = {
  name: "stalks",
  len: 4.2,
  frame(b, t) {
    const { g, f } = b,
      L = 4.2,
      out = step(t, 3.3, L - 0.1);
    // three-quarters on (not square to the glass): the stalks' length shows as they reach
    g.face = 0.2 * step(t, 0, 0.6) * (1 - out);
    g.yaw = 0.55 * step(t, 0, 0.6) * (1 - out); // its front turned toward screen-right: the stalks show their length
    g.pin = 0.8 * step(t, 0, 0.9) * (1 - out);
    g.focusAt = "eyes";
    g.focus = step(t, 0.3, 1.3) * (1 - out);
    g.zoom = t < 3.3 ? zlerp(1, 1.3, step(t, 0.3, 1.6)) : zlerp(1.3, 1, backOut(Math.min(1, (t - 3.3) / (L - 3.4))));
    f.still = 0.8;
    f.mouth = 0;
    f.gaze = 1;
    // at the glass: both eyes on you, then each its own way (a goofy split), then on you again
    const split = hold(t, 2.0, 2.6, 0.12);
    f.lx = 0;
    f.ly = 0;
    b.state.split = split;
    f.lid = Math.max(blink(t, 1.75), blink(t, 2.85));
    if (b.crossed(1.55)) b.fx.rim(0.3); // eyes on the glass
  },
  post(b, t) {
    // stretch the stalks forward and carry the eyes (and their cups) to the new tips, the eyes
    // growing as they come (the camera has no perspective: the growth sells the approach)
    const reach = step(t, 0.3, 1.5) * (1 - step(t, 3.2, 3.55)),
      boing = 0.25 * Math.sin((t - 3.55) * 22) * Math.exp(-(t - 3.55) * 6) * (t > 3.55 ? 1 : 0),
      split = b.state.split ?? 0;
    for (const side of [1, -1]) {
      const st = b.part(`stalk${side}`)[0],
        cup = b.part(`eyecup${side}`)[0],
        e = b.kit.eyes.find((x) => x.side === side);
      if (!st || !cup || !e) continue;
      // the stalks come together as they reach (no splaying), wobbling a little; at the glass
      // each eye swings its own way
      const look = reach * (0.05 * Math.sin(t * 2.3 + side * 0.9) - 0.25 * side * split);
      const rx0 = STALK_RX,
        rz0 = st.rotation.z,
        rx = rx0 + 1.0 * reach,
        rz = rz0 * (1 - 0.7 * reach) + look,
        k = 1 + 2.6 * reach + boing;
      st.rotation.x = rx;
      st.rotation.z = rz;
      st.scale.y *= k;
      // and thicker: at the rig's own radius a long stalk falls below a cell and vanishes
      // (absolute: the kit re-sets only scale.y each frame, so *= would compound)
      st.scale.x = st.scale.z = 1 + 1.4 * reach;
      // the cylinder is centred: its top end moves by half its length along its axis
      const H = 0.095;
      const dir = (ax: number, az: number, len: number) => [
        -Math.sin(az) * len,
        Math.cos(az) * Math.cos(ax) * len,
        Math.cos(az) * Math.sin(ax) * len,
      ];
      const a = dir(rx0, rz0, H),
        c = dir(rx, rz, H * k);
      const dx = c[0]! - a[0]!,
        dy = c[1]! - a[1]!,
        dz = c[2]! - a[2]!;
      e.g.position.set(e.g.position.x + dx, e.g.position.y + dy, e.g.position.z + dz);
      cup.position.set(cup.position.x + dx, cup.position.y + dy, CUP_Z + dz);
      const grow = 1 + 0.6 * reach;
      sc(b.node(e.g), grow);
      sc(cup, grow);
    }
  },
};

// ---------------------------------------------------------------- eat: a lettuce leaf
const LETTUCE = "#8fe35e"; // deeper than the pistachio snail
const FRILL = "#e9ffd2";
const RING: Array<[number, number]> = [];
for (let i = 0; i < 10; i++)
  RING.push([0.5 * Math.cos((i / 10) * Math.PI * 2), 0.5 * Math.sin((i / 10) * Math.PI * 2)]);
// the leaf: ruffled blobs (fit units), nearest the snail's mouth first; [x, y, size, eaten at]
const LEAF: Array<[number, number, number, number]> = [
  [0.42, -0.45, 0.4, 1.2],
  [0.58, -0.3, 0.42, 1.95],
  [0.68, -0.5, 0.38, 2.7],
  [0.52, -0.62, 0.34, 3.3],
];

const eat: Beat = {
  name: "eat",
  len: 4.3,
  start(b) {
    b.g.marks = LEAF.flatMap(([x, y]) => [
      { kind: "poly" as const, x, y, size: 0, rot: 0, color: LETTUCE, pts: RING },
      { kind: "poly" as const, x: x + 0.02, y: y + 0.03, size: 0, rot: 0, color: FRILL, pts: RING },
    ]);
  },
  frame(b, t) {
    const { g, f } = b;
    g.face = 0.3 * hold(t, 0, 4.2, 0.4);
    g.ox = -0.3 * hold(t, 0, 4.2, 0.4);
    g.oy = 0.08 * hold(t, 0, 4.2, 0.4);
    let chew = 0;
    for (let i = 0; i < LEAF.length; i++) {
      const [, , s, at] = LEAF[i]!,
        leaf = g.marks[2 * i]!,
        frill = g.marks[2 * i + 1]!;
      // pops in, then shrinks over its slow bite
      const k = step(t, 0.1 + 0.08 * i, 0.4 + 0.08 * i) * (1 - step(t, at - 0.45, at));
      leaf.size = s * k;
      frill.size = 0.35 * s * k;
      leaf.rot = frill.rot = 0.2 * Math.sin(t * 2 + i);
      chew = Math.max(chew, bump(t, at - 0.6, at + 0.1));
    }
    // slow munching: the mouth works open and shut, the head leans in, eyes half shut in bliss
    f.mouth = chew > 0.05 ? 0.4 * chew * (0.5 + 0.5 * Math.sin(t * 9)) : 0;
    f.nod = 0.15 * chew;
    f.gaze = 1;
    f.lx = t < 3.5 ? 0.7 : 0;
    f.ly = t < 3.5 ? -0.6 : 0;
    f.lid = t > 1.0 && t < 3.5 ? 0.35 : blink(t, 3.8);
    for (const [, , , at] of LEAF) if (b.crossed(at)) b.fx.ripple(0.5, -0.75, 0.15, 0.06);
    if (b.crossed(3.5)) b.mood("happy");
    if (b.crossed(4.1)) b.mood("idle");
  },
};

export const beats: Beat[] = [shell, stalks, eat];
