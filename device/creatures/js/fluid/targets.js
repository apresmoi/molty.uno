// Targets: each fills the sim's T (ink), TR/TG/TB (colour) and TA (accent) arrays.
// The fluid never shows a target directly — it relaxes and flows toward it.

import * as THREE from 'three';
import { MoodEngine } from '../mood.js';
import { CuteCreature } from '../cute/kit.js';
import { patchValue, clamp01, noise3 } from './synth.js';

// separable box blur (radius rx × ry), plain loops
function blur(src, dst, tmp, W, H, rx, ry) {
  for (let j = 0; j < H; j++) for (let i = 0; i < W; i++) { let s = 0, n = 0; for (let d = -rx; d <= rx; d++) { const x = i + d; if (x >= 0 && x < W) { s += src[j * W + x]; n++; } } tmp[j * W + i] = s / n; }
  for (let j = 0; j < H; j++) for (let i = 0; i < W; i++) { let s = 0, n = 0; for (let d = -ry; d <= ry; d++) { const y = j + d; if (y >= 0 && y < H) { s += tmp[y * W + i]; n++; } } dst[j * W + i] = s / n; }
}

const hex = (c) => [parseInt(c.slice(1, 3), 16) / 255, parseInt(c.slice(3, 5), 16) / 255, parseInt(c.slice(5, 7), 16) / 255];
const smooth = (a, b, x) => { const t = clamp01((x - a) / (b - a)); return t * t * (3 - 2 * t); };

// ---------------------------------------------------------------- blob
// A breathing metaball: 4 orbiting radial sources summed, iso-surfaced with a soft edge,
// textured inside by a field-synth patch (radial sin × noise, "Lava"-like).
export class BlobTarget {
  constructor() {
    this.patch = { combine: 'add', gain: 1.2, bias: 0.55, scale: 1, voices: [
      { field: 'noise', freq: 2.6, speed: 0.45, amp: 1 },
      { field: 'radial', wave: 'sin', freq: 1.6, speed: 0.35, amp: 0.6 },
    ] };
    this.cA = hex('#ffb3c7'); this.cB = hex('#b9a4ff');
  }
  fill(sim, t, swell) {
    const { N, X, Y, mask, T, TR, TG, TB, TA } = sim;
    const C = [];
    for (let q = 0; q < 4; q++) {
      const w = 0.35 + q * 0.17, ph = q * 1.7;
      C.push([0.2 * Math.cos(t * w + ph), 0.17 * Math.sin(t * w * 1.3 + ph), (0.19 + 0.025 * q) * swell]);
    }
    for (let k = 0; k < N; k++) {
      if (!mask[k]) { T[k] = 0; continue; }
      const x = X[k], y = Y[k];
      let F = 0;
      for (const [cx, cy, r] of C) { const dx = x - cx, dy = y - cy; F += (r * r) / (dx * dx + dy * dy + 1e-4); }
      const edge = smooth(0.85, 1.6, F);
      const tex = patchValue(this.patch, x, y, t);
      T[k] = edge * (0.5 + 0.5 * tex);
      const m = clamp01(tex);
      TR[k] = this.cA[0] + (this.cB[0] - this.cA[0]) * m; TG[k] = this.cA[1] + (this.cB[1] - this.cA[1]) * m; TB[k] = this.cA[2] + (this.cB[2] - this.cA[2]) * m;
      TA[k] = 0;
    }
  }
}

