// The device reveal: the first time a round screen scrolls into view, the board it sits in turns
// into place and its glass comes in. Pure physics, sampled: `revealFrame(kind, u)` is the whole
// pose at progress u (0 → 1) of the kind's duration, so the component, the tests and a
// screenshot seek all read the same motion. At u = 1 every kind is exactly at rest (REST).
//
// One motion language for every section: a short anticipation (it winds a little further away,
// eased in and out), then a damped spring releases it into place (mass on a spring: it
// accelerates, overshoots once by a few degrees and settles). A lid falls under gravity and
// bounces once; a coin arrives already spinning and the same spring catches it.
//
// Angles are degrees. `yaw` turns the board about the screen's vertical (+ = its face toward
// screen-left: polycss camera rotY), `pitch` about the horizontal (- = its face tipped down,
// top edge toward the viewer: polycss camera rotX - 90).

export type RevealKind = "arrive" | "turn" | "lid" | "rise" | "coin" | "quick";

export interface RevealFrame {
  yaw: number;
  pitch: number;
  /** Drop below its place, as a fraction of the screen's diameter (negative: above). */
  lift: number;
  /** The whole device's opacity (it arrives). */
  opacity: number;
  /** 0…1: the screen is off (a dark disc over it), so the fluid behind cannot show unturned. */
  cover: number;
  /** The glass shell's swing about its top edge (deg, negative = open). */
  lid: number;
  /** Keep the slot's Molty melted (the fluid cannot turn with the board, so it waits). */
  hold: boolean;
  /** Turn the Molty with the board (deg; the slot's rig yaw), or null to leave it be. */
  moltyYaw: number | null;
}

export const REST: RevealFrame = {
  yaw: 0,
  pitch: 0,
  lift: 0,
  opacity: 1,
  cover: 0,
  lid: 0,
  hold: false,
  moltyYaw: null,
};

/** Seconds each kind takes (every one ≤ 1.2 s). */
export const REVEAL_SECONDS: Record<RevealKind, number> = {
  arrive: 1.15,
  turn: 1.15,
  lid: 1.05,
  rise: 1.15,
  coin: 1.2,
  quick: 0.7,
};

const clamp01 = (x: number) => (x <= 0 ? 0 : x >= 1 ? 1 : x);
const smooth = (x: number) => {
  const t = clamp01(x);
  return t * t * (3 - 2 * t);
};

/** A damped spring (unit mass; natural frequency ω rad/s, damping ratio ζ < 1) released at
 *  x0 with velocity v0, pulled to 0: its position t seconds later (closed form, no stepping). */
export function spring(t: number, x0: number, v0: number, omega: number, zeta: number): number {
  if (t <= 0) return x0;
  const wd = omega * Math.sqrt(1 - zeta * zeta),
    k = zeta * omega;
  return Math.exp(-k * t) * (x0 * Math.cos(wd * t) + ((v0 + k * x0) / wd) * Math.sin(wd * t));
}

/** Anticipation, then the spring: from `a` it winds `wind` further away over `wt` seconds
 *  (eased: it starts and ends that wind-up at rest), then a spring releases it to 0. */
function wound(t: number, a: number, wind: number, wt: number, omega: number, zeta: number): number {
  if (t < wt) return a + wind * smooth(t / wt);
  return spring(t - wt, a + wind, 0, omega, zeta);
}

/** The spring's last tenth of a degree, faded out over the end of the reveal so it lands on
 *  exactly 0 (the residue is far below a pixel by then). */
const land = (x: number, u: number) => x * (1 - smooth((u - 0.9) / 0.1));

const ZETA = 0.58; // one overshoot of ~11 %, the next ~1 %: a single small swing back

