// Look options for the creature in the fluid (crisp-options.html): the real cat package
// (packages/cat/cat.json) played by the creature player, drawn into ONE real FluidSim the way
// the device draws a package body (device/firmware/main/creature/creature_body.c: rig, camera,
// flat Lambert raster at 2x2 samples a cell, style('dither'), fill with holes), then the same
// sim drawn six ways by GlyphRenderer. The options are renderer flags through its cellHook.

import * as THREE from 'three';
import { FluidSim } from './fluid/sim.js';
import { GlyphRenderer } from './fluid/render.js';
import { GlyphRamps } from './fluid/synth.js';
import { MoodEngine, MOODS } from './mood.js';
import { actionPose, ACTIONS } from './actions.js';
import { creaturePlayer } from './creature-player.js';

THREE.ColorManagement.enabled = false; // the device shades sRGB bytes directly: no linear round trip

const $ = (id) => document.getElementById(id);
const W = 52, H = 32, PANEL = 412;
const clamp = (v, a, b) => (v < a ? a : v > b ? b : v);
const smooth = (a, b, x) => { const t = clamp((x - a) / (b - a), 0, 1); return t * t * (3 - 2 * t); };
const BAYER4 = [0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5].map((v) => (v + 0.5) / 16);

// ---------------------------------------------------------------- the package body (creature_body.c)
const FIT = 0.8, AMB = 0.4, KEY = 1.15, VIEW = [0.16, 0.2, 1], LIGHT = [-0.75, 1, 0.55];
const S = { FLOOR: 0.32, LOW: 0.1, SPAN: 0.68, SAT: 1.5, SHADOW: 0.55, HOT: 0.55, EDGE: 0.7, GLARE: 0.74 };

function partGeometry(p) {
  switch (p.type) {
    case 'ellipsoid': { const seg = p.seg || [12, 8], g = new THREE.SphereGeometry(1, seg[0], seg[1]); g.scale(...p.size.map((s) => s || 1e-4)); return g; }
    case 'box': return new THREE.BoxGeometry(...p.size);
    case 'cone': return new THREE.CylinderGeometry(0, p.r, p.h, p.seg || 8, 1);
    case 'cylinder': return new THREE.CylinderGeometry(p.r, p.r2 ?? p.r, p.h, p.seg || 10, 1);
    default: return new THREE.TubeGeometry(new THREE.CatmullRomCurve3(p.path.map((v) => new THREE.Vector3(...v)), false, 'centripetal', 0.5), p.seg || 32, p.r, 6, false);
  }
}

