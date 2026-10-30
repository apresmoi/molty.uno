// A Molty as a @glyphcss/fluid mesh target. The cute kit (device/creatures/js/cute/kit.js) and its
// MoodEngine stay the creature's rig: they pose the three.js parts exactly as on the device
// and in the console. This module only reads the posed triangles out of that scene graph and
// hands them to glyphcss, which rasterises them (no WebGL, no offscreen renderer, no GPU
// readback). The camera is CreatureTarget's (device/creatures/js/fluid/targets.js): view direction
// (0.38, 0.28, 1), an orthographic envelope fit made round for the disc, key light (0.5, 1, 1).
// Polygons are emitted normalised to that fit (±1 across), so the target's `extent` is 2.
import { createGlyphOrthographicCamera, type GlyphCamera, type Polygon } from "glyphcss";
import type { CuteCreature } from "@creatures/cute/kit.js";
import type { MoodEngine } from "@creatures/mood.js";

type Vec3 = [number, number, number];
type Rig = {
  CuteCreature: typeof import("@creatures/cute/kit.js").CuteCreature;
  MoodEngine: typeof import("@creatures/mood.js").MoodEngine;
};

const norm = (v: Vec3): Vec3 => {
  const l = Math.hypot(v[0], v[1], v[2]);
  return [v[0] / l, v[1] / l, v[2] / l];
};
const cross = (a: Vec3, b: Vec3): Vec3 => [
  a[1] * b[2] - a[2] * b[1],
  a[2] * b[0] - a[0] * b[2],
  a[0] * b[1] - a[1] * b[0],
];
const dot = (a: Vec3, b: Vec3) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];

// three's lookAt from (0.38, 0.28, 1) with +Y up: the rows of the view rotation
const VIEW_Z = norm([0.38, 0.28, 1]);
const VIEW_X = norm(cross([0, 1, 0], VIEW_Z));
const VIEW_Y = cross(VIEW_Z, VIEW_X);
/** CreatureTarget's key light position (0.5, 1, 1), as a direction. */
export const MOLTY_KEY_LIGHT = norm([0.5, 1, 1]);
/** World units across the target: polygons are normalised to ±1. */
export const MOLTY_EXTENT = 2;

/** The rig's camera as a glyphcss camera. glyphcss's `useMat` path applies `mat` to
 *  (v.y, v.x, v.z) with screen y down, so the x/y columns swap and the y row flips. */
export function moltyCamera(): GlyphCamera {
  const [x, y, z] = [VIEW_X, VIEW_Y, VIEW_Z];
  return createGlyphOrthographicCamera({
    useMat: true,
    mat: [x[1], x[0], x[2], -y[1], -y[0], -y[2], z[1], z[0], z[2]],
  });
}

export async function loadRig(): Promise<Rig> {
  const [kit, mood] = await Promise.all([import("@creatures/cute/kit.js"), import("@creatures/mood.js")]);
  return { CuteCreature: kit.CuteCreature, MoodEngine: mood.MoodEngine };
}

// the turn that points a Molty's front (+z) at the camera: pitch up to its elevation, then yaw
const FACE_YAW = Math.atan2(0.38, 1);
const FACE_PITCH = -Math.atan2(0.28, Math.hypot(0.38, 1));
// what is left of the depth that goes past the glass: a monotonic squeeze along the view axis
// keeps both the depth order and every triangle's winding (a per-vertex sideways spread
// folded steep parts like a duck's bill over themselves: inverted, unlit, a black hole)
const PRESS_KEEP = 0.12;

