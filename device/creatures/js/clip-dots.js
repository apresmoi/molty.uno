// The clip creature's look on the braille lattice (docs/CREATURES.md section 12): the JS reference for
// main/creature/creature_dots.c. Every frame, straight from the posed rig, with no meshes:
//
//   body      each rounded primitive projected orthographically to a 2D ellipse or capsule. The merged body's
//             primitives (kind `blob`) add a metaball field (st (1 - q)^3, the same falloff as the Blender model), so
//             the back, rump and thighs are one smooth shape; the head, front legs, tail and props are solid parts.
//             Each dot keeps the nearest surface (an analytic depth) and the outline group it belongs to.
//   lines     one outline round the whole silhouette, and inner lines where a nearer group crosses another (a near
//             leg over the body, the head over the chest); a hind leg draws only its lower front edge against the body
//             (the haunch), never a line over the rump. Brighter where nearer.
//   features  round eyes (an iris ring, a lighter pupil, a white shine), the nose, the pink inner ears, the white chest
//             patch's edge (a region of the body's surface, not a shape), the mouth and tongue when open, whiskers as
//             fine lines where nothing else is drawn, the bowl and the yarn.
//   world     a static perspective floor (a dotted texture fixed in the world and a few receding lines; the cat walks
//             over it, the camera fixed), faint dust above that thickens toward the horizon and twinkles a few rows a
//             frame, filling the disc to a narrow fade at its rim, and a two-dot halo kept clear round the cat.
//
//   const r = clipDots(rig, W, H);   r.draw(world, cam, frame) -> { on: Uint8Array(2W*4H), rgb: Float32Array(*3), pri: Uint8Array }
//   r.cells() -> { dots: Uint8Array(W*H) braille bytes, rgb: Uint32Array(W*H) 0xRRGGBB }   (one colour a cell, as the panel draws)

export const VIEW_SIZE = 1.75, FLOOR_AT = 0.72, HORIZON_AT = 0.495, THRESH = 0.6;   // the creature on the near floor, larger
const STRUCT = ['torso', 'hindL', 'hindR', 'head', 'legF.L', 'legF.R', 'tail', 'bowl', 'yarn'];
const FEAT = ['eye', 'earIn', 'nose', 'maw', 'tongue', 'whisker', 'chest', 'milk', 'yarnline'];
export const hash3 = (i, j, k) => {
  let h = (Math.imul(i, 374761393) + Math.imul(j, 668265263) + Math.imul(k, 2147483647)) | 0;
  h = Math.imul(h ^ (h >>> 13), 1274126177);
  return ((h ^ (h >>> 16)) >>> 0) / 4294967296;
};
const sstep = (a, b, x) => { const t = Math.min(1, Math.max(0, (x - a) / (b - a))); return t * t * (3 - 2 * t); };
// the dust twinkles a few cell rows a frame (each row every TWINKLE frames, rows in a scattered order): a twinkle all
// over the screen each frame would make every row a window for the panel to resend
export const TWINKLE = 24;
const twinkleOffset = (j, H) => Math.floor(((j * 11) % H) * TWINKLE / H);
const hex = (h) => [parseInt(h.slice(1, 3), 16) / 255, parseInt(h.slice(3, 5), 16) / 255, parseInt(h.slice(5, 7), 16) / 255];

function quatMat(q) {
  const [x, y, z, w] = q;
  return [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
    2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
    2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)];
}