class PkgCreature {
  constructor(pkg, sim) {
    const cr = pkg.creature, { N } = sim;
    this.sim = sim; this.cr = cr; this.RW = W * 2; this.RH = H * 2;
    this.player = creaturePlayer(cr, { actionPose, ACTIONS });
    this.engine = new MoodEngine();
    this.canvas = document.createElement('canvas');
    this.renderer = new THREE.WebGLRenderer({ canvas: this.canvas, antialias: false, alpha: true });
    this.rt = new THREE.WebGLRenderTarget(this.RW, this.RH, { samples: 0, type: THREE.FloatType });
    this.bufA = new Float32Array(this.RW * this.RH * 4); this.bufB = new Float32Array(this.RW * this.RH * 4);
    this.scene = new THREE.Scene();
    this.key = new THREE.DirectionalLight(0xffffff, KEY * Math.PI); this.amb = new THREE.AmbientLight(0xffffff, AMB * Math.PI);
    this.scene.add(this.key, this.amb);
    this.white = new THREE.MeshLambertMaterial({ color: 0xffffff, flatShading: true });
    // rig: root, then bones parent first; parts hang off their bone
    this.root = new THREE.Group(); this.scene.add(this.root);
    this.bones = new Map([['root', this.root]]); this.rest = [];
    for (const b of cr.body.bones) {
      const g = new THREE.Group(); (b.parent ? this.bones.get(b.parent) : this.root).add(g);
      this.bones.set(b.id, g); this.rest.push([g, b.pos || [0, 0, 0], b.rot || [0, 0, 0]]);
    }
    // feature ids for the outline: a region is (limb, colour), so the head, each leg, the tail and
    // every differently coloured part (ears, eyes, nose, chest) get their own outline; torso and hips are one body
    const pal = cr.body.palette, byId = Object.fromEntries(cr.body.bones.map((b) => [b.id, b]));
    const limb = (id) => { if (!id || id === 'torso' || id === 'hips' || /^legB/.test(id)) return 'body'; /* a hind thigh is the haunch, part of the body's outline */ let b = byId[id]; while (b.parent && !['torso', 'hips'].includes(b.parent)) b = byId[b.parent]; return b.id; };
    const EYE = new Set(['eye', 'iris', 'shine', 'maw']), keys = ['none'];
    this.meshes = [];
    for (const p of cr.body.parts) {
      const m = new THREE.Mesh(partGeometry(p), new THREE.MeshLambertMaterial({ color: new THREE.Color(pal[p.color] || p.color), flatShading: true }));
      if (p.pos) m.position.set(...p.pos); if (p.rot) m.rotation.set(...p.rot);
      (p.bone ? this.bones.get(p.bone) : this.root).add(m);
      const key = limb(p.bone) + ':' + (EYE.has(p.color) ? 'eye' : p.color);
      if (!keys.includes(key)) keys.push(key);
      m.userData.lit = m.material; m.userData.id = new THREE.MeshBasicMaterial({ color: new THREE.Color(keys.indexOf(key) / 255, 0, 0) });
      this.meshes.push(m);
    }
    this.bufC = new Float32Array(this.RW * this.RH * 4); this.ids = new Uint8Array(this.RW * this.RH); // sample ids, top row first

    this.cov = new Float32Array(N); this.lum = new Float32Array(N); this.cr_ = new Float32Array(N); this.cg = new Float32Array(N); this.cb = new Float32Array(N);
    this.hole = new Uint8Array(N); this.body = new Float32Array(N);
    this._reset(); this._fit();
    this.ms = 0;
  }
  _reset() {
    this.root.position.set(0, 0, 0); this.root.rotation.set(0, 0, 0); this.root.scale.set(1, 1, 1);
    for (const [g, p, r] of this.rest) { g.position.set(...p); g.rotation.set(...r); g.scale.set(1, 1, 1); }
  }
  // renderer.mjs frame(): from VIEW, centred on the rest body's projected bounds, furthest vertex at FIT of the glass
  _fit() {
    const z = new THREE.Vector3(...VIEW).normalize(), x = new THREE.Vector3(0, 1, 0).cross(z).normalize(), y = z.clone().cross(x);
    this.cx = x; this.cy = y; this.cz = z;
    this.scene.updateMatrixWorld(true);
    const pts = [], v = new THREE.Vector3();
    this.scene.traverse((o) => { if (!o.isMesh) return; const a = o.geometry.attributes.position; for (let i = 0; i < a.count; i += 2) { v.fromBufferAttribute(a, i).applyMatrix4(o.matrixWorld); pts.push([v.dot(x), v.dot(y)]); } });
    let x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
    for (const [px, py] of pts) { x0 = Math.min(x0, px); x1 = Math.max(x1, px); y0 = Math.min(y0, py); y1 = Math.max(y1, py); }
    this.c0 = [(x0 + x1) / 2, (y0 + y1) / 2];
    let h = 0; for (const [px, py] of pts) h = Math.max(h, Math.hypot(px - this.c0[0], py - this.c0[1]));
    this.half = Math.max(h, 1e-3) / FIT;
    const centre = x.clone().multiplyScalar(this.c0[0]).addScaledVector(y, this.c0[1]);
    this.cam = new THREE.OrthographicCamera(-this.half, this.half, this.half, -this.half, 0.01, 100);
    this.cam.position.copy(centre).addScaledVector(z, 10); this.cam.up.set(0, 1, 0); this.cam.lookAt(centre); this.cam.updateMatrixWorld();
  }
  /** A world point on the glass, in sim coords (-1..1, y down). */
  glass(p) { return [(p.dot(this.cx) - this.c0[0]) / this.half, -(p.dot(this.cy) - this.c0[1]) / this.half]; }
  act(name) { this.engine.act(name, this.player.gestures[name]); }
  /** One frame. d: the stage's walk (drive below): stood up (up), turned side on (side), going (k), the cycle (q), where (x). */
  update(dt, d) {
    const t0 = performance.now(), eng = this.engine;
    dt = Math.min(dt, 1 / 30);
    const p = eng.update(dt);
    // the stage's part, as the lab's walk engine: it stands up first (prowl), turns side on (a quad turns a quarter), looks where it goes
    p.prowl = d.up; p.rot.y += (Math.PI / 2) * d.side; p.look.x += 0.5 * d.side;
    this.player.move.move(p, { q: d.q, k: d.k, go: d.k, along: Math.abs(d.side), side: d.side, fz: 0, up: d.up });
    this.player.pose(p, dt, { action: eng.action, prevAction: eng.prevAction, work: 0 });
    this._reset(); this.player.apply(this.bones); this.pose = p;
    // the travel across the floor is the stage's (the device never walks it anywhere): it carries the body; the bowl stays put
    if (d.x) { this.root.position.x += d.x; const bowl = this.bones.get('bowl'); if (bowl) bowl.position.sub(new THREE.Vector3(d.x, 0, 0).applyQuaternion(this.root.quaternion.clone().invert())); }
    // the key comes round to the front as the body turns side on
    const side = Math.abs(Math.sin(this.root.rotation.y));
    this.key.position.set(LIGHT[0] * (1 - 0.8 * side), LIGHT[1], LIGHT[2] + 0.5 * side).normalize();
    const r = this.renderer, RW = this.RW, RH = this.RH;
    r.setRenderTarget(this.rt); r.setClearColor(0x000000, 0);
    this.key.visible = false; this.amb.intensity = Math.PI; // pass A: albedo
    r.clear(); r.render(this.scene, this.cam); r.readRenderTargetPixels(this.rt, 0, 0, RW, RH, this.bufA);
    this.key.visible = true; this.amb.intensity = AMB * Math.PI; // pass B: the shade on white
    this.scene.overrideMaterial = this.white; r.clear(); r.render(this.scene, this.cam); this.scene.overrideMaterial = null;
    r.readRenderTargetPixels(this.rt, 0, 0, RW, RH, this.bufB);
    for (const m of this.meshes) m.material = m.userData.id; // pass C: which feature is in front
    this.key.visible = false; r.clear(); r.render(this.scene, this.cam); this.key.visible = true;
    for (const m of this.meshes) m.material = m.userData.lit;
    r.readRenderTargetPixels(this.rt, 0, 0, RW, RH, this.bufC); r.setRenderTarget(null);
    for (let y = 0; y < RH; y++) for (let x = 0; x < RW; x++) { const q = ((RH - 1 - y) * RW + x) * 4; this.ids[y * RW + x] = this.bufC[q + 3] > 0 ? Math.round(this.bufC[q] * 255) : 0; }
    const A = this.bufA, B = this.bufB, { cov, lum, cr_: R, cg: G, cb: Bl } = this;
    let lo = 1, hi = 0;
    for (let j = 0; j < H; j++) for (let i = 0; i < W; i++) {
      let n = 0, rr = 0, gg = 0, bb = 0;
      for (let sy = 0; sy < 2; sy++) for (let sx = 0; sx < 2; sx++) {
        const q = ((RH - 1 - (j * 2 + sy)) * RW + i * 2 + sx) * 4;
        if (A[q + 3] > 0) { const s = Math.min(1, B[q]); n++; rr += A[q] * s; gg += A[q + 1] * s; bb += A[q + 2] * s; }
      }
      const k = j * W + i, inv = n ? 1 / n : 0;
      cov[k] = n / 4; R[k] = rr * inv; G[k] = gg * inv; Bl[k] = bb * inv;
      const l = 0.2126 * R[k] + 0.7152 * G[k] + 0.0722 * Bl[k]; lum[k] = l;
      if (n > 2) { lo = Math.min(lo, l); hi = Math.max(hi, l); }
    }
    this._style(lo, 1 / Math.max(0.15, hi - lo));
    this.ms = performance.now() - t0;
  }
  // renderer.mjs style('dither'), as creature_body.c style()
  _style(lo, span) {
    const N = W * H, { cov: C, lum: L, cr_: R, cg: G, cb: B, hole } = this;
    for (let i = 0; i < N; i++) {
      const mx = Math.max(R[i], G[i], B[i]);
      hole[i] = C[i] > 0.5 && mx < 0.09 ? 1 : 0;
      if (C[i] <= 0) continue;
      const m = Math.max(mx, 1e-4), v = clamp((L[i] - lo) * span, 0, 1), hr = R[i] / m, hg = G[i] / m, hb = B[i] / m;
      const edge = i < W || i >= N - W || !C[i - 1] || !C[i + 1] || !C[i - W] || !C[i + W];
      const z = clamp((v - S.LOW) / S.SPAN, 0, 1);
      let u = z * z * (3 - 2 * z); if (edge) u *= S.EDGE;
      const y = 0.2126 * hr + 0.7152 * hg + 0.0722 * hb, calmer = Math.min(1, S.GLARE / Math.max(y, 0.01));
      L[i] = hole[i] ? 0 : (S.FLOOR + (1 - S.FLOOR) * u) * calmer;
      const g = (hr + hg + hb) / 3, sr = Math.min(1, g + (hr - g) * S.SAT), sg = Math.min(1, g + (hg - g) * S.SAT), sb = Math.min(1, g + (hb - g) * S.SAT);
      const n = Math.max(sr, sg, sb, 1e-4), br = sr / n, bg = sg / n, bb = sb / n;
      if (u < 0.5) { const q = u / 0.5, k = S.SHADOW + (1 - S.SHADOW) * q; R[i] = br * k * (0.92 + 0.08 * q); G[i] = bg * k * (0.62 + 0.38 * q); B[i] = Math.min(1, bb * k * (0.8 + 0.2 * q) + (1 - q) * 0.12); }
      else { const q = Math.max(0, (v - 0.62) / 0.38) * S.HOT; R[i] = br + (1 - br) * q; G[i] = bg + (0.97 - bg) * q; B[i] = bb + (0.88 - bb) * q; }
      R[i] *= calmer; G[i] *= calmer; B[i] *= calmer;
    }
  }
  // CreatureTarget.fill plus holes (cb_fill); also keeps the body's coverage for the look hooks
  fill(sim, swell) {
    const { N, X, Y, mask, T, TR, TG, TB, TA } = sim, { cov, lum, cr_: R, cg: G, cb: B, hole, body } = this;
    for (let k = 0; k < N; k++) {
      sim.TF[k] = sim.TE[k] = 0; body[k] = 0;
      if (!mask[k]) { T[k] = 0; continue; }
      let fx = ((X[k] / swell + 1) / 2) * W - 0.5, fy = ((Y[k] / swell + 1) / 2) * H - 0.5;
      fx = clamp(fx, 0, W - 1.001); fy = clamp(fy, 0, H - 1.001);
      const x0 = fx | 0, y0 = fy | 0, ax = fx - x0, ay = fy - y0, q = y0 * W + x0;
      const bl = (a) => (a[q] * (1 - ax) + a[q + 1] * ax) * (1 - ay) + (a[q + W] * (1 - ax) + a[q + W + 1] * ax) * ay;
      const c = bl(cov), qn = Math.round(fy) * W + Math.round(fx);
      T[k] = c > 0 && !hole[qn] ? c * (0.06 + 0.94 * Math.pow(clamp(bl(lum), 0, 1), 1.3)) : 0;
      body[k] = c > 0 && !hole[qn] ? c : 0;
      if (c > 0.05) { const d = c >= 0.25 ? 1 / c : 4; TR[k] = bl(R) * d; TG[k] = bl(G) * d; TB[k] = bl(B) * d; }
      TA[k] = 0;
    }
  }
}