/** A Molty at the glass of its round window (all neutral as `neutralGlass()` returns). */
export interface MoltyGlass {
  /** 0…1: turned to look straight out of the screen. */
  face: number;
  /** Scale about the anchor (the Molty walking up to the glass). */
  zoom: number;
  /** 0…1: how far the anchor is pulled to the slot's centre. */
  pin: number;
  /** World units (±1 fit) the front is pressed flat past the glass. */
  press: number;
  /** Squash on screen about the anchor (a face on glass widens). */
  sx: number;
  sy: number;
  /** 0…1: the anchor moves from the face's centre to `focusAt`. */
  focus: number;
  /** What the anchor moves to: "eye" (the screen-right eye), "eyes" (between them), "mouth",
   *  "face", or a part name (the centre of its first mesh). */
  focusAt: string;
  /** Screen offset of the anchor after the pin (±1 fit units), and a turn about it in the
   *  screen plane (radians, + counter-clockwise). */
  ox: number;
  oy: number;
  roll: number;
  /** Extra turn of the body about the vertical (radians, + toward screen-right). */
  yaw: number;
  /** Every colour but the dark features pulled toward `tint` by `tintK` (0…1): a cloud gone grey. */
  tint: string;
  tintK: number;
  /** Cheeks (blush) drawn while at the glass. */
  blush: boolean;
  /** Flat marks drawn on the glass in front of the Molty (±1 fit units). */
  marks: MoltyMark[];
}

export const neutralGlass = (): MoltyGlass => ({
  face: 0,
  zoom: 1,
  pin: 0,
  press: 0,
  sx: 1,
  sy: 1,
  focus: 0,
  focusAt: "face",
  ox: 0,
  oy: 0,
  roll: 0,
  yaw: 0,
  tint: "#000000",
  tintK: 0,
  blush: true,
  marks: [],
});

/** A flat shape on the glass: a glyph, an emote, a splat, or a small prop outline. */
export interface MoltyMark {
  /** "?", "!", "star", "z", "drop", "blot" (an irregular splat), "poly" (`pts`, a fan). */
  kind: "?" | "!" | "star" | "z" | "drop" | "blot" | "poly";
  x: number;
  y: number;
  /** Height in ±1 fit units; 0 hides it. */
  size: number;
  /** Turn in the screen plane (radians). */
  rot: number;
  color: string;
  /** "poly": the outline in units of `size`, fanned from its first point (keep it convex). */
  pts?: ReadonlyArray<readonly [number, number]>;
  /** Depth toward the viewer (fit units; default 4, in front of everything). A negative depth
   *  puts it behind the Molty. */
  d?: number;
}

// a "?" as a thick polyline (hook + stem) and a dot, in a unit box centred on 0
const QMARK: Array<[number, number]> = [];
for (let i = 0; i <= 8; i++) {
  const a = Math.PI * (0.95 - (1.45 * i) / 8); // from upper-left round the top to below the right
  QMARK.push([0.24 * Math.cos(a), 0.2 + 0.24 * Math.sin(a)]);
}
QMARK.push([0, -0.06], [0, -0.2]);
const ZMARK: Array<[number, number]> = [
  [-0.24, 0.3],
  [0.24, 0.3],
  [-0.24, -0.3],
  [0.24, -0.3],
];

/** The slice of the MoodEngine's pose the glass beat overrides. */
export interface MoltyPose {
  pos: { x: number; y: number; z: number };
  rot: { x: number; y: number; z: number };
  head: { x: number; y: number; z: number };
  look: { x: number; y: number };
  squash: number;
  lid: number;
  mouth: number;
}

const hexRGB = (h: string) => {
  const n = parseInt(h.slice(1), 16);
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255] as const;
};

