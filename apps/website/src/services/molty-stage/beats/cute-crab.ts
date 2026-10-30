// Crab: (1) "pinch" — scuttles sideways across the window and back, walks up to the glass,
// raises a claw and pinches it: clack, clack, CLACK, each one a thud and a little crack star
// in the glass; proud, it claps. (2) "eat" — a clam slides in; it grabs it, cracks it open
// with two clacks (the halves fall away), and munches what was inside.
import { backOut, blink, bump, hold, step, WINDOW, zlerp, type Beat, type BeatCtx } from "./api";

const CRACK = "#ffffff";
const SHELL = "#ffd6a0";
const MEAT = "#fff4e8";

/** The claw whose palm is the part's first mesh (where() reads that one): its socket and side. */
function claw(b: BeatCtx) {
  const palm = b.kit.parts["palm"]?.[0] as unknown as { parent?: unknown } | undefined;
  return (b.kit.sock["side"] ?? []).find((r) => r.g === palm?.parent) ?? b.kit.sock["side"]?.[0];
}

/** Bring one claw (by `up`) round to the front, cartoon-big, toward the glass, and open its
 *  pincers by `open` (0 shut … 1 wide). At rest the claws sit at the shell's far sides,
 *  outside the window once it is in close. */
function poseClaw(b: BeatCtx, up: number, open: number) {
  const c = claw(b);
  if (!c) return;
  const g = b.node(c.g),
    s = b.state;
  // the kit re-poses the socket's rotation every frame but not its position or scale:
  // write those from the rest values (kept once), never on top of last frame's
  if (s.x0 === undefined) {
    s.x0 = g.position.x;
    s.y0 = g.position.y;
    s.z0 = g.position.z;
    s.s0 = g.scale.x;
  }
  g.position.set(s.x0, s.y0! + 0.04 * up, s.z0! + 0.08 * up);
  const k = s.s0! * (1 + 0.6 * up);
  g.scale.set(k, k, k);
  g.rotation.y -= c.side * 0.9 * up;
  const palm = b.kit.parts["palm"]?.[0] as unknown as { parent?: unknown } | undefined;
  for (const m of b.part("pinU"))
    if ((m as unknown as { parent?: unknown }).parent === palm?.parent) m.rotation.z += c.side * 0.9 * open;
  for (const m of b.part("pinL"))
    if ((m as unknown as { parent?: unknown }).parent === palm?.parent) m.rotation.z -= c.side * 0.6 * open;
}

const CLACKS = [1.95, 2.35, 2.7];