// ---------------------------------------------------------------- the new look (renderer cellHook)
// Shared per frame: which feature is under every dot (the id raster, through the breathing swell and
// the wave's refraction, so outlines ripple with the ink). The floor is fixed to the frame: its horizon
// and vanishing point come from the cat's rest pose, so the cat walks over it and it never scrolls.
const DW = W * 2, DH = H * 4, DOT_W = 1 / W, DOT_H = 0.5 / H; // dots, and their pitch in sim units
// one per sim (a cat and its fluid); the hooks read the one being drawn
const world = (sim, cat) => ({ sim, cat, idDot: new Uint8Array(DW * DH), horizon: 0.3, vx: 0 });
let F = null;

function mapIds(swell) {
  const { sim, cat } = F, { h, p, A } = sim, ids = cat.ids, RW = cat.RW, RH = cat.RH, out = F.idDot;
  for (let sy = 0; sy < DH; sy++) for (let sx = 0; sx < DW; sx++) {
    const i = sx >> 1, j = sy >> 2, k = j * W + i;
    let x = i + ((sx & 1) ? 0.25 : -0.25), y = j + ((sy & 3) - 1.5) * 0.25;
    if (i > 0 && j > 0 && i < W - 1 && j < H - 1) { x += (h[k + 1] - h[k - 1]) * 0.5 * p.refr * 6; y += ((h[k + W] - h[k - W]) * 0.5 * p.refr * 6) / A; }
    const X = ((x + 0.5) / W) * 2 - 1, Y = ((y + 0.5) / H) * 2 - 1;
    const fx = ((X / swell + 1) / 2) * W - 0.5, fy = ((Y / swell + 1) / 2) * H - 0.5;
    const qx = Math.floor((fx + 0.5) * 2), qy = Math.floor((fy + 0.5) * 2);
    out[sy * DW + sx] = qx >= 0 && qy >= 0 && qx < RW && qy < RH ? ids[qy * RW + qx] : 0;
  }
}
const idAt = (sx, sy) => (sx < 0 || sy < 0 || sx >= DW || sy >= DH ? 0 : F.idDot[sy * DW + sx]);