/** One posed Molty: set a creature and a mood, advance it, read its polygons. */
export class MoltyRig {
  private creature: CuteCreature | null = null;
  private engine: MoodEngine;
  private center: Vec3 = [0, 0, 0];
  private half = 1;
  private polys: Polygon[] = [];
  /** Screen-x offset (along the camera's right, in the ±1 normalised units) and a turn about
   *  the vertical axis (radians, + turns the face toward screen-right): a Molty running in. */
  shift = 0;
  yaw = 0;
  /** The glass beat (beats/): applied to the posed polygons, null when not at the glass. */
  glass: MoltyGlass | null = null;
  /** Edits the mood pose before the kit applies it (the glass beat's gaze and mouth). */
  patch: ((pose: MoltyPose) => void) | null = null;
  /** Runs after the kit posed the creature, before its triangles are read (a beat moving parts). */
  post: (() => void) | null = null;
  /** The eye's radius in the ±1 fit (measured every pose; the glass beat's eye close-up). */
  eyeR = 0.08;
  private cheeks = new Set<unknown>();
  /** The last frame's world → final (fit) transform, for `where`. */
  private xf: ((x: number, y: number, z: number) => Vec3) | null = null;

  constructor(private rig: Rig) {
    this.engine = new rig.MoodEngine();
  }

  get ready() {
    return !!this.creature;
  }

  /** The posed creature (its parts, sockets, head pivot), for a beat's `post`. */
  get kit(): CuteCreature | null {
    return this.creature;
  }

  setCreature(entry: unknown) {
    this.creature?.dispose();
    this.creature = new this.rig.CuteCreature(entry);
    this.frame(this.creature.envelope);
    this.cheeks = new Set(this.creature.cheeks);
    this.polys = [];
  }

  setMood(mood: string) {
    this.engine.set(mood);
  }

  /** Pose the creature `dt` seconds on and collect its triangles. */
  pose(dt: number): readonly Polygon[] {
    const c = this.creature;
    if (!c) return this.polys;
    const step = Math.min(dt, 1 / 30);
    const p = this.engine.update(step);
    this.patch?.(p as MoltyPose);
    c.update(step, p);
    this.post?.();
    this.polys = this.collect(c);
    return this.polys;
  }

  /** The last posed polygons (reduced motion: one still pose). */
  get polygons(): readonly Polygon[] {
    return this.polys;
  }

  /** Where a point of the Molty was drawn last frame, in ±1 fit units on screen (x right,
   *  y up): "face", "eye", "eyes", "mouth" or a part name; null if there is no such point. */
  where(at: string): [number, number] | null {
    const c = this.creature;
    if (!c || !this.xf) return null;
    const p = this.worldPoint(c, at);
    if (!p) return null;
    const v = this.xf(p[0], p[1], p[2]);
    return [dot(v, VIEW_X), dot(v, VIEW_Y)];
  }

  dispose() {
    this.creature?.dispose();
    this.creature = null;
  }

  /** CreatureTarget._frame without three: the envelope's round orthographic fit. */
  private frame(env: CuteCreature["envelope"]) {
    const STD = { min: [-0.36, 0, -0.36], max: [0.36, 1.26, 0.36] };
    const lo = [Math.min(env.min.x, STD.min[0]!), 0, Math.min(env.min.z, STD.min[2]!)];
    const hi = [Math.max(env.max.x, STD.max[0]!), Math.max(env.max.y, STD.max[1]!), Math.max(env.max.z, STD.max[2]!)];
    const rx = Math.max(-lo[0]!, hi[0]!),
      rz = Math.max(-lo[2]!, hi[2]!),
      restTop = hi[1]! - (env.rise || 0);
    const center: Vec3 = [0, (lo[1]! + hi[1]!) / 2 - 0.03, 0];
    let half = 0;
    for (let i = 0; i < 16; i++) {
      const a = (i / 16) * Math.PI * 2,
        cx = Math.cos(a) * rx,
        cz = Math.sin(a) * rz;
      for (const [y, r] of [
        [lo[1]!, 0.75],
        [restTop * 0.55, 1],
        [hi[1]!, 0.55],
      ] as const) {
        const d: Vec3 = [cx * r - center[0], y - center[1], cz * r - center[2]];
        half = Math.max(half, Math.hypot(dot(d, VIEW_X), dot(d, VIEW_Y)));
      }
    }
    this.center = center;
    this.half = half * 0.98;
  }

