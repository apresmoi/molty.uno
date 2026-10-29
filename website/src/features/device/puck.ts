// The device's body as polycss polygons: the part of the puck you only see while it turns. The
// front (the bezel, its rounded lip, the gasket, the glass) is CSS on the face layer, which
// turns with it, so at rest (face on) this body is entirely hidden behind the bezel and is not
// mounted at all. World space is polycss's (+X, +Y, +Z up); the puck's axis is +X (its face
// looks down +X), so a camera at rotX 90 / rotY 0 sees it face on and rotY / rotX turn it about
// the screen's vertical / horizontal. Units: the screen's radius is 1 (the slot's half-width).
//
//   wall   the board's thickness: `segments` rectangles (polycss's cheapest exact leaf), with a
//          radius a little inside the bezel's, so face on it never peeks out from behind it
//   back   a flat disc (only for a board that spins past edge-on), a fan of triangles (one
//          48-gon is a clipped leaf that renders unreliably edge-on)
//
// Every face winds counter-clockwise seen from outside (polycss culls back faces).
import type { Polygon, Vec3 } from "@layoutit/polycss-core";

export const PUCK = {
  segments: 48,
  /** The screen's radius, the bezel's outer one (CSS, on the face) and the wall's. */
  screen: 1,
  bezel: 1.1,
  wall: 1.08,
  /** Planes (x): the bezel's face (the face layer stands here), the wall's front and back. */
  front: 0.05,
  wallFront: 0.045,
  back: -0.16,
} as const;

export interface PuckColors {
  wall: string;
  back: string;
}

/** A point at plane x, distance d out along the segment's centre direction a, s across it. */
const at = (x: number, d: number, s: number, a: number): Vec3 => [
  x,
  d * Math.cos(a) - s * Math.sin(a),
  d * Math.sin(a) + s * Math.cos(a),
];

/** `back`: add the back plate (only a board that spins past edge-on ever shows it). */
export function puckPolygons(c: PuckColors, { back: withBack = true } = {}): Polygon[] {
  const { segments: n, wall: r, wallFront: f, back } = PUCK;
  // half a segment's chord: the wall's rectangles meet edge to edge on the polygon
  const w = r * Math.sin(Math.PI / n);
  const d = r * Math.cos(Math.PI / n);
  const out: Polygon[] = [];
  for (let i = 0; i < n; i++) {
    const a = ((i + 0.5) / n) * Math.PI * 2;
    out.push({ vertices: [at(f, d, -w, a), at(back, d, -w, a), at(back, d, w, a), at(f, d, w, a)], color: c.wall });
  }
  const centre: Vec3 = [back, 0, 0];
  const corner = (k: number) => at(back, d, -w, ((k + 0.5) / n) * Math.PI * 2);
  for (let i = 0; withBack && i < n; i++) out.push({ vertices: [centre, corner(i + 1), corner(i)], color: c.back });
  return out;
}

/** A polygon's normal by the right-hand rule ((v1 - v0) × (v2 - v0)), unit length. */
export function normalOf(p: Polygon): Vec3 {
  const [a, b, d] = p.vertices as [Vec3, Vec3, Vec3];
  const u = [b[0] - a[0], b[1] - a[1], b[2] - a[2]],
    v = [d[0] - a[0], d[1] - a[1], d[2] - a[2]];
  const n: Vec3 = [u[1]! * v[2]! - u[2]! * v[1]!, u[2]! * v[0]! - u[0]! * v[2]!, u[0]! * v[1]! - u[1]! * v[0]!];
  const l = Math.hypot(...n);
  return [n[0] / l, n[1] / l, n[2] / l];
}