// the floor at (X, Y) below the horizon: 0 nothing, 1 a stipple dot, 2 a stronger receding line
function floorAt(X, Y, f) {
  const d = Y - F.horizon;
  if (d < 0.04) return 0;
  const u = ((X - F.vx) * f.su) / d; // across, in floor units (1 unit = d / su on screen)
  if (f.lineEvery && d > 0.09) { const L = f.lineEvery; if (Math.abs(u - Math.round(u / L) * L) * (d / f.su) < DOT_W * 0.5) return 2; }
  // stipple: a lattice on the floor plane, so its dots crowd toward the horizon; every other one
  // drops out where they would touch
  let ax = d / (f.su * f.stipple), ux = (X - F.vx) / ax; while (ax < f.gap * DOT_W) { ax *= 2; ux *= 0.5; }
  let ay = (d * d) / (f.sz * f.stipple), z = (f.sz * f.stipple) / d; while (ay < f.gap * DOT_H) { ay *= 2; z *= 0.5; }
  const ox = (Math.floor(z) & 1) * 0.5; // alternate rows offset: a woven stipple, not a square grid
  if (Math.abs(ux + ox - Math.round(ux + ox)) * ax < DOT_W * 0.5 && Math.abs(z - Math.round(z)) * ay < DOT_H * 0.5) return 1;
  return 0;
}