  /** A named point of the posed creature, in world space. */
  private worldPoint(c: CuteCreature, at: string): Vec3 | null {
    const h = c.headG.matrixWorld.elements;
    const onHead = (u: number, v: number): Vec3 => {
      const q = c._surfPoint(u, v, 0);
      return [
        h[0]! * q.x + h[4]! * q.y + h[8]! * q.z + h[12]!,
        h[1]! * q.x + h[5]! * q.y + h[9]! * q.z + h[13]!,
        h[2]! * q.x + h[6]! * q.y + h[10]! * q.z + h[14]!,
      ];
    };
    const origin = (m: ArrayLike<number> | undefined): Vec3 | null => (m ? [m[12]!, m[13]!, m[14]!] : null);
    if (at === "face") return onHead(0, (c.eyeSpec.y + c.mouthSpec.y) / 2);
    if (at === "mouth") return onHead(0, c.mouthSpec.y);
    if (at === "eye") return origin(c.eyes[0]?.g.matrixWorld.elements);
    if (at === "eyes") {
      const a = origin(c.eyes[0]?.g.matrixWorld.elements),
        b = origin(c.eyes[1]?.g.matrixWorld.elements);
      return a && b ? [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2, (a[2] + b[2]) / 2] : a;
    }
    const part = c.parts[at]?.[0];
    if (!part) return null;
    // a part's mesh origin can sit at its hinge: use its bounding box's centre when it has one
    const g = part.geometry as { boundingBox?: { min: Vec3Like; max: Vec3Like } | null } | undefined;
    const bb = g?.boundingBox,
      e = part.matrixWorld.elements;
    if (!bb) return origin(e);
    const x = (bb.min.x + bb.max.x) / 2,
      y = (bb.min.y + bb.max.y) / 2,
      z = (bb.min.z + bb.max.z) / 2;
    return [
      e[0]! * x + e[4]! * y + e[8]! * z + e[12]!,
      e[1]! * x + e[5]! * y + e[9]! * z + e[13]!,
      e[2]! * x + e[6]! * y + e[10]! * z + e[14]!,
    ];
  }