// ---------------------------------------------------------------- effect interludes
// Each is a field-synth patch (the preset families from stock.ts) plus a PHYSICAL action
// on the fluid, so the interlude is felt in the medium, not painted over it.
export const EFFECTS = {
  ripple: { label: 'ripple bloom', colors: ['#2effb0', '#7df9ff'],
    patch: { combine: 'add', gain: 1.5, bias: 0.15, voices: [{ field: 'radial', wave: 'sin', freq: 3.2, speed: 0.9, amp: 1 }, { field: 'noise', freq: 2, speed: 0.3, amp: 0.3 }] },
    enter(sim) { sim.impulse(0, 0, 2.2, 0.16); sim.swirlKick = 2; },
    tick(sim, t, dt, s) { s.acc = (s.acc || 0) + dt; if (s.acc > 0.55) { s.acc = 0; sim.impulse(0, 0, 1.4, 0.12); } } },
  vortex: { label: 'vortex swirl', colors: ['#ff7a45', '#ffd24a'],
    patch: { combine: 'add', gain: 1.3, bias: 0.25, voices: [{ field: 'spiral', wave: 'saw', freq: 2.5, speed: 0.8, amp: 1 }, { field: 'angular', wave: 'sin', freq: 3, speed: -0.5, amp: 0.7 }] },
    enter(sim) { sim.swirlKick = 4; },
    tick(sim, t, dt, s) { s.e = Math.min(1, (s.e || 0) + dt * 0.9); sim.rotate = 1.6 * s.e; },
    exit(sim) { sim.rotate = 0; } },
  wave: { label: 'wave sheet', colors: ['#7b5cff', '#2effb0'],
    patch: { combine: 'add', gain: 1.3, bias: 0.3, voices: [{ field: 'linearY', wave: 'sin', freq: 1.6, speed: 0.6, amp: 1 }, { field: 'noise', freq: 2.4, speed: 0.5, amp: 0.8 }, { field: 'linearX', wave: 'sin', freq: 1.1, speed: 0.2, amp: 0.5 }] },
    enter(sim) { sim.sheet.amp = 1.2; sim.sheet.freq = 1.3; },
    tick() {},
    exit(sim) { sim.sheet.amp = 0; } },
};

export class EffectTarget {
  constructor(name) { this.name = name; this.fx = EFFECTS[name]; this.cA = hex(this.fx.colors[0]); this.cB = hex(this.fx.colors[1]); this.state = {}; }
  fill(sim, t, swell) {
    const { N, X, Y, mask, T, TR, TG, TB, TA } = sim, p = this.fx.patch;
    for (let k = 0; k < N; k++) {
      if (!mask[k]) { T[k] = 0; continue; }
      const x = X[k] / swell, y = Y[k] / swell, rr = x * x + y * y;
      const v = patchValue(p, x, y, t);
      T[k] = v * (1 - smooth(0.55, 1.0, rr)) * 0.9;
      TR[k] = this.cA[0] + (this.cB[0] - this.cA[0]) * v; TG[k] = this.cA[1] + (this.cB[1] - this.cA[1]) * v; TB[k] = this.cA[2] + (this.cB[2] - this.cA[2]) * v;
      TA[k] = 0;
    }
  }
}