const hashDot = (x, y) => { let h = Math.imul(x, 374761393) + Math.imul(y, 668265263) | 0; h = Math.imul(h ^ (h >>> 13), 1274126177); return ((h ^ (h >>> 16)) >>> 0) / 4294967296; };

function makeHook(o) {
  const hook = { mean: -1, cell(k, i, j, sub, dots) {
    const sim = F.sim, h = sim.h;
    let gx = 0, gy = 0; // the wave's shift, so the floor ripples under a touch but stays put otherwise
    if (i > 0 && j > 0 && i < W - 1 && j < H - 1) { gx = (h[k + 1] - h[k - 1]) * 0.5 * sim.p.refr * 6; gy = ((h[k + W] - h[k - W]) * 0.5 * sim.p.refr * 6) / sim.A; }
    let out = 0, edges = 0, body = 0, lines = 0, floor = 0, vig = 0;
    for (let q = 0; q < 8; q++) {
      const bit = 1 << q, sx = i * 2 + (q >> 2), sy = j * 4 + (q & 3);
      const thr = BAYER4[((sy & 3) << 2) | (sx & 3)], thr2 = hashDot(sx, sy); // the vignette's own (static) noise, so it does not cancel the dither
      const X = ((i + ((q >> 2) ? 0.25 : -0.25) + 0.5) / W) * 2 - 1, Y = ((j + ((q & 3) - 1.5) * 0.25 + 0.5) / H) * 2 - 1;
      // vignette over the whole screen: fades to the rim, a little sooner at the bottom
      const rr = Math.hypot(X, Y > 0 ? Y * 1.06 : Y), v = 1 - smooth(0.74, 1.02, rr); vig += v;
      const a = idAt(sx, sy);
      if (a) {
        body++;
        const l = idAt(sx - 1, sy), r = idAt(sx + 1, sy), u = idAt(sx, sy - 1), dn = idAt(sx, sy + 1);
        // thin: the silhouette from inside, and one side (the higher id) of a boundary between features
        const edge = o.thick ? l !== a || r !== a || u !== a || dn !== a
          : !l || !r || !u || !dn || (l !== a && a > l) || (r !== a && a > r) || (u !== a && a > u) || (dn !== a && a > dn);
        if (edge) { out |= bit; edges++; } else if (o.fill * sub[q] > thr) out |= bit;
        continue;
      }
      if (o.thick && (idAt(sx - 1, sy) || idAt(sx + 1, sy) || idAt(sx, sy - 1) || idAt(sx, sy + 1))) { out |= bit; edges++; continue; } // thick: one more dot outside
      // a clear dot round the body, so the floor and the dim field never run into an outline
      if (idAt(sx - 1, sy) || idAt(sx + 1, sy) || idAt(sx, sy - 1) || idAt(sx, sy + 1) || idAt(sx - 1, sy - 1) || idAt(sx + 1, sy - 1) || idAt(sx - 1, sy + 1) || idAt(sx + 1, sy + 1)) continue;
      let on = (dots & bit) !== 0; // the fluid's own dim field (and a touch's crests)
      const Xr = X + (gx * 2) / W, Yr = Y + (gy * 2) / H;
      if (Yr > F.horizon) {
        // the ground: the background thickens into it, with a stipple and a few receding lines
        const lv = o.floor.tone * smooth(F.horizon, F.horizon + 0.45, Yr);
        const f = floorAt(Xr, Yr, o.floor);
        if (f === 2) { on = true; lines++; } else if (f === 1 || lv > thr) on = true;
        if (on) floor++;
      }
      if (on && thr2 >= v) on = false;
      if (on) out |= bit;
    }
    const vc = vig / 8;
    if (edges) hook.mean = 0.8;
    else if (body) hook.mean = 0.3;
    else if (lines) hook.mean = 0.75 * vc;
    else if (floor) hook.mean = 0.42 * vc;
    return out;
  } };
  return hook;
}