  private collect(c: CuteCreature): Polygon[] {
    const out: Polygon[] = [];
    const [ox, oy, oz] = this.center,
      s = 1 / this.half;
    const g = this.glass;
    // the body's turn: the run-in's yaw, plus (at the glass) the turn to face the camera
    const yaw = this.yaw + (g ? FACE_YAW * g.face + g.yaw : 0),
      pitch = g ? FACE_PITCH * g.face : 0;
    const cy = Math.cos(yaw),
      sy = Math.sin(yaw),
      cp = Math.cos(pitch),
      sp = Math.sin(pitch),
      [tx, ty, tz] = [VIEW_X[0] * this.shift, VIEW_X[1] * this.shift, VIEW_X[2] * this.shift];
    const turn = (wx: number, wy: number, wz: number): Vec3 => {
      const x = (wx - ox) * s,
        y = (wy - oy) * s,
        z = (wz - oz) * s;
      const py = cp * y - sp * z,
        pz = sp * y + cp * z;
      return [cy * x + sy * pz, py, -sy * x + cy * pz];
    };
    c.root.updateMatrixWorld(true);
    // at the glass: the anchor (the face point between the eyes and the mouth, on the head's
    // surface, or moving to `focusAt`) in view coordinates. The Molty scales about it, it is
    // pulled to the slot's centre, and the glass stands just in front of it; everything past
    // the glass is pressed flat.
    let fa = 0,
      fb = 0,
      fd = 0,
      ca = 0,
      cb = 0;
    if (g) {
      const f0 = turn(...this.worldPoint(c, "face")!);
      const at = g.focus > 0 && g.focusAt !== "face" ? this.worldPoint(c, g.focusAt) : null;
      const f1 = at ? turn(...at) : f0,
        k = at ? g.focus : 0;
      const f: Vec3 = [f0[0] + (f1[0] - f0[0]) * k, f0[1] + (f1[1] - f0[1]) * k, f0[2] + (f1[2] - f0[2]) * k];
      fa = dot(f, VIEW_X);
      fb = dot(f, VIEW_Y);
      fd = dot(f, VIEW_Z);
      ca = fa * (1 - g.pin) + g.ox; // where the anchor lands on screen
      cb = fb * (1 - g.pin) + g.oy;
    }
    const cr = g ? Math.cos(g.roll) : 1,
      sr = g ? Math.sin(g.roll) : 0;
    {
      // the eye's radius: its scaled unit disc's x axis, in the fit
      const m = c.eyes[0]?.inner.matrixWorld.elements;
      if (m) this.eyeR = Math.hypot(m[0]!, m[1]!, m[2]!) * s;
    }
    const xf = (x: number, y: number, z: number): Vec3 => {
      const w = turn(x, y, z);
      if (!g) return [w[0] + tx, w[1] + ty, w[2] + tz];
      // view coordinates about the anchor, scaled; the glass is at depth -press
      let a = (dot(w, VIEW_X) - fa) * g.zoom,
        b = (dot(w, VIEW_Y) - fb) * g.zoom,
        d = (dot(w, VIEW_Z) - fd) * g.zoom;
      const over = d + g.press;
      if (over > 0) d = -g.press + over * PRESS_KEEP;
      a *= g.sx;
      b *= g.sy;
      const ra = a * cr - b * sr;
      b = a * sr + b * cr + cb;
      a = ra + ca;
      return [
        a * VIEW_X[0] + b * VIEW_Y[0] + d * VIEW_Z[0] + tx,
        a * VIEW_X[1] + b * VIEW_Y[1] + d * VIEW_Z[1] + ty,
        a * VIEW_X[2] + b * VIEW_Y[2] + d * VIEW_Z[2] + tz,
      ];
    };
    this.xf = xf;
    const tint = g && g.tintK > 0 ? hexRGB(g.tint) : null,
      tk = g?.tintK ?? 0;
    c.root.traverseVisible((o) => {
      if (!o.isMesh || !o.geometry || !o.material) return;
      if (g && !g.blush && this.cheeks.has(o)) return;
      let color = "#" + o.material.color.getHexString();
      const rgb = tint ? hexRGB(color) : null;
      // dark features (eyes, mouth) keep their colour: tinted, they would stop reading as features
      if (tint && rgb && 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2] > 64) {
        const [r, gg, b] = rgb;
        const mix = (v: number, w: number) => Math.round(v + (w - v) * tk);
        color =
          "#" + ((1 << 24) | (mix(r, tint[0]) << 16) | (mix(gg, tint[1]) << 8) | mix(b, tint[2])).toString(16).slice(1);
      }
      const pos = o.geometry.getAttribute("position");
      const idx = o.geometry.getIndex();
      const e = o.matrixWorld.elements;
      const n = idx ? idx.count : pos.count;
      const at = (i: number): Vec3 => {
        const q = idx ? idx.getX(i) : i;
        const x = pos.getX(q),
          y = pos.getY(q),
          z = pos.getZ(q);
        return xf(
          e[0]! * x + e[4]! * y + e[8]! * z + e[12]!,
          e[1]! * x + e[5]! * y + e[9]! * z + e[13]!,
          e[2]! * x + e[6]! * y + e[10]! * z + e[14]!,
        );
      };
      for (let i = 0; i + 2 < n; i += 3) out.push({ vertices: [at(i), at(i + 1), at(i + 2)], color });
    });
    if (g) for (const m of g.marks) if (m.size > 0) this.mark(out, m, tx, ty, tz);
    return out;
  }

  /** A mark's triangles, flat on the glass. */
  private mark(out: Polygon[], m: MoltyMark, tx: number, ty: number, tz: number) {
    const c = Math.cos(m.rot),
      sn = Math.sin(m.rot),
      k = m.size,
      d = m.d ?? 4;
    const P = (u: number, v: number): Vec3 => {
      const a = m.x + (u * c - v * sn) * k,
        b = m.y + (u * sn + v * c) * k;
      return [
        a * VIEW_X[0] + b * VIEW_Y[0] + d * VIEW_Z[0] + tx,
        a * VIEW_X[1] + b * VIEW_Y[1] + d * VIEW_Z[1] + ty,
        a * VIEW_X[2] + b * VIEW_Y[2] + d * VIEW_Z[2] + tz,
      ];
    };
    const tri = (a: Vec3, b: Vec3, e: Vec3) => out.push({ vertices: [a, b, e], color: m.color });
    const disc = (u: number, v: number, r: number, n: number, wob = 0) => {
      const R = (a: number) => r * (1 + wob * (0.6 * Math.sin(3 * a + 0.4) + 0.4 * Math.sin(5 * a + 1.3)));
      for (let i = 0; i < n; i++) {
        const a0 = (i / n) * Math.PI * 2,
          a1 = ((i + 1) / n) * Math.PI * 2;
        tri(
          P(u, v),
          P(u + R(a0) * Math.cos(a0), v + R(a0) * Math.sin(a0)),
          P(u + R(a1) * Math.cos(a1), v + R(a1) * Math.sin(a1)),
        );
      }
    };
    const stroke = (pts: ReadonlyArray<readonly [number, number]>, w: number) => {
      for (let i = 0; i + 1 < pts.length; i++) {
        const [u0, v0] = pts[i]!,
          [u1, v1] = pts[i + 1]!;
        const dl = Math.hypot(u1 - u0, v1 - v0) || 1,
          nu = (-(v1 - v0) / dl) * w,
          nv = ((u1 - u0) / dl) * w;
        const a = P(u0 + nu, v0 + nv),
          b = P(u0 - nu, v0 - nv),
          e = P(u1 + nu, v1 + nv),
          f = P(u1 - nu, v1 - nv);
        tri(a, b, e);
        tri(b, f, e);
        disc(u1, v1, w, 6); // round joints
      }
    };
    switch (m.kind) {
      case "star":
        // five chunky points (thin spikes fall below a cell's coverage and vanish)
        for (let i = 0; i < 5; i++) {
          const a = Math.PI / 2 + (i * Math.PI * 2) / 5;
          const tip = P(0.5 * Math.cos(a), 0.5 * Math.sin(a));
          const l = P(0.26 * Math.cos(a + Math.PI / 5), 0.26 * Math.sin(a + Math.PI / 5)),
            r = P(0.26 * Math.cos(a - Math.PI / 5), 0.26 * Math.sin(a - Math.PI / 5));
          tri(P(0, 0), r, tip);
          tri(P(0, 0), tip, l);
        }
        return;
      case "?":
        stroke(QMARK, 0.085);
        disc(0, -0.42, 0.1, 8);
        return;
      case "!":
        stroke(
          [
            [0, 0.45],
            [0, -0.12],
          ],
          0.1,
        );
        disc(0, -0.4, 0.1, 8);
        return;
      case "z":
        stroke(ZMARK, 0.12);
        return;
      case "drop":
        disc(0, -0.12, 0.28, 10);
        tri(P(-0.26, -0.03), P(0.26, -0.03), P(0, 0.45));
        return;
      case "blot":
        disc(0, 0, 0.5, 14, 0.22);
        return;
      case "poly": {
        const p = m.pts;
        if (!p || p.length < 3) return;
        for (let i = 1; i + 1 < p.length; i++)
          tri(P(p[0]![0], p[0]![1]), P(p[i]![0], p[i]![1]), P(p[i + 1]![0], p[i + 1]![1]));
        return;
      }
    }
  }
}

type Vec3Like = { x: number; y: number; z: number };