// ---------------------------------------------------------------- speed text
// Two looks:
//   'glyph' — the word is rasterised big and the FLUID takes the letter shapes
//             (glyph-of-glyphs: every letter is made of ramp characters)
//   'plain' — the fluid pools into a band; real letters (large glyphs) condense out of
//             it once enough ink has arrived under each letter (scramble → letter)
// Both pivot-align the ORP letter on the centre column like mole_reader.c.
const PANEL = 412;
export class TextTarget {
  constructor(sim) {
    this.sim = sim;
    this.cv = document.createElement('canvas'); this.cv.width = PANEL; this.cv.height = PANEL;
    this.g = this.cv.getContext('2d', { willReadFrequently: true });
    this.cov = new Float32Array(sim.N); this.orp = new Float32Array(sim.N);
    this.near = new Float32Array(sim.N); this.nearS = new Float32Array(sim.N); this.dark = new Float32Array(sim.N); this.tmp = new Float32Array(sim.N);
    this.mode = 'plain'; this.letters = []; this.font = 30; this.white = [1, 1, 1];
  }
  /** Fix the type size for a whole sentence so it doesn't jump between words. */
  setSentence(tokens) {
    let ext = 1;
    for (const t of tokens) ext = Math.max(ext, t.orp + 0.5, t.word.length - t.orp - 0.5);
    // letter advance ≈ 0.6 em (mono) / 0.62 em (bold sans); budget is the disc's middle chord
    this.plainFont = Math.max(18, Math.min(40, 330 / (2 * ext) / 0.6));
    this.glyphFont = Math.max(40, Math.min(150, 372 / (2 * ext) / 0.64));
  }
  setWord(tok) {
    const { sim, g } = this, { W, H } = sim;
    this.tok = tok;
    g.clearRect(0, 0, PANEL, PANEL);
    const w = tok.word, o = tok.orp;
    this.letters = [];
    if (this.mode === 'glyph') {
      // cells are ~1.6× taller than wide, so letters are drawn condensed (stretched ×1.9 in y)
      // and sized per word — otherwise a 7-letter word is only ~4 rows tall and unreadable
      const ext = Math.max(1, o + 0.5, w.length - o - 0.5);
      const fs = Math.max(40, Math.min(150, 380 / (2 * ext) / 0.62)), SY = 1.9;
      g.font = `900 ${fs}px "Arial Black", "Helvetica Neue", Arial, sans-serif`; g.textBaseline = 'middle';
      const pre = g.measureText(w.slice(0, o)).width, ow = g.measureText(w[o]).width;
      const x0 = PANEL / 2 - pre - ow / 2;
      g.save(); g.translate(0, PANEL / 2); g.scale(1, SY);
      g.fillStyle = '#f00'; g.fillText(w.slice(0, o), x0, fs * 0.04); g.fillText(w.slice(o + 1), x0 + pre + ow, fs * 0.04);
      g.fillStyle = '#0f0'; g.fillText(w[o], x0 + pre, fs * 0.04);
      g.restore();
      const img = g.getImageData(0, 0, PANEL, PANEL).data;
      const cw = PANEL / W, ch = PANEL / H;
      for (let j = 0; j < H; j++) for (let i = 0; i < W; i++) {
        let s = 0, a = 0, n = 0;
        const xa = Math.floor(i * cw), xb = Math.floor((i + 1) * cw), ya = Math.floor(j * ch), yb = Math.floor((j + 1) * ch);
        for (let y = ya; y < yb; y += 2) for (let x = xa; x < xb; x += 2) { const q = (y * PANEL + x) * 4; s += img[q] + img[q + 1]; a += img[q + 1]; n++; }
        const k = j * W + i; this.cov[k] = Math.min(1, s / (n * 255)); this.orp[k] = s > 0 ? a / s : 0;
      }
    } else {
      const fs = this.plainFont || 30, adv = fs * 0.6, lh = fs * 1.05;
      const x0 = PANEL / 2 - (o + 0.5) * adv;
      for (let i = 0; i < w.length; i++) this.letters.push({ ch: w[i], x: x0 + i * adv, y: PANEL / 2 - lh / 2, w: adv, h: lh, acc: i === o, fs });
      // ink target: dense under each letter (the letters condense out of it, and their
      // imprint melts back into the field when the next word replaces them)
      const cw = PANEL / W, ch = PANEL / H;
      for (let j = 0; j < H; j++) for (let i = 0; i < W; i++) {
        const k = j * W + i, px = (i + 0.5) * cw, py = (j + 0.5) * ch;
        let inL = 0, isO = 0;
        if (py > PANEL / 2 - lh / 2 && py < PANEL / 2 + lh / 2) { const li = Math.floor((px - x0) / adv); if (li >= 0 && li < w.length && w[li] !== ' ') { inL = 1; isO = li === o ? 1 : 0; } }
        this.cov[k] = inL * 0.62; this.orp[k] = isO;
      }
    }
    // where the word is: 'near' (wide, normalised) shapes the meniscus field that wraps the
    // word; 'dark' (tight) is the soft darker falloff right around the letters for legibility
    const { near, dark, tmp, cov } = this;
    blur(cov, near, tmp, W, H, 9, 4); let mx = 1e-6; for (let k = 0; k < sim.N; k++) mx = Math.max(mx, near[k]);
    for (let k = 0; k < sim.N; k++) near[k] = Math.min(1, near[k] / mx * 1.15);
    blur(cov, dark, tmp, W, H, 2, 1); for (let k = 0; k < sim.N; k++) dark[k] = Math.min(1, dark[k] * 2.2);
  }
  fill(sim, t) {
    // the word sits IN the fluid: streak lines stream past left→right and bow around the
    // word (y is pushed away from the word centre in proportion to 'near'), denser and
    // brighter close to it (the meniscus), softly darker right at the letters
    const { N, mask, X, Y, T, TR, TG, TB, TA, BGM } = sim, { near, nearS, dark, cov } = this, plain = this.mode === 'plain';
    for (let k = 0; k < N; k++) {
      if (!mask[k]) { T[k] = 0; continue; }
      nearS[k] += (near[k] - nearS[k]) * 0.2; // the meniscus re-shapes smoothly between words
      const nr = nearS[k], dk = Math.max(0, dark[k] - cov[k]);
      const ys = Y[k] - 0.2 * nr * Math.sign(Y[k] + 1e-3);
      const n = noise3((X[k] - t * 0.32) * 2.2, ys * 3.2, t * 0.35);
      const streak = 0.5 + 0.5 * Math.sin(ys * 30 + n * 5);
      const field = Math.pow(nr, 1.3) * (0.14 + 0.4 * streak * (0.6 + 0.4 * n)) * (1 - 0.5 * dk);
      T[k] = plain ? (cov[k] > 0 ? cov[k] : field) : Math.max(cov[k], field);
      BGM[k] = 1 - 0.5 * dk;
      TA[k] = this.orp[k];
      const wv = cov[k] > 0 ? 1 : 0.35 + 0.5 * nr;
      TR[k] = 0.6 + 0.35 * wv; TG[k] = 0.68 + 0.28 * wv; TB[k] = 0.92 + 0.08 * wv;
    }
  }
}