const FLOOR = { su: 5, sz: 1.1, stipple: 1, gap: 3, tone: 0.28, lineEvery: 3 }, NOFILL = { fill: 0, floor: FLOOR };

// ---------------------------------------------------------------- page
const [pkg, old] = await Promise.all(['../../packages/cat/cat.json', 'crisp-cat-old.json'].map(async (u) => (await fetch(u)).json()));
const worlds = [pkg, old].map((pk) => { const sim = new FluidSim(W, H, PANEL / W, PANEL / H), w = world(sim, new PkgCreature(pk, sim));
  const g = w.cat.glass(new THREE.Vector3(0, 0, 0)); w.vx = g[0]; w.horizon = g[1] - 0.3; // fixed: the rest pose's ground
  // the target colour starts as the background's everywhere (it is only written where something is drawn, and the floor is drawn
  // past the background's rim, where it would otherwise stay black); the creature state's settings from js/fluid/main.js
  { const c = sim.p.bgColor; sim.TR.fill(c[0]); sim.TG.fill(c[1]); sim.TB.fill(c[2]); sim.r.set(sim.TR); sim.g.set(sim.TG); sim.b.set(sim.TB); }
  sim.p.relaxUp = 2.6; sim.p.relaxDown = 3.4; sim.edgeW = 0; sim.swirlKick = 3; sim.impulse(0, 0, 0.7, 0.2);
  return w; });
const [NEW, OLD] = worlds, cat = NEW.cat, sim = NEW.sim;
const OPTIONS = [
  { name: 'No fill', note: `The chosen look on the new cat (${pkg.version}): slimmer legs, folding hind legs, a long tail.`, o: NOFILL, w: NEW, main: true },
  { name: 'Old design', note: `The same look on the cat as it was (${old.version}), to compare.`, o: NOFILL, w: OLD },
  { name: 'Today', note: 'The renderer as it ships now, on the new cat, for reference.', o: null, w: NEW },
];