const pinch: Beat = {
  name: "pinch",
  len: 4.2,
  start(b) {
    b.g.marks = CLACKS.map(() => ({
      kind: "star" as const,
      x: 0,
      y: 0,
      size: 0,
      rot: 0,
      color: CRACK,
    }));
  },
  frame(b, t) {
    const { g, f } = b,
      L = 4.2,
      out = step(t, 3.3, L - 0.1);
    // sideways scuttle: right, left, back to the middle
    const sc = t < 1.3 ? Math.sin((t / 1.3) * Math.PI * 2) : 0;
    g.ox = 0.45 * sc;
    f.bob = 0.025 * Math.abs(Math.sin(t * 22)) * (t < 1.3 ? 1 : 0);
    g.roll = 0.05 * Math.sin(t * 22) * (t < 1.3 ? 1 : 0);
    // up to the glass
    g.face = step(t, 1.0, 1.6) * (1 - out);
    g.pin = g.face;
    g.focusAt = "palm";
    g.focus = 0.75 * step(t, 1.4, 1.9) * (1 - step(t, 2.95, 3.4));
    let jab = 0;
    for (const at of CLACKS) jab = Math.max(jab, bump(t, at - 0.15, at + 0.1));
    g.zoom =
      t < 3.3
        ? zlerp(1, 1.6, step(t, 1.15, 1.8)) * (1 + 0.1 * jab)
        : zlerp(1.6, 1, backOut(Math.min(1, (t - 3.3) / (L - 3.4))));
    f.gaze = 1;
    f.lid = Math.max(blink(t, 1.6), blink(t, 3.6));
    f.squash = 0.05 * jab;
    // the crack stars where the claw hit (they stay a moment)
    const p = b.where("palm") ?? [0.4, 0];
    CLACKS.forEach((at, i) => {
      const m = g.marks[i]!;
      if (b.crossed(at)) {
        m.x = (p[0] + 0.12 * (i - 1)) * WINDOW;
        m.y = (p[1] + 0.12 + 0.05 * i) * WINDOW;
        m.rot = i * 0.7;
        b.fx.ripple(p[0], p[1] + 0.1, 0.55, 0.09);
        b.fx.bolt(p[0], p[1] + 0.1, p[0] + 0.35, p[1] + 0.45, 0.35);
        b.fx.bolt(p[0], p[1] + 0.1, p[0] - 0.3, p[1] + 0.4, 0.3);
        b.fx.rim(0.35 + 0.2 * i);
      }
      m.size = t < at ? 0 : (0.2 + 0.06 * i) * (1 - step(t, 3.0, 3.35)) * (t < at + 0.08 ? 1.6 : 1);
    });
    if (b.crossed(0.05)) b.mood("happy"); // the scuttle (legs go)
    if (b.crossed(1.3)) b.mood("listening");
    if (b.crossed(2.85)) b.mood("happy"); // proud clapping
    if (b.crossed(3.8)) b.mood("idle");
  },
  post(b, t) {
    const up = hold(t, 1.6, 2.9, 0.25);
    if (up <= 0) return;
    // open before each clack, snapped shut at it
    let open = 0;
    for (const at of CLACKS) open = Math.max(open, step(t, at - 0.3, at - 0.12) * (1 - step(t, at - 0.04, at)));
    poseClaw(b, up, open);
  },
};

const GRAB = 1.0,
  CR1 = 1.6,
  CR2 = 2.0,
  BITES = [2.45, 2.8, 3.15];

// a clam: a fan, flat at the hinge; the halves are the same outline cut in two
const CLAM: Array<[number, number]> = [[0, -0.4]];
for (let i = 0; i <= 8; i++) {
  const a = Math.PI * (0.15 + (0.7 * i) / 8);
  CLAM.push([0.55 * Math.cos(a), -0.4 + 0.7 * Math.sin(a)]);
}
const HALF_L = CLAM.slice(0, 6),
  HALF_R: Array<[number, number]> = [[0, -0.4], ...CLAM.slice(5)];
const DISC: Array<[number, number]> = Array.from({ length: 8 }, (_, i) => [
  Math.cos((i / 8) * Math.PI * 2),
  Math.sin((i / 8) * Math.PI * 2),
]);

