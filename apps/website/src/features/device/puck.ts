// The device's body as polycss polygons: the part of the puck you only see while it turns. The
// front (the bezel, its rounded lip, the gasket, the glass) is CSS on the face layer, which
// turns with it, so at rest (face on) this body is entirely hidden behind the bezel and is not
// mounted at all. World space is polycss's (+X, +Y, +Z up); the puck's axis is +X (its face
// looks down +X), so a camera at rotX 90 / rotY 0 sees it face on and rotY / rotX turn it about
// the screen's vertical / horizontal. Units: the screen's radius is 1 (the slot's half-width).
//
//   wall   the board's thickness: a rectangle (polycss's cheapest exact leaf) per side of its
//          outline (`segments` round it, or the ink's rounded square), a little inside the
//          bezel's (or the ink's frame), so face on it never peeks out from behind it
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

/** The ink screen's board (a concept): a rounded square round a square screen (half-side 1),
 *  its frame (CSS, on the face) reaching `frame` from the centre, its corners rounded by
 *  `corner`; the wall a little inside that, like the round one's. */
export const INK_BOARD = {
  frame: 1.44,
  wall: 1.42,
  corner: 0.46,
  /** Points a corner. */
  per: 8,
} as const;

export type Shape = "round" | "ink";

/** The wall's outline, counter-clockwise (y, z): the round one's polygon, or the ink's rounded
 *  square. */
export function outline(shape: Shape): Array<[number, number]> {
  if (shape === "round") {
    const { segments: n, wall: r } = PUCK;
    return Array.from({ length: n }, (_, i) => [
      r * Math.cos((i / n) * Math.PI * 2),
      r * Math.sin((i / n) * Math.PI * 2),
    ]);
  }
  const { wall: h, corner: c, per } = INK_BOARD;
  const out: Array<[number, number]> = [];
  for (let k = 0; k < 4; k++) {
    const cy = (k === 0 || k === 3 ? 1 : -1) * (h - c);
    const cz = (k < 2 ? 1 : -1) * (h - c);
    for (let j = 0; j <= per; j++) {
      const a = ((k + j / per) * Math.PI) / 2;
      out.push([cy + c * Math.cos(a), cz + c * Math.sin(a)]);
    }
  }
  return out;
}

/** `back`: add the back plate (only a board that spins past edge-on ever shows it). */
export function puckPolygons(c: PuckColors, { back: withBack = true, shape = "round" as Shape } = {}): Polygon[] {
  const { wallFront: f, back } = PUCK;
  const ring = outline(shape);
  const at = (x: number, i: number): Vec3 => [x, ...ring[i % ring.length]!];
  const out: Polygon[] = [];
  // the wall's rectangles meet edge to edge on the outline
  for (let i = 0; i < ring.length; i++)
    out.push({ vertices: [at(f, i), at(back, i), at(back, i + 1), at(f, i + 1)], color: c.wall });
  const centre: Vec3 = [back, 0, 0];
  for (let i = 0; withBack && i < ring.length; i++)
    out.push({ vertices: [centre, at(back, i + 1), at(back, i)], color: c.back });
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