const grid = $('discs'), discs = [];
OPTIONS.forEach((opt, n) => {
  const fig = document.createElement('figure'); if (opt.main) fig.className = 'main';
  fig.innerHTML = `<canvas width="${PANEL}" height="${PANEL}"></canvas><figcaption><b>${n + 1}. ${opt.name}</b><span>${opt.note}</span></figcaption>`;
  grid.append(fig);
  const cv = fig.querySelector('canvas'), r = new GlyphRenderer(cv), s = opt.w.sim; r.resize(1);
  discs.push({ r, opt, cv, hook: opt.o ? makeHook(opt.o) : null });
  // touch: tap = ripple, drag = ripple trail + a push of the flow (as fluid.html)
  let down = null;
  const pos = (e) => { const b = cv.getBoundingClientRect(); return [((e.clientX - b.left) / b.width) * 2 - 1, ((e.clientY - b.top) / b.height) * 2 - 1]; };
  cv.addEventListener('pointerdown', (e) => { cv.setPointerCapture(e.pointerId); down = pos(e); s.impulse(down[0], down[1], 1.6, 0.09); });
  cv.addEventListener('pointermove', (e) => {
    if (!down) return; const p = pos(e), dx = p[0] - down[0], dy = p[1] - down[1];
    if (Math.hypot(dx, dy) < 0.03) return;
    s.push(p[0], p[1], dx * s.W * 6, dy * s.W * 6, 0.16); s.impulse(p[0], p[1], 0.5, 0.06); down = p;
  });
  const up = () => { down = null; }; cv.addEventListener('pointerup', up); cv.addEventListener('pointercancel', up);
});

// the walk (the stage's, as the lab's walk engine): stand up, turn side on, walk to a spot at a steady pace with the cycle advanced by
// the ground covered (stride world units a cycle, so planted paws never slide), slow to a stop, stay up a moment, turn back, sit
const STRIDE = pkg.creature.walk.stride, VMAX = 0.16, SPOT = 0.24;
const drive = { on: false, x: 0, target: 0, dir: 1, pause: 0, k: 0, up: 0, rest: 0, side: 0, q: 0 };
function stepDrive(dt) {
  const D = drive;
  if (D.on && D.pause <= 0 && Math.abs(D.target - D.x) < 0.004 && D.k < 0.05) D.pause = 1.8;
  if (D.pause > 0) { D.pause -= dt; if (D.pause <= 0 && D.on) { D.dir = -D.dir; D.target = SPOT * D.dir; } }
  const dx = D.target - D.x, moving = D.on && D.pause <= 0 && Math.abs(dx) > 0.004, want = moving ? Math.min(1, Math.abs(dx) * 5) : 0;
  D.side += ((moving ? Math.sign(dx) : 0) - D.side) * (1 - Math.exp(-dt * 4));
  D.rest = want > 0.02 || D.k > 0.1 ? 0.7 : D.rest - dt;
  D.up += ((D.rest > 0 ? 1 : 0) - D.up) * (1 - Math.exp(-dt * 5));
  const goal = D.up > 0.85 && Math.abs(D.side) > 0.8 ? want : 0;
  D.k += (goal - D.k) * (1 - Math.exp(-dt * (goal > D.k ? 4 : 7)));
  const v = D.k * VMAX * Math.sign(dx);
  D.x += v * dt; D.q = (D.q + (Math.abs(v) * dt) / STRIDE) % 1;
}