const eat: Beat = {
  name: "eat",
  len: 4.2,
  start(b) {
    b.g.marks = [
      { kind: "poly", x: 0, y: 0, size: 0, rot: 0, color: SHELL, pts: CLAM },
      { kind: "poly", x: 0, y: 0, size: 0, rot: 0, color: SHELL, pts: HALF_L },
      { kind: "poly", x: 0, y: 0, size: 0, rot: 0, color: SHELL, pts: HALF_R },
      { kind: "poly", x: 0, y: 0, size: 0, rot: 0, color: MEAT, pts: DISC },
    ];
  },
  frame(b, t) {
    const { g, f } = b,
      L = 4.2,
      out = step(t, 3.45, L - 0.1);
    g.face = 0.8 * step(t, 0, 0.5) * (1 - out);
    g.pin = g.face;
    g.zoom = zlerp(1, 1.15, step(t, 0, 0.6) * (1 - out));
    g.oy = 0.22 * step(t, 0, 0.6) * (1 - out);
    // a lunge down at the clam on each crack
    f.nod = 0.15 * (bump(t, CR1 - 0.15, CR1 + 0.15) + bump(t, CR2 - 0.15, CR2 + 0.15));
    const [whole, hl, hr, meat] = g.marks as [
      (typeof g.marks)[0],
      (typeof g.marks)[0],
      (typeof g.marks)[0],
      (typeof g.marks)[0],
    ];
    const mo = b.where("mouth") ?? [0, -0.1];
    // the clam slides in from the right along the floor and stops in front of the crab
    // (fixed on the glass: at the crab's own claw it was clipped by the window's edge)
    const inn = step(t, 0.1, GRAB);
    const cx = (1.25 + (0.32 - 1.25) * inn) * WINDOW,
      cy = -0.5 * WINDOW;
    const shake = 0.02 * Math.sin(t * 60) * (bump(t, CR1 - 0.05, CR1 + 0.15) + bump(t, CR2 - 0.05, CR2 + 0.15));
    whole.x = cx + shake;
    whole.y = cy;
    whole.size = t < CR2 ? 0.34 : 0;
    whole.rot = 0.25 * Math.sin(t * 7) * (1 - inn);
    // cracked: the halves fall away either side
    const fall = Math.max(0, t - CR2);
    for (const [m, s] of [
      [hl, -1],
      [hr, 1],
    ] as const) {
      m.x = cx + s * 0.4 * fall;
      m.y = cy + 0.6 * fall - 3 * fall * fall;
      m.rot = s * 2.2 * fall;
      m.size = t < CR2 ? 0 : 0.34 * (1 - step(t, CR2 + 0.55, CR2 + 0.75));
    }
    // the meat goes to the mouth and gets smaller with each bite
    const toMouth = step(t, CR2 + 0.05, BITES[0]! - 0.05);
    meat.x = (cx / WINDOW + (mo[0] - cx / WINDOW) * toMouth) * WINDOW;
    meat.y = (cy / WINDOW + (mo[1] - 0.05 - cy / WINDOW) * toMouth) * WINDOW;
    let left = 1;
    for (const at of BITES) if (t > at) left -= 1 / BITES.length;
    meat.size = t < CR2 ? 0 : 0.11 * Math.max(0, left);
    let chomp = 0;
    for (const at of BITES) chomp = Math.max(chomp, bump(t, at - 0.12, at + 0.12));
    f.mouth = chomp > 0.05 ? 0.6 * chomp : 0;
    f.squash = 0.05 * chomp + 0.06 * (bump(t, CR1 - 0.1, CR1 + 0.15) + bump(t, CR2 - 0.1, CR2 + 0.15));
    f.gaze = 1;
    f.lx = Math.max(-1, Math.min(1, (meat.size > 0 ? meat.x : whole.x) / WINDOW - mo[0]));
    f.ly = -0.4 * (1 - toMouth);
    f.lid = blink(t, 3.6);
    for (const at of [CR1, CR2])
      if (b.crossed(at)) {
        b.fx.ripple(cx / WINDOW, cy / WINDOW - 0.1, 0.5, 0.09);
        b.fx.bolt(cx / WINDOW, cy / WINDOW, cx / WINDOW + 0.3, cy / WINDOW + 0.35, 0.35);
      }
    for (const at of BITES)
      if (b.crossed(at)) {
        b.fx.ripple(mo[0] - 0.2, mo[1] - 0.3, 0.25, 0.06);
        b.fx.ripple(mo[0] + 0.25, mo[1] - 0.35, 0.2, 0.05);
      }
    if (b.crossed(CR2)) b.mood("surprised");
    if (b.crossed(CR2 + 0.4)) b.mood("idle");
    if (b.crossed(3.3)) b.mood("happy");
    if (b.crossed(3.95)) b.mood("idle");
  },
  post(b, t) {
    const up = hold(t, 0.6, 2.2, 0.3);
    if (up <= 0) return;
    let open = 0.8 * step(t, 0.5, 0.85) * (1 - step(t, GRAB - 0.1, GRAB));
    for (const at of [CR1, CR2])
      open = Math.max(open, 0.6 * step(t, at - 0.3, at - 0.15) * (1 - step(t, at - 0.05, at)));
    poseClaw(b, up, open);
  },
};

export const beats: Beat[] = [pinch, eat];