// ---------------------------------------------------------------- creature
// The cute-kit creature (js/cute registry) driven by the viewer's MoodEngine, rendered
// offscreen with three.js the way stage.js renders the device preview (orthographic,
// flat Lambert, 1 key + 0.4 ambient, one colour per face): pass A = unlit albedo,
// pass B = light intensity on a white override. Read back at 2×2 samples per cell →
// coverage, shaded colour, luminance → ink = coverage · (0.18 + 0.82·lum).
const DEV = { key: 1.0, amb: 0.4 };
const LUT = new Float32Array(4096);
for (let i = 0; i < 4096; i++) { const c = i / 4095; LUT[i] = c <= 0.0031308 ? 12.92 * c : 1.055 * Math.pow(c, 1 / 2.4) - 0.055; }
const enc = (v) => LUT[v <= 0 ? 0 : v >= 1 ? 4095 : (v * 4095) | 0];

export class CreatureTarget {
  constructor(sim) {
    this.sim = sim; const { W, H, N } = sim;
    this.RW = W * 2; this.RH = H * 2;
    this.canvas = document.createElement('canvas');
    this.renderer = new THREE.WebGLRenderer({ canvas: this.canvas, antialias: false, alpha: true });
    this.renderer.outputColorSpace = THREE.SRGBColorSpace;
    this.rt = new THREE.WebGLRenderTarget(this.RW, this.RH, { samples: 0, type: THREE.FloatType });
    this.bufA = new Float32Array(this.RW * this.RH * 4); this.bufB = new Float32Array(this.RW * this.RH * 4);
    this.scene = new THREE.Scene();
    this.key = new THREE.DirectionalLight(0xffffff, DEV.key * Math.PI); this.key.position.set(0.5, 1, 1);
    this.amb = new THREE.AmbientLight(0xffffff, DEV.amb * Math.PI);
    this.scene.add(this.key, this.amb);
    this.white = new THREE.MeshLambertMaterial({ color: 0xffffff, flatShading: true });
    this.cam = new THREE.OrthographicCamera(-1, 1, 1, -1, 0.01, 100);
    this.engine = new MoodEngine();
    this.cov = new Float32Array(N); this.lum = new Float32Array(N); this.cr = new Float32Array(N); this.cg = new Float32Array(N); this.cb = new Float32Array(N); this.sh = new Float32Array(N); this.e = new Float32Array(N); this.f = new Float32Array(N); this.ex = new Float32Array(N); this.ey = new Float32Array(N);
    this.creature = null; this.ms = 0;
  }
  setCreature(entry) {
    if (this.creature) this.creature.dispose();
    this.creature = new CuteCreature(entry); this.entry = entry;
    this.scene.add(this.creature.root);
    this._frame(this.creature.envelope);
  }
  setMood(m) { this.engine.set(m); }
  _frame(env) {
    // same envelope fit as stage.js _frameEnvelope (ortho part), view dir (0.38, 0.28, 1)
    const STD = { min: [-0.36, 0, -0.36], max: [0.36, 1.26, 0.36] };
    const lo = [Math.min(env.min.x, STD.min[0]), 0, Math.min(env.min.z, STD.min[2])];
    const hi = [Math.max(env.max.x, STD.max[0]), Math.max(env.max.y, STD.max[1]), Math.max(env.max.z, STD.max[2])];
    const rx = Math.max(-lo[0], hi[0]), rz = Math.max(-lo[2], hi[2]), restTop = hi[1] - (env.rise || 0), pts = [];
    for (let i = 0; i < 16; i++) { const a = (i / 16) * Math.PI * 2, cx = Math.cos(a) * rx, cz = Math.sin(a) * rz; for (const [y, r] of [[lo[1], 0.75], [restTop * 0.55, 1], [hi[1], 0.55]]) pts.push(new THREE.Vector3(cx * r, y, cz * r)); }
    const dir = new THREE.Vector3(0.38, 0.28, 1).normalize();
    const target = new THREE.Vector3(0, (lo[1] + hi[1]) / 2 - 0.03, 0);
    this.cam.position.copy(target).addScaledVector(dir, 10); this.cam.lookAt(target); this.cam.updateMatrixWorld();
    const inv = this.cam.matrixWorldInverse, vt = target.clone().applyMatrix4(inv), v = new THREE.Vector3();
    let half = 0; for (const c of pts) { v.copy(c).applyMatrix4(inv).sub(vt); half = Math.max(half, Math.hypot(v.x, v.y)); } // round panel: fit a circle, not a box
    // the disc is round: fit the hull inside ~80 % of the radius so the body isn't clipped by the rim
    this.half = half * 0.98;
    const h = this.half; Object.assign(this.cam, { left: -h, right: h, top: h, bottom: -h }); this.cam.updateProjectionMatrix();
  }
  update(dt) {
    if (!this.creature) return;
    const t0 = performance.now();
    this.pose = this.engine.update(Math.min(dt, 1 / 30));
    this.creature.update(Math.min(dt, 1 / 30), this.pose);
    const r = this.renderer, RW = this.RW, RH = this.RH;
    r.setRenderTarget(this.rt); r.setClearColor(0x000000, 0);
    this.key.visible = false; this.amb.intensity = Math.PI;
    r.clear(); r.render(this.scene, this.cam); r.readRenderTargetPixels(this.rt, 0, 0, RW, RH, this.bufA);
    this.key.visible = true; this.amb.intensity = DEV.amb * Math.PI;
    this.scene.overrideMaterial = this.white; r.clear(); r.render(this.scene, this.cam); this.scene.overrideMaterial = null;
    r.readRenderTargetPixels(this.rt, 0, 0, RW, RH, this.bufB); r.setRenderTarget(null);
    const { W, H } = this.sim, A = this.bufA, B = this.bufB;
    for (let j = 0; j < H; j++) for (let i = 0; i < W; i++) {
      let cov = 0, cr = 0, cg = 0, cb = 0;
      for (let sy = 0; sy < 2; sy++) for (let sx = 0; sx < 2; sx++) {
        const q = ((RH - 1 - (j * 2 + sy)) * RW + i * 2 + sx) * 4;
        if (A[q + 3] > 0) { const kk = B[q + 3] > 0 ? Math.min(1, B[q]) : 1; cov++; cr += enc(A[q]) * kk; cg += enc(A[q + 1]) * kk; cb += enc(A[q + 2]) * kk; }
      }
      const k = j * W + i;
      this.cov[k] = cov / 4;
      if (cov) { cr /= cov; cg /= cov; cb /= cov; }
      this.cr[k] = cr; this.cg[k] = cg; this.cb[k] = cb;
      this.lum[k] = 0.2126 * cr + 0.7152 * cg + 0.0722 * cb;
    }
    // features: covered but very dark cells (eyes, mouth, nostrils) — in ASCII dark = empty,
    // which would erase the face, so they are marked and drawn as a solid 'o'/'@'
    for (let k = 0; k < this.sim.N; k++) this.f[k] = this.cov[k] > 0.5 && this.lum[k] < 0.22 ? 1 : 0;
    // auto-exposure: stretch the covered luminance range over the whole ramp
    let lo = 1, hi = 0;
    for (let k = 0; k < this.sim.N; k++) if (this.cov[k] > 0.5) { const l = this.lum[k]; if (l < lo) lo = l; if (l > hi) hi = l; }
    const span = Math.max(0.15, hi - lo);
    for (let k = 0; k < this.sim.N; k++) this.lum[k] = Math.max(0, Math.min(1, (this.lum[k] - lo) / span));
    // ink contours (glyphcss 'ink' mode): where shade or coverage jumps between neighbouring
    // cells — part boundaries, eyes, bill, silhouette — keep the gradient for a stroke glyph
    for (let j = 1; j < H - 1; j++) for (let i = 1; i < W - 1; i++) {
      const k = j * W + i, L = this.lum, C = this.cov;
      const fx = (L[k + 1] * C[k + 1] - L[k - 1] * C[k - 1]) * 0.5 + (C[k + 1] - C[k - 1]) * 0.35;
      const fy = (L[k + W] * C[k + W] - L[k - W] * C[k - W]) * 0.5 + (C[k + W] - C[k - W]) * 0.35;
      this.ex[k] = fx; this.ey[k] = fy; this.e[k] = Math.hypot(fx, fy);
    }
    this.ms = performance.now() - t0;
  }
  fill(sim, t, swell) {
    const { N, X, Y, mask, T, TR, TG, TB, TA } = sim, { W, H } = sim;
    for (let k = 0; k < N; k++) {
      if (!mask[k]) { T[k] = 0; continue; }
      // breathing swell = sample the creature buffer slightly scaled about the centre
      let fx = ((X[k] / swell + 1) / 2) * W - 0.5, fy = ((Y[k] / swell + 1) / 2) * H - 0.5;
      fx = Math.max(0, Math.min(W - 1.001, fx)); fy = Math.max(0, Math.min(H - 1.001, fy));
      const x0 = fx | 0, y0 = fy | 0, ax = fx - x0, ay = fy - y0, q = y0 * W + x0;
      const bl = (a) => (a[q] * (1 - ax) + a[q + 1] * ax) * (1 - ay) + (a[q + W] * (1 - ax) + a[q + W + 1] * ax) * ay;
      const c = bl(this.cov), l = bl(this.lum);
      T[k] = c * (0.06 + 0.94 * Math.pow(l, 1.3));
      const qn = Math.round(fy) * W + Math.round(fx);
      sim.TE[k] = this.e[qn]; sim.TF[k] = this.f[qn]; sim.TEX[k] = this.ex[qn]; sim.TEY[k] = this.ey[qn];
      if (c > 0.05) { TR[k] = bl(this.cr) / Math.max(c, 0.25); TG[k] = bl(this.cg) / Math.max(c, 0.25); TB[k] = bl(this.cb) / Math.max(c, 0.25); }
      TA[k] = 0;
    }
    void t;
  }
  dispose() { this.creature?.dispose(); this.renderer.dispose(); this.rt.dispose(); }
}