/** The pose of a `kind` reveal at progress u (0…1). */
export function revealFrame(kind: RevealKind, u: number): RevealFrame {
  if (u >= 1) return { ...REST };
  const t = clamp01(u) * REVEAL_SECONDS[kind];
  const arrive = smooth(t / 0.16);
  switch (kind) {
    // the hero, on load: it comes up out of the depth below, turned and tipped away, swings to
    // face you and the screen lights; only then does the Molty run in (the hold), so the
    // device arrives first and the creature second
    case "arrive": {
      const yaw = land(wound(t, -38, -4, 0.13, 8.6, ZETA), u);
      return {
        ...REST,
        yaw,
        pitch: land(wound(t, -22, -3, 0.13, 9.2, ZETA), u),
        lift: land(wound(t, 0.12, 0.015, 0.13, 9.5, 0.7), u),
        opacity: arrive,
        cover: 1 - smooth((t - 0.55) / 0.3),
        hold: t < 0.72,
      };
    }
    // turned away, it winds a touch further, swings to face you and overshoots once; the
    // screen lights as the swing ends
    case "turn": {
      const yaw = land(wound(t, 46, 5, 0.13, 8.6, ZETA), u);
      return { ...REST, yaw, opacity: arrive, cover: 1 - smooth((t - 0.5) / 0.32), hold: t < 0.36, moltyYaw: yaw };
    }
    // the reader: it swings in with its glass held open, then the glass falls shut under
    // gravity, bounces once on the bezel and the screen lights under it
    case "lid": {
      const yaw = land(wound(t, 42, 4, 0.11, 10.5, ZETA), u);
      return { ...REST, yaw, opacity: arrive, lid: lidFall(t - 0.36), cover: 1 - smooth((t - 0.74) / 0.22) };
    }
    // it rises from below with its face tipped down (crouching a little first), and tips back
    case "rise": {
      const pitch = land(wound(t, -48, -5, 0.13, 8.6, ZETA), u);
      return {
        ...REST,
        pitch,
        lift: land(wound(t, 0.15, 0.02, 0.13, 9.5, 0.7), u),
        opacity: arrive,
        cover: 1 - smooth((t - 0.5) / 0.32),
        hold: t < 0.36,
      };
    }
    // a coin arriving already spinning on its edge: the spring catches its momentum, it slows,
    // swings past face-on once and settles; it drops the last bit into place
    case "coin": {
      const yaw = land(spring(t, 300, -260, 7.2, 0.72), u);
      return {
        ...REST,
        yaw,
        lift: land(spring(t, -0.08, 0, 9, 0.6), u),
        opacity: arrive,
        cover: 1 - smooth((t - 0.55) / 0.3),
        hold: t < 0.48,
        moltyYaw: Math.abs(yaw) < 80 ? yaw : null,
      };
    }
    // a pick on the showcase: a quick swing, the Molty turning with it, the screen dimmed a beat
    case "quick": {
      const yaw = land(wound(t, 26, 3, 0.08, 12, ZETA), u);
      return { ...REST, yaw, cover: 0.85 * (1 - smooth(t / 0.32)), moltyYaw: yaw };
    }
  }
}

// the lid: open at -100°, it falls under gravity, hits the bezel and bounces once
const LID_OPEN = -100;
const LID_G = 1800; // deg/s²
const LID_E = 0.22; // the bounce keeps this much of the speed
const LID_T = Math.sqrt((2 * -LID_OPEN) / LID_G); // fall time
const LID_V = LID_G * LID_T * LID_E; // speed back up after the hit
function lidFall(t: number): number {
  if (t <= 0) return LID_OPEN;
  if (t < LID_T) return LID_OPEN + 0.5 * LID_G * t * t;
  const b = t - LID_T; // the bounce: up and back under the same gravity
  return b < (2 * LID_V) / LID_G ? -(LID_V * b - 0.5 * LID_G * b * b) : 0;
}

/** Where the glass's reflections are when the board is turned: a window (or lamp) stays put in
 *  the room, so its reflection on the domed glass SLIDES across it, against the turn, and is
 *  squeezed as it nears the rim (the dome curves away). The dome's radius of curvature is
 *  DOME screen radii; at rest the highlight sits up-left, where the light comes from (as the
 *  bezel's lit side). Offsets are fractions of the screen's diameter from the rest place. */
export const DOME = 2.4;
const REST_GLARE: [number, number] = [-0.36, -0.42]; // screen radii, from the centre (x right, y down)
export function glareOf(yaw: number, pitch: number) {
  const rad = Math.PI / 180;
  // the normal that reflects the window into the eye is fixed in the room; in the board's own
  // frame it turns by the full turn, against it (yaw + turns the face left, so the reflecting
  // point moves right; a face tipped down (pitch -) moves it up)
  const ax = Math.asin(REST_GLARE[0] / DOME) + yaw * rad,
    ay = Math.asin(REST_GLARE[1] / DOME) + pitch * rad;
  const x = DOME * Math.sin(ax),
    y = DOME * Math.sin(ay);
  const r = Math.hypot(x, y);
  // squeezed toward the rim (radially), and dimmer as it slides off the glass
  const squeeze = Math.sqrt(Math.max(0.05, 1 - Math.min(1, r / 1.05) ** 2));
  return {
    dx: (x - REST_GLARE[0]) / 2,
    dy: (y - REST_GLARE[1]) / 2,
    squeeze,
    restSqueeze: Math.sqrt(1 - (Math.hypot(...REST_GLARE) / 1.05) ** 2),
    angle: Math.atan2(y, x),
    fade: 1 - smooth((r - 0.85) / 0.35),
  };
}