export function clipDots(rig, W, H) {
  const DW = 2 * W, DH = 4 * H, N = DW * DH, dsx = VIEW_SIZE / DW, dsy = VIEW_SIZE / DH, floorRow = FLOOR_AT * DH;
  const pal = Object.fromEntries(Object.entries(rig.palette).map(([k, v]) => [k, hex(v)]));
  const shapes = rig.shapes.map((s) => ({ ...s, rq: s.q ? quatMat(s.q) : null }));
  const thighs = { hindL: rig.nodes.findIndex((n) => n.id === 'thigh.L'), hindR: rig.nodes.findIndex((n) => n.id === 'thigh.R') };
  const field = new Float32Array(N), z = new Float32Array(N), own = new Int8Array(N);
  const feat = new Uint8Array(N), fz = new Float32Array(N), on = new Uint8Array(N), rgb = new Float32Array(N * 3), pri = new Uint8Array(N), region = new Uint8Array(N);
  const gid = Object.fromEntries(STRUCT.map((g, i) => [g, i]));
  let cam = null;

  const xf = (w, n, p) => [w[n * 12] * p[0] + w[n * 12 + 1] * p[1] + w[n * 12 + 2] * p[2] + w[n * 12 + 3],
    w[n * 12 + 4] * p[0] + w[n * 12 + 5] * p[1] + w[n * 12 + 6] * p[2] + w[n * 12 + 7],
    w[n * 12 + 8] * p[0] + w[n * 12 + 9] * p[1] + w[n * 12 + 10] * p[2] + w[n * 12 + 11]];
  const xv = (w, n, v) => [w[n * 12] * v[0] + w[n * 12 + 1] * v[1] + w[n * 12 + 2] * v[2], w[n * 12 + 4] * v[0] + w[n * 12 + 5] * v[1] + w[n * 12 + 6] * v[2], w[n * 12 + 8] * v[0] + w[n * 12 + 9] * v[1] + w[n * 12 + 10] * v[2]];
  const proj = (p) => { const q = [p[0] - cam.c[0], p[1], p[2] - cam.c[2]]; return [q[0] * cam.r[0] + q[1] * cam.r[1] + q[2] * cam.r[2], q[0] * cam.u[0] + q[1] * cam.u[1] + q[2] * cam.u[2], q[0] * cam.d[0] + q[1] * cam.d[1] + q[2] * cam.d[2]]; };
  const toDot = (u, v) => [u / dsx + DW / 2 - 0.5, floorRow - v / dsy - 0.5];
  const dotUV = (X, Y) => [(X + 0.5 - DW / 2) * dsx, (floorRow - (Y + 0.5)) * dsy];

  // an ellipsoid on the screen: centre (u, v, depth), the inverse of its 2D covariance, its half thickness toward the camera
  function ellipse(world, s) {
    const c = proj(xf(world, s.b, s.p)), A = [];
    for (let k = 0; k < 3; k++) { const ax = xv(world, s.b, [s.rq[k] * s.r[k], s.rq[3 + k] * s.r[k], s.rq[6 + k] * s.r[k]]); A.push(ax); }
    let s00 = 0, s01 = 0, s11 = 0, hh = 0;
    for (const a of A) {
      const au = a[0] * cam.r[0] + a[1] * cam.r[1] + a[2] * cam.r[2], av = a[0] * cam.u[0] + a[1] * cam.u[1] + a[2] * cam.u[2], ad = a[0] * cam.d[0] + a[1] * cam.d[1] + a[2] * cam.d[2];
      s00 += au * au; s01 += au * av; s11 += av * av; hh += ad * ad;
    }
    const det = s00 * s11 - s01 * s01;
    if (det < 1e-12) return null;
    return { u: c[0], v: c[1], d: c[2], i00: s11 / det, i01: -s01 / det, i11: s00 / det, eu: Math.sqrt(s00), ev: Math.sqrt(s11), h: Math.sqrt(hh), A };
  }
  function box(cu, cv, eu, ev, f) {        // the dots within (eu, ev) of (cu, cv), each with its (u, v)
    const [x0, y1] = toDot(cu - eu, cv - ev), [x1, y0] = toDot(cu + eu, cv + ev);
    for (let Y = Math.max(0, Math.floor(y0)); Y <= Math.min(DH - 1, Math.ceil(y1)); Y++)
      for (let X = Math.max(0, Math.floor(x0)); X <= Math.min(DW - 1, Math.ceil(x1)); X++) { const [u, v] = dotUV(X, Y); f(Y * DW + X, u, v); }
  }
  // a capsule (a cone or a cylinder too) on the screen
  function capsule(world, s, r0, r1) {
    const a = proj(xf(world, s.b, s.p)), b = proj(xf(world, s.b, s.p1)), sc = Math.hypot(world[s.b * 12], world[s.b * 12 + 4], world[s.b * 12 + 8]);
    return { a, b, r0: r0 * sc, r1: r1 * sc };
  }
  function inCapsule(c, u, v) {
    const ex = c.b[0] - c.a[0], ey = c.b[1] - c.a[1], L = ex * ex + ey * ey;
    let t = L > 1e-12 ? ((u - c.a[0]) * ex + (v - c.a[1]) * ey) / L : 0; t = t < 0 ? 0 : t > 1 ? 1 : t;
    const r = c.r0 + (c.r1 - c.r0) * t, dx = u - c.a[0] - ex * t, dy = v - c.a[1] - ey * t, d2 = dx * dx + dy * dy;
    return d2 < r * r ? c.a[2] + (c.b[2] - c.a[2]) * t + Math.sqrt(r * r - d2) : null;
  }
  function coneCapsule(world, s) {
    const c = capsule(world, s, 0, 0), fa = xv(world, s.b, s.fa), ax3 = [0, 0, 0];
    const pb = xf(world, s.b, s.p), pt = xf(world, s.b, s.p1);
    for (let k = 0; k < 3; k++) ax3[k] = pt[k] - pb[k];
    const e2 = [ax3[1] * fa[2] - ax3[2] * fa[1], ax3[2] * fa[0] - ax3[0] * fa[2], ax3[0] * fa[1] - ax3[1] * fa[0]], l2 = Math.hypot(...e2) || 1;
    const ex = c.b[0] - c.a[0], ey = c.b[1] - c.a[1], l = Math.hypot(ex, ey) || 1, px = -ey / l, py = ex / l;
    const fu = fa[0] * cam.r[0] + fa[1] * cam.r[1] + fa[2] * cam.r[2], fv = fa[0] * cam.u[0] + fa[1] * cam.u[1] + fa[2] * cam.u[2];
    const gu = (e2[0] * cam.r[0] + e2[1] * cam.r[1] + e2[2] * cam.r[2]) / l2, gv = (e2[0] * cam.u[0] + e2[1] * cam.u[1] + e2[2] * cam.u[2]) / l2;
    const sc = Math.hypot(world[s.b * 12], world[s.b * 12 + 4], world[s.b * 12 + 8]);
    c.r0 = Math.hypot(s.r[0] * s.r[1] * (fu * px + fv * py), s.r[0] * (gu * px + gv * py)) * sc; c.r1 = 0.004;
    c.facing = fa[0] * cam.d[0] + fa[1] * cam.d[1] + fa[2] * cam.d[2];
    return c;
  }

  const api = {
    W, H, DW, DH, on, rgb, pri,
    draw(world, camIn, frame) {
      // the camera: orthographic, from its yaw (0: in front, -90: from the cat's right), a little above unless side on
      const yaw = camIn.yaw * Math.PI / 180, el = (6 * Math.cos(yaw) ** 2) * Math.PI / 180;
      const d = [Math.sin(yaw) * Math.cos(el), Math.sin(el), Math.cos(yaw) * Math.cos(el)];
      const r = [d[2], 0, -d[0]], rl = Math.hypot(r[0], r[2]); r[0] /= rl; r[2] /= rl;
      const u = [d[1] * r[2] - d[2] * r[1], d[2] * r[0] - d[0] * r[2], d[0] * r[1] - d[1] * r[0]];
      cam = { d, r, u, c: [camIn.x, 0, camIn.z] };
      {   // the camera in whole dots across the screen: the creature and the floor move together a dot at a time
        const uu = (cam.c[0] * r[0] + cam.c[2] * r[2]) / dsx, fu = uu - Math.floor(uu + 0.5);
        cam.c[0] -= fu * dsx * r[0]; cam.c[2] -= fu * dsx * r[2];
      }
      field.fill(0); z.fill(-1e9); own.fill(-1); feat.fill(0); on.fill(0); pri.fill(0); rgb.fill(0);
      // 1. the merged body: a metaball field, and the nearest element surface per dot
      for (const s of shapes) {
        if (s.k !== 'blob') continue;
        const e = ellipse(world, s); if (!e) continue;
        const sf = Math.sqrt(1 - Math.cbrt(THRESH / s.st)), k = sf * sf, g = gid[s.g];
        box(e.u, e.v, e.eu / sf, e.ev / sf, (i, uu, vv) => {
          const du = uu - e.u, dv = vv - e.v, q = e.i00 * du * du + 2 * e.i01 * du * dv + e.i11 * dv * dv, qf = q * k;
          if (qf >= 1) return;
          field[i] += s.st * (1 - qf) ** 3;
          const zz = e.d + (q < 1 ? e.h * Math.sqrt(1 - q) : 0);   // its front surface (its centre's depth where only the blend covers)
          if (zz > z[i]) { z[i] = zz; own[i] = g; }
        });
      }
      for (let i = 0; i < N; i++) if (field[i] < THRESH) { z[i] = -1e9; own[i] = -1; }
      // 2. the solid parts, nearest wins
      const put = (i, zz, g) => { if (zz > z[i]) { z[i] = zz; own[i] = g; } };
      for (const s of shapes) {
        if (s.k === 'blob' || !(s.g in gid)) continue;
        const g = gid[s.g];
        if (s.k === 'ell') { const e = ellipse(world, s); if (!e) continue;
          box(e.u, e.v, e.eu, e.ev, (i, uu, vv) => { const du = uu - e.u, dv = vv - e.v, q = e.i00 * du * du + 2 * e.i01 * du * dv + e.i11 * dv * dv; if (q < 1) put(i, e.d + e.h * Math.sqrt(1 - q), g); });
        } else {
          const c = s.k === 'cone' ? coneCapsule(world, s) : capsule(world, s, s.r[0], s.r[1]);
          const m = Math.max(c.r0, c.r1);
          box((c.a[0] + c.b[0]) / 2, (c.a[1] + c.b[1]) / 2, Math.abs(c.b[0] - c.a[0]) / 2 + m, Math.abs(c.b[1] - c.a[1]) / 2 + m, (i, uu, vv) => { const zz = inCapsule(c, uu, vv); if (zz != null) put(i, zz, g); });
        }
      }
      // 3. lines: the outline, and where a nearer group crosses another
      let zlo = Infinity, zhi = -Infinity;
      for (let i = 0; i < N; i++) if (own[i] >= 0 && own[i] < gid.bowl) { if (z[i] < zlo) zlo = z[i]; if (z[i] > zhi) zhi = z[i]; }
      const hip = {};
      for (const g of ['hindL', 'hindR']) { const n = thighs[g]; hip[g] = n >= 0 ? proj([world[n * 12 + 3], world[n * 12 + 7], world[n * 12 + 11]])[1] : 0; }
      const body = pal.body, shade = (zz) => { const t = zhi > zlo ? Math.min(1, Math.max(0, (zz - zlo) / (zhi - zlo))) : 1; return [0, 1, 2].map((c) => Math.min(1, body[c] * (0.5 + 0.5 * t) + (1 - body[c]) * 0.45 * t * t)); };
      const set = (i, col, p) => { if (p < pri[i]) return; on[i] = 1; pri[i] = p; rgb[i * 3] = col[0]; rgb[i * 3 + 1] = col[1]; rgb[i * 3 + 2] = col[2]; };
      for (let Y = 0; Y < DH; Y++) for (let X = 0; X < DW; X++) {
        const i = Y * DW + X, g = own[i]; if (g < 0) continue;
        let line = false;
        for (const [dx, dy] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
          const x = X + dx, y = Y + dy;
          if (x < 0 || y < 0 || x >= DW || y >= DH) { line = true; break; }
          const j = y * DW + x, h = own[j];
          if (h < 0) { line = true; break; }
          if (h === g || g === gid.torso) continue;
          if (z[i] <= z[j] + 0.01) continue;
          if ((g === gid.hindL || g === gid.hindR) && h === gid.torso && dotUV(X, Y)[1] > hip[STRUCT[g]]) continue;   // no line over the rump
          line = true; break;
        }
        if (line) set(i, g === gid.bowl || g === gid.yarn ? pal.bowl : shade(z[i]), 2);
      }
      // 4. features, where they are seen
      const visible = (i, zz) => zz >= z[i] - 0.02;
      const regionEdge = (col, p, keepOut) => { for (let Y = 0; Y < DH; Y++) for (let X = 0; X < DW; X++) { const i = Y * DW + X; if (!region[i]) continue;
        let edge = false; for (const [dx, dy] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) { const x = X + dx, y = Y + dy; if (x < 0 || y < 0 || x >= DW || y >= DH || !region[y * DW + x]) { edge = true; break; } }
        if (edge && !(keepOut && keepOut(i))) set(i, col, p); } };
      const fill = (col, p) => { for (let i = 0; i < N; i++) if (region[i]) set(i, col, p); };
      const discRegion = (s, needFacing) => {
        region.fill(0); const e = ellipse(world, s); if (!e) return;
        if (needFacing) { const n = e.A[2], l = Math.hypot(...n) || 1; if ((n[0] * cam.d[0] + n[1] * cam.d[1] + n[2] * cam.d[2]) / l < 0.15) return; }
        box(e.u, e.v, e.eu, e.ev, (i, uu, vv) => { const du = uu - e.u, dv = vv - e.v, q = e.i00 * du * du + 2 * e.i01 * du * dv + e.i11 * dv * dv; if (q < 1 && visible(i, e.d + e.h * Math.sqrt(1 - q))) region[i] = 1; });
      };
      const eyes = shapes.filter((s) => s.g === 'eye');
      for (const s of eyes.filter((s) => s.c === 'iris')) {
        discRegion(s, true); const eye = region.slice();
        regionEdge(pal.iris, 4);
        for (const p of eyes.filter((t) => t.b === s.b && t.c !== 'iris')) {
          discRegion(p, true);
          for (let i = 0; i < N; i++) if (region[i] && eye[i]) set(i, p.c === 'shine' ? pal.shine : pal.iris.map((c) => c * 0.75 + 0.25), p.c === 'shine' ? 6 : 5);
        }
      }
      for (const s of shapes) {
        if (s.g === 'nose' || s.g === 'tongue') { discRegion(s, false); fill(pal.nose, 5); }
        else if (s.g === 'maw') { discRegion(s, false); regionEdge(pal.nose, 4); }
        else if (s.g === 'milk') { discRegion(s, false); regionEdge(pal.belly, 3); }
        else if (s.g === 'earIn') {
          region.fill(0); const c = coneCapsule(world, s); if (c.facing < 0.1) continue; const m = Math.max(c.r0, c.r1);
          box((c.a[0] + c.b[0]) / 2, (c.a[1] + c.b[1]) / 2, Math.abs(c.b[0] - c.a[0]) / 2 + m, Math.abs(c.b[1] - c.a[1]) / 2 + m, (i, uu, vv) => { const zz = inCapsule(c, uu, vv); if (zz != null && visible(i, zz)) region[i] = 1; });
          regionEdge(pal.ear, 4);
        } else if (s.k === 'patch') {           // the white chest: a region of the body's own surface
          region.fill(0);
          const C = xf(world, s.b, s.p), Ax = [0, 1, 2].map((k) => xv(world, s.b, [s.rq[k] * s.r[k], s.rq[3 + k] * s.r[k], s.rq[6 + k] * s.r[k]]));
          const inv = inv3(Ax); if (!inv) continue;
          for (let i = 0; i < N; i++) {
            if (own[i] !== gid.torso) continue;
            const [uu, vv] = dotUV(i % DW, (i / DW) | 0), P = [cam.c[0] + uu * cam.r[0] + vv * cam.u[0] + z[i] * cam.d[0], uu * cam.r[1] + vv * cam.u[1] + z[i] * cam.d[1], cam.c[2] + uu * cam.r[2] + vv * cam.u[2] + z[i] * cam.d[2]];
            const q = [P[0] - C[0], P[1] - C[1], P[2] - C[2]], l = [0, 1, 2].map((k) => inv[k * 3] * q[0] + inv[k * 3 + 1] * q[1] + inv[k * 3 + 2] * q[2]);
            if (l[0] * l[0] + l[1] * l[1] + l[2] * l[2] < 1) region[i] = 1;
          }
          regionEdge(pal.belly, 3, (i) => pri[i] >= 2);
        }
      }
      for (const s of shapes) {                  // whiskers last: fine lines where nothing else is drawn
        if (s.g !== 'whisker') continue;
        const c = capsule(world, s, 0, 0); c.r0 = c.r1 = 0.55 * dsx;
        box((c.a[0] + c.b[0]) / 2, (c.a[1] + c.b[1]) / 2, Math.abs(c.b[0] - c.a[0]) / 2 + c.r0, Math.abs(c.b[1] - c.a[1]) / 2 + c.r0, (i, uu, vv) => {
          if (on[i]) return; const zz = inCapsule(c, uu, vv); if (zz != null && (own[i] < 0 || zz >= z[i] - 0.05)) set(i, pal.whisker.map((x) => Math.min(1, x * 1.3)), 1);
        });
      }
      // 5. the world: floor, dust, vignette, kept off the cat by a halo
      const halo = new Uint8Array(N);
      for (let i = 0; i < N; i++) if (own[i] >= 0 || on[i]) { const X = i % DW, Y = (i / DW) | 0;
        for (let dy = -2; dy <= 2; dy++) for (let dx = -2; dx <= 2; dx++) { const x = X + dx, y = Y + dy; if (x >= 0 && y >= 0 && x < DW && y < DH && dx * dx + dy * dy <= 5) halo[y * DW + x] = 1; } }
      const scroll = (cam.c[0] * cam.r[0] + cam.c[2] * cam.r[2]) / dsx, horizon = HORIZON_AT * DH, R = DW / 2, flr = hex('#8f87c4'), tex = hex('#6c6598'), dim = hex('#5c5688'), mote = hex('#b4acec');
      for (let Y = 0; Y < DH; Y++) for (let X = 0; X < DW; X++) {
        const i = Y * DW + X; if (halo[i]) continue;
        const ex = (X + 0.5 - DW / 2) / R, ey = (Y + 0.5 - DH / 2) / (DH / 2), rr = Math.sqrt(ex * ex + ey * ey);
        const vig = 1 - sstep(0.9, 0.99, rr);   // the world fills the disc, fading only at its rim if (vig <= 0.01) continue;
        let a = 0, col = null;
        if (Y > horizon) {
          const zz = (floorRow - horizon) / (Y + 0.5 - horizon), uw = (X + 0.5 - DW / 2) * zz + scroll;
          const lane = Math.abs((((uw + 13) % 26) + 26) % 26 - 13),   // the lines either side of the middle, none under the cat
            near = sstep(horizon, horizon + 14 * DH / 128, Y);
          if (lane > 13 - 0.55 * zz) { a = 0.85 * near; col = flr; }
          else if (hash3(Math.floor(uw / 1.6), Math.round(14 / zz), 7) < 0.16 + 0.1 * (1 - near)) { a = (0.5 + 0.25 * near) * (0.6 + 0.4 * hash3(Math.floor(uw), Y, 3)); col = tex; }
        }
        if (!a) {
          const band = Math.exp(-Math.abs(Y - horizon) / 14), seed = hash3(X, Y, 11);
          if (seed < 0.02 + 0.09 * band) { const tw = hash3(X, Y, Math.floor((frame + twinkleOffset(Y >> 2, DH / 4)) / TWINKLE)); a = (0.3 + 0.55 * tw) * (0.55 + 0.45 * band); col = tw > 0.8 ? mote : dim; }
        }
        if (a) { const k = Math.min(1, a * vig); set(i, col.map((c) => c * k), 0); on[i] = k > 0.12 ? 1 : 0; }
      }
      return { on, rgb, pri };
    },
    /** One braille byte and one colour per cell: the cell's highest-priority dots set its colour (the brightest of them). */
    cells() {
      const dots = new Uint8Array(W * H), col = new Uint32Array(W * H);
      for (let j = 0; j < H; j++) for (let i = 0; i < W; i++) {
        let bits = 0, best = -1, bl = -1, c = 0;
        for (let q = 0; q < 8; q++) {
          const x = 2 * i + (q >> 2), y = 4 * j + (q & 3), k = y * DW + x;
          if (!on[k]) continue;
          bits |= 1 << (q < 4 ? (q & 3) < 3 ? (q & 3) : 6 : (q & 3) < 3 ? 3 + (q & 3) : 7);
          const l = rgb[k * 3] + rgb[k * 3 + 1] + rgb[k * 3 + 2];
          if (pri[k] > best || (pri[k] === best && l > bl)) { best = pri[k]; bl = l; c = k; }
        }
        if (best >= 1) {   // a cell with the cat in it shows only the cat's dots
          bits = 0; for (let q = 0; q < 8; q++) { const x = 2 * i + (q >> 2), y = 4 * j + (q & 3), k = y * DW + x; if (on[k] && pri[k] >= 1) bits |= 1 << (q < 4 ? (q & 3) < 3 ? (q & 3) : 6 : (q & 3) < 3 ? 3 + (q & 3) : 7); } }
        dots[j * W + i] = bits;
        col[j * W + i] = bits ? (Math.round(rgb[c * 3] * 255) << 16) | (Math.round(rgb[c * 3 + 1] * 255) << 8) | Math.round(rgb[c * 3 + 2] * 255) : 0;
      }
      return { dots, col };
    },
  };
  return api;
}

function inv3(A) {       // A: three column vectors; the inverse of the matrix with those columns, row-major
  const [a, b, c] = A, m = [a[0], b[0], c[0], a[1], b[1], c[1], a[2], b[2], c[2]];
  const d = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
  if (Math.abs(d) < 1e-12) return null;
  return [(m[4] * m[8] - m[5] * m[7]) / d, (m[2] * m[7] - m[1] * m[8]) / d, (m[1] * m[5] - m[2] * m[4]) / d,
    (m[5] * m[6] - m[3] * m[8]) / d, (m[0] * m[8] - m[2] * m[6]) / d, (m[2] * m[3] - m[0] * m[5]) / d,
    (m[3] * m[7] - m[4] * m[6]) / d, (m[1] * m[6] - m[0] * m[7]) / d, (m[0] * m[4] - m[1] * m[3]) / d];
}