// moods, gestures and walking: buttons, plus an auto cycle so the cat keeps doing things
const cats = worlds.map((w) => w.cat);
const ctl = { auto: true, mood: 'idle' };
const setMood = (m) => { ctl.mood = m; for (const c of cats) c.engine.set(m); for (const b of $('moods').children) b.classList.toggle('on', b.dataset.m === m); };
const act = (g) => { for (const c of cats) if (c.player.gestures[g]) c.act(g); };
const setWalk = (on) => { drive.on = on; if (on) { drive.pause = 0; drive.target = SPOT * drive.dir; } else drive.target = drive.x; $('walk').classList.toggle('on', on); };
for (const m of MOODS) { const b = document.createElement('button'); b.textContent = m; b.dataset.m = m; b.onclick = () => setMood(m); $('moods').append(b); }
const GESTURES = Object.keys(cat.player.gestures);
for (const g of GESTURES) { const b = document.createElement('button'); b.textContent = g.replace('_', ' '); b.onclick = () => act(g); $('gestures').append(b); }
$('walk').onclick = () => setWalk(!drive.on);
$('auto').onchange = (e) => { ctl.auto = e.target.checked; };
setMood('idle');
const CYCLE = ['idle', 'happy', 'listening', 'thinking', 'speaking', 'sleepy', 'surprised'];

const STEP = 1 / 60, ramp = GlyphRamps.Fade;
let fc = 0, last = performance.now(), tNow = 0, acc = 0, breathPh = 0, nextMood = 7, nextGesture = 2.5, nextWalk = 9, gi = 0, fps = 0;

function frame(now) {
  const rdt = Math.min(0.1, (now - last) / 1000); last = now; tNow += rdt;
  fps = fps ? fps * 0.95 + 0.05 / Math.max(rdt, 1e-3) : 60;
  if (ctl.auto) {
    if (tNow > nextMood) { nextMood = tNow + 7; setMood(CYCLE[(CYCLE.indexOf(ctl.mood) + 1) % CYCLE.length]); }
    if (tNow > nextGesture && !drive.on && drive.up < 0.1) { nextGesture = tNow + 3.2; act(GESTURES[gi++ % GESTURES.length]); }
    if (tNow > nextWalk) { setWalk(!drive.on); nextWalk = tNow + (drive.on ? 12 : 10); }
  }
  if (!drive.hold) stepDrive(Math.min(rdt, 1 / 30)); // hold: a test pins the walk to look at one phase
  const sleepy = ctl.mood === 'sleepy', period = sleepy ? 6 : 4;
  breathPh += rdt / period; const breath = 0.5 - 0.5 * Math.cos(breathPh * Math.PI * 2);
  const swell = 1 + (sleepy ? 0.07 : 0.05) * (breath * 2 - 1);
  const thinking = ctl.mood === 'thinking', speaking = ctl.mood === 'speaking';
  acc += rdt; let n = 0; while (acc >= STEP && n < 4) { acc -= STEP; n++; } if (n === 4) acc = 0;
  for (const w of worlds) {
    const { sim, cat } = w;
    { const q = clamp((tNow - 0.7) / 1.0, 0, 1); sim.p.hold = q * q * (3 - 2 * q); sim.p.tintRate = 3 + 9 * sim.p.hold; }
    cat.update(rdt, drive);
    cat.fill(sim, swell);
    sim.applyBackground(tNow, breath);
    const level = speaking ? cat.engine.envValue || 0 : 0;
    if (speaking) { sim.osc.x = 0; sim.osc.y = 0.12; sim.osc.amp = 1.2 * level; } else { sim.osc.x = 0; sim.osc.y = 0; sim.osc.amp = 0; }
    sim.osc.freq = 2.5 + 6 * 0.4;
    sim.vibe.amp = thinking ? 0.9 : 0; sim.vibe.freq = 7 + 9 * 0.3;
    for (let s = 0; s < n; s++) sim.step(STEP);
    sim.computeView(null);
    F = w; mapIds(swell);
  }
  const shimmerToday = 0.12 + (thinking ? 0.35 : 0);
  for (const d of discs) { F = d.opt.w; d.r.draw(F.sim, { ramp, palette: 'pastel', braille: 'all', time: tNow, shimmer: d.hook ? 0 : shimmerToday, edges: 1, letters: [], melt: 0, cellHook: d.hook }); }
  if ((++fc & 15) === 0) $('stat').textContent = `${fps.toFixed(0)} fps · ${ctl.mood}${drive.on ? ' · walking' : ''}${cat.engine.action ? ' · ' + cat.engine.action.name.replace('_', ' ') : ''}`;
  requestAnimationFrame(frame);
}
requestAnimationFrame(frame);
window.__crisp = { sim, cat, worlds, discs, drive, setMood, setWalk, act, ctl };
