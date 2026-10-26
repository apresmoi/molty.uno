// A Molty as a @glyphcss/fluid mesh target. The cute kit (creatures/js/cute/kit.js) and its
// MoodEngine stay the creature's rig: they pose the three.js parts exactly as on the device
// and in the console. This module only reads the posed triangles out of that scene graph and
// hands them to glyphcss, which rasterises them (no WebGL, no offscreen renderer, no GPU
// readback). The camera is CreatureTarget's (creatures/js/fluid/targets.js): view direction
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

/** One posed Molty: set a creature and a mood, advance it, read its polygons. */
export class MoltyRig {
  private creature: CuteCreature | null = null;
  private engine: MoodEngine;
  private center: Vec3 = [0, 0, 0];
  private half = 1;
  private polys: Polygon[] = [];

  constructor(private rig: Rig) {
    this.engine = new rig.MoodEngine();
  }

  get ready() {
    return !!this.creature;
  }

  setCreature(entry: unknown) {
    this.creature?.dispose();
    this.creature = new this.rig.CuteCreature(entry);
    this.frame(this.creature.envelope);
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
    c.update(step, this.engine.update(step));
    this.polys = this.collect(c);
    return this.polys;
  }

  /** The last posed polygons (reduced motion: one still pose). */
  get polygons(): readonly Polygon[] {
    return this.polys;
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

  private collect(c: CuteCreature): Polygon[] {
    const out: Polygon[] = [];
    const [ox, oy, oz] = this.center,
      s = 1 / this.half;
    c.root.updateMatrixWorld(true);
    c.root.traverseVisible((o) => {
      if (!o.isMesh || !o.geometry || !o.material) return;
      const color = "#" + o.material.color.getHexString();
      const pos = o.geometry.getAttribute("position");
      const idx = o.geometry.getIndex();
      const e = o.matrixWorld.elements;
      const n = idx ? idx.count : pos.count;
      const at = (i: number): Vec3 => {
        const q = idx ? idx.getX(i) : i;
        const x = pos.getX(q),
          y = pos.getY(q),
          z = pos.getZ(q);
        return [
          (e[0]! * x + e[4]! * y + e[8]! * z + e[12]! - ox) * s,
          (e[1]! * x + e[5]! * y + e[9]! * z + e[13]! - oy) * s,
          (e[2]! * x + e[6]! * y + e[10]! * z + e[14]! - oz) * s,
        ];
      };
      for (let i = 0; i + 2 < n; i += 3) out.push({ vertices: [at(i), at(i + 1), at(i + 2)], color });
    });
    return out;
  }
}
