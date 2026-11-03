#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// Builds astrology.json (docs/PACKAGES.md): sign and planet glyph masks, ring masks, sphere maps
// for the Sun, Moon and planets, and the layouts the molty-astro skill plays. Drawings only:
// the sky itself is computed by the skill and arrives as params. Deterministic; run it after any
// change here and commit the output (`node packages/astrology/build.mjs`, then
// `node tools/scripts/check-packages.mjs --write-index`).
import { writeFileSync } from 'node:fs';

const OUT = new URL('astrology.json', import.meta.url);
const n3 = (v) => +v.toFixed(3) + 0;
const TAU = Math.PI * 2;
const DIM = '#3a3e4e', GRID = '#565b72', INK = '#c8cce0', SIGN_INK = ['#ff9a7a', '#a8d890', '#9fc0ff'];
export const SIGNS = ['aries', 'taurus', 'gemini', 'cancer', 'leo', 'virgo', 'libra', 'scorpio', 'sagittarius', 'capricorn', 'aquarius', 'pisces'];
const ASPECT_INK = { sextiles: '#6fe0a0', squares: '#ff6a55', trines: '#7fb0ff', oppositions: '#ff5fa0' };
export const BODIES = { sun: '#ffd678', moon: '#e6e6e0', mercury: '#c8bcb0', venus: '#fff0c4', mars: '#ff7a55', jupiter: '#ffc266', saturn: '#e8d890', uranus: '#9fe3e0', neptune: '#8fb4ff', pluto: '#c0a0d0' };

// ---- strokes, in a -1..1 box, y up
const circle = (cx, cy, r, a0 = 0, a1 = TAU, n = 24) => Array.from({ length: n + 1 }, (_, i) => { const a = a0 + ((a1 - a0) * i) / n; return [cx + r * Math.cos(a), cy + r * Math.sin(a)]; });
const DEG = Math.PI / 180;
// The twelve signs, as drawn in the lab (molty-creature-lab/astronomy/sky.mjs).
const SIGN_STROKES = {
  aries: [[[-0.75, 0.45], [-0.45, 0.8], [-0.12, 0.5], [0, -0.8]], [[0, -0.8], [0.12, 0.5], [0.45, 0.8], [0.75, 0.45]]],
  taurus: [circle(0, -0.3, 0.48), [[-0.75, 0.85], [-0.42, 0.35], [0, 0.18], [0.42, 0.35], [0.75, 0.85]]],
  gemini: [[[-0.65, 0.85], [0, 0.65], [0.65, 0.85]], [[-0.65, -0.85], [0, -0.65], [0.65, -0.85]], [[-0.3, 0.7], [-0.3, -0.7]], [[0.3, 0.7], [0.3, -0.7]]],
  cancer: [circle(-0.45, 0.25, 0.25), circle(0.45, -0.25, 0.25), [[-0.5, 0.5], [-0.1, 0.75], [0.35, 0.7], [0.8, 0.4]], [[0.5, -0.5], [0.1, -0.75], [-0.35, -0.7], [-0.8, -0.4]]],
  leo: [circle(-0.42, 0.05, 0.28), [[-0.25, 0.27], [-0.2, 0.68], [0.15, 0.85], [0.42, 0.55], [0.3, 0], [0.15, -0.5], [0.35, -0.8], [0.7, -0.6]]],
  virgo: [[[-0.85, 0.65], [-0.62, 0.75], [-0.55, -0.7]], [[-0.55, 0.45], [-0.3, 0.75], [-0.12, 0.45], [-0.12, -0.7]], [[-0.12, 0.45], [0.12, 0.75], [0.3, 0.45], [0.3, -0.5], [0.55, -0.85]], [[0.3, 0.3], [0.7, 0.3], [0.6, -0.2], [0.05, -0.65]]],
  libra: [[[-0.8, -0.65], [0.8, -0.65]], [[-0.8, -0.2], [-0.25, -0.2], [-0.42, 0.25], [0, 0.65], [0.42, 0.25], [0.25, -0.2], [0.8, -0.2]]],
  scorpio: [[[-0.85, 0.65], [-0.62, 0.75], [-0.55, -0.7]], [[-0.55, 0.45], [-0.3, 0.75], [-0.12, 0.45], [-0.12, -0.7]], [[-0.12, 0.45], [0.12, 0.75], [0.3, 0.45], [0.3, -0.5], [0.5, -0.72], [0.8, -0.55]], [[0.6, -0.35], [0.82, -0.55], [0.6, -0.8]]],
  sagittarius: [[[-0.75, -0.8], [0.8, 0.8]], [[0.15, 0.8], [0.8, 0.8], [0.8, 0.15]], [[-0.5, 0.15], [0.2, -0.5]]],
  capricorn: [[[-0.85, 0.6], [-0.55, 0.7], [-0.35, -0.5]], [[-0.35, 0.3], [0, 0.7], [0.15, 0.2], [0.05, -0.65], [-0.25, -0.85]], [[0.12, -0.2], [0.5, -0.05], [0.72, -0.4], [0.45, -0.7], [0.08, -0.4]]],
  aquarius: [0.3, -0.3].map((y) => [[-0.85, y - 0.1], [-0.5, y + 0.25], [-0.2, y - 0.2], [0.15, y + 0.25], [0.45, y - 0.2], [0.85, y + 0.15]]),
  pisces: [[[-0.65, 0.85], [-0.28, 0], [-0.65, -0.85]], [[0.65, 0.85], [0.28, 0], [0.65, -0.85]], [[-0.45, 0], [0.45, 0]]],
};
// Planet symbols, drawn for this package (the lab used font glyphs).
const BODY_STROKES = {
  sun: [circle(0, 0, 0.8), circle(0, 0, 0.12, 0, TAU, 8)],
  moon: [circle(0, 0, 0.8, -100 * DEG, 100 * DEG, 16), circle(-0.6, 0, 0.913, 59.7 * DEG, -59.7 * DEG, 12)],
  mercury: [circle(0, 0.05, 0.35), [[0, -0.3], [0, -0.92]], [[-0.25, -0.62], [0.25, -0.62]], circle(0, 0.62, 0.32, 200 * DEG, 340 * DEG, 8)],
  venus: [circle(0, 0.3, 0.45), [[0, -0.15], [0, -0.92]], [[-0.32, -0.55], [0.32, -0.55]]],
  mars: [circle(-0.2, -0.2, 0.48), [[0.14, 0.14], [0.75, 0.75]], [[0.3, 0.75], [0.75, 0.75], [0.75, 0.3]]],
  jupiter: [[[-0.6, 0.5], [-0.35, 0.8], [-0.05, 0.65], [-0.1, 0.3], [-0.6, -0.3]], [[-0.6, -0.3], [0.65, -0.3]], [[0.3, 0.7], [0.3, -0.9]]],
  saturn: [[[-0.25, 0.9], [-0.25, -0.3]], [[-0.55, 0.6], [0.05, 0.6]], [[-0.25, 0.1], [0.05, 0.35], [0.4, 0.2], [0.35, -0.2], [0.1, -0.55], [0.3, -0.85], [0.5, -0.7]]],
  uranus: [[[-0.45, 0.8], [-0.45, 0]], [[0.45, 0.8], [0.45, 0]], [[-0.45, 0.4], [0.45, 0.4]], [[0, 0.8], [0, -0.42]], circle(0, -0.64, 0.22, 0, TAU, 12)],
  neptune: [[[-0.6, 0.7], [-0.5, 0.1], [0, -0.1], [0.5, 0.1], [0.6, 0.7]], [[0, 0.8], [0, -0.92]], [[-0.3, -0.55], [0.3, -0.55]]],
  pluto: [[[-0.4, 0.85], [-0.4, -0.85], [0.45, -0.85]], [[-0.4, 0.85], [0.15, 0.85], [0.42, 0.6], [0.42, 0.35], [0.15, 0.1], [-0.4, 0.1]]],
};
/** Bounding box on the origin: a symbol must sit in the middle of where it is drawn. */
const centred = (strokes) => { const all = strokes.flat(), xs = all.map((p) => p[0]), ys = all.map((p) => p[1]), cx = (Math.min(...xs) + Math.max(...xs)) / 2, cy = (Math.min(...ys) + Math.max(...ys)) / 2; return strokes.map((s) => s.map(([x, y]) => [x - cx, y - cy])); };
// Catmull-Rom through the points, as the lab's curves
const smooth = (p) => { if (p.length < 3) return p; const out = []; for (let i = 0; i + 1 < p.length; i++) { const a = p[Math.max(0, i - 1)], b = p[i], c = p[i + 1], d = p[Math.min(p.length - 1, i + 2)]; for (let k = 0; k < 8; k++) { const t = k / 8, f = (j) => 0.5 * (2 * b[j] + (c[j] - a[j]) * t + (2 * a[j] - 5 * b[j] + 4 * c[j] - d[j]) * t * t + (3 * b[j] - a[j] - 3 * c[j] + d[j]) * t * t * t); out.push([f(0), f(1)]); } } out.push(p.at(-1)); return out; };
const segDist = (px, py, [ax, ay], [bx, by]) => { const dx = bx - ax, dy = by - ay, l = dx * dx + dy * dy, t = l ? Math.max(0, Math.min(1, ((px - ax) * dx + (py - ay) * dy) / l)) : 0; return Math.hypot(px - ax - t * dx, py - ay - t * dy); };

// ---- assets
const b64 = (bytes) => Buffer.from(bytes).toString('base64');
/** strokes (box half-extents bx, by) -> mask asset, `width` in box units */
function mask(strokes, w, h, width, bx = 1, by = 1, keep = () => true) {
  const lines = strokes.map(smooth), bits = new Uint8Array(Math.ceil((w * h) / 8));
  for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) {
    const x = (((i + 0.5) / w) * 2 - 1) * bx, y = (1 - ((j + 0.5) / h) * 2) * by;
    if (!keep(x, y)) continue;
    if (lines.some((l) => l.some((p, k) => k > 0 && segDist(x, y, l[k - 1], p) <= width / 2))) bits[(j * w + i) >> 3] |= 0x80 >> ((j * w + i) & 7);
  }
  return { kind: 'mask', w, h, bits: b64(bits) };
}

// Sphere maps (equirectangular: longitude -180..180 across, latitude 90..-90 down), from the lab's surfaces (plot.mjs).
const hash = (a, b) => { let h = (Math.imul(a | 0, 374761393) + Math.imul(b | 0, 668265263)) | 0; h = Math.imul(h ^ (h >>> 13), 1274126177); return ((h ^ (h >>> 16)) >>> 0) / 4294967296; };
const vnoise = (x, y) => { const i = Math.floor(x), j = Math.floor(y), u = x - i, v = y - j, s = (t) => t * t * (3 - 2 * t), a = hash(i, j), b = hash(i + 1, j), c = hash(i, j + 1), d = hash(i + 1, j + 1); return (a + (b - a) * s(u)) * (1 - s(v)) + (c + (d - c) * s(u)) * s(v); };
const fbm = (x, y) => { const wx = x + 0.6 * vnoise(x * 1.7 + 9, y * 1.7), wy = y + 0.6 * vnoise(x * 1.7, y * 1.7 + 5); return vnoise(wx, wy) * 0.55 + vnoise(wx * 2.1 + 7, wy * 2.1) * 0.3 + vnoise(wx * 4.3, wy * 4.3 + 3) * 0.15; };
const band = (v, a, b) => { const t = Math.min(1, Math.max(0, (v - a) / (b - a))); return t * t * (3 - 2 * t); };
const mix = (a, b, t) => a.map((v, i) => v + (b[i] - v) * t);
// the near side's seas, as soft ellipses (x, y, rx, ry; north up), and a few craters
const MARIA = [[-0.28, 0.5, 0.3, 0.26], [0.2, 0.46, 0.17, 0.15], [0.42, 0.14, 0.2, 0.17], [0.76, 0.28, 0.1, 0.13], [0.68, -0.08, 0.13, 0.17], [-0.68, 0.22, 0.2, 0.4], [-0.3, -0.32, 0.2, 0.18], [-0.56, -0.4, 0.12, 0.1], [0.05, 0.1, 0.1, 0.08]];
const CRATERS = [[0.12, -0.6, 0.11], [-0.45, 0.72, 0.09], [0.5, 0.55, 0.07], [-0.2, -0.75, 0.08], [0.72, -0.35, 0.08], [0.35, -0.2, 0.05], [-0.72, -0.15, 0.06], [0.1, 0.25, 0.05]];
const crater = (x, y) => CRATERS.reduce((m, [a, b, r]) => { const d = Math.hypot(x - a, y - b) / r; return d < 1 ? Math.min(m, 0.82 + 0.18 * d) : d < 1.3 ? Math.max(m, 1.12) : m; }, 1);
const moonAlb = (lo, la) => { const x = Math.sin(lo) * Math.cos(la), y = Math.sin(la); let sea = 0; for (const [a, b, rx, ry] of MARIA) sea = Math.max(sea, 1 - band(((x - a) / rx) ** 2 + ((y - b) / ry) ** 2, 0.55, 1.25)); const ray = 1 - band(Math.hypot(x + 0.1, y + 0.72), 0.03, 0.09); return Math.min(1, (0.92 - 0.55 * sea + 0.1 * fbm(lo * 7, la * 7) + 0.3 * ray) * (Math.cos(lo) > 0 ? crater(x, y) : 1)); };
const SURFACE = {
  moon: (lo, la) => [[232, 232, 226], moonAlb(lo, la)],
  'moon-red': (lo, la) => [[255, 96, 60], 0.45 + 0.4 * moonAlb(lo, la)],
  sun: (lo, la) => [[255, 206, 104], 0.72 + 0.4 * fbm(lo * 8, la * 8)],
  mercury: (lo, la) => [[214, 202, 190], 0.5 + 0.7 * fbm(lo * 6, la * 6)],
  venus: (lo, la) => [[255, 238, 190], 0.72 + 0.28 * fbm(lo * 1.5 + Math.sin(la * 3), la * 5)],
  mars: (lo, la) => (Math.abs(la) > 1.2 - 0.25 * fbm(lo * 2, 2) ? [[255, 250, 245], 1] : [[255, 112, 66], 0.42 + 0.62 * band(fbm(lo * 2.2 + 4, la * 2.8), 0.34, 0.6)]),
  jupiter: (lo, la) => { const w = la + 0.05 * Math.sin(lo * 7 + la * 11), b = Math.sin(w * 9.5) * 0.5 + 0.5, dl = lo - 0.5, dy = la + 0.36; if ((dl * dl) / 0.05 + (dy * dy) / 0.012 < 1) return [[255, 95, 60], 1]; return [mix([176, 104, 62], [255, 232, 190], b), 0.55 + 0.45 * b]; },
  saturn: (lo, la) => { const b = Math.sin(la * 7) * 0.5 + 0.5; return [mix([206, 164, 96], [255, 236, 176], b), 0.72 + 0.28 * b]; },
};
/** rgb pixels -> 15 colours (k-means, deterministic) + index 0 transparent, 4 bits per pixel */
function image(px, w, h) {
  let pal = Array.from({ length: 15 }, (_, k) => px[Math.floor(((k + 0.5) / 15) * px.length)].slice()), idx = new Uint8Array(px.length);
  const near = (p) => { let best = 0, bd = Infinity; pal.forEach((c, k) => { const d = (p[0] - c[0]) ** 2 + (p[1] - c[1]) ** 2 + (p[2] - c[2]) ** 2; if (d < bd) { bd = d; best = k; } }); return best; };
  for (let it = 0; it < 12; it++) {
    const sum = pal.map(() => [0, 0, 0, 0]);
    px.forEach((p, i) => { const k = (idx[i] = near(p)); sum[k][0] += p[0]; sum[k][1] += p[1]; sum[k][2] += p[2]; sum[k][3]++; });
    pal = pal.map((c, k) => (sum[k][3] ? sum[k].slice(0, 3).map((v) => v / sum[k][3]) : c));
  }
  const bytes = new Uint8Array(Math.ceil((w * h) / 2));
  px.forEach((p, i) => { bytes[i >> 1] |= (near(p) + 1) << (i & 1 ? 0 : 4); });
  const hex = (c) => '#' + c.map((v) => Math.round(Math.max(0, Math.min(255, v))).toString(16).padStart(2, '0')).join('');
  return { kind: 'image', w, h, px: b64(bytes), palette: ['#000000', ...pal.map(hex)] };
}
function sphereMap(name, w = 128, h = 64) {
  const px = [];
  for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) { const [c, a] = SURFACE[name](((i + 0.5) / w) * TAU - Math.PI, Math.PI / 2 - ((j + 0.5) / h) * Math.PI); px.push(c.map((v) => v * Math.min(1.1, a))); }
  return image(px, w, h);
}
/**
 * Saturn's rings seen at `tilt` degrees: the near half only (below the centre for a positive tilt,
 * above for a negative one). Drawn twice: turned 180 degrees behind the ball, as is in front of it.
 */
function rings(tilt) {
  const R = 0.34, s = Math.max(0.08, Math.abs(Math.sin(tilt * DEG))), bx = 2.15 * R, by = Math.max(0.06, bx * s + 0.03), front = tilt >= 0 ? (x, y) => y <= 0 : (x, y) => y >= 0;
  const ellipses = [1.28, 1.42, 1.56, 1.7, 2, 2.12].map((k) => circle(0, 0, 1, 0, TAU, 48).map(([x, y]) => [x * k * R, y * k * R * s]));
  const w = 112, h = Math.max(6, 2 * Math.round((w * by) / bx / 2));
  return mask(ellipses, w, h, 0.012, bx, by, front);
}
const RING_TILTS = [3, 9, 15, 21, 27];

const assets = {};
for (const s of SIGNS) assets[s] = mask(centred(SIGN_STROKES[s]), 64, 64, 0.085);
for (const b of Object.keys(BODIES)) assets[`g-${b}`] = mask(centred(BODY_STROKES[b]), 32, 32, 0.17);
for (const t of RING_TILTS) { assets[`rings-p${t}`] = rings(t); assets[`rings-n${t}`] = rings(-t); }
for (const s of Object.keys(SURFACE)) assets[s] = sphereMap(s);

// ---- layouts. Geometry in glass units (radius 1, y up), angles in degrees, counter-clockwise from the right.
const wheel = () => [
  { k: 'ring', r: 0.9, color: INK }, { k: 'ring', r: 0.75, color: GRID },
  { k: 'ticks', r: 0.75, len: 0.15, n: 12, a0: 0, color: INK }, { k: 'ticks', r: 0.73, len: 0.02, n: 36, a0: 0, color: GRID },
  // 0 degrees Aries at 9 o'clock, longitude counter-clockwise: sign i is centred at 195 + 30 i
  ...SIGNS.map((s, i) => ({ k: 'glyph', asset: s, size: 0.09, polar: [n3((195 + 30 * i) % 360), 0.825], color: SIGN_INK[i % 3] })),
];
const num = 'number', text = 'text', list = 'list';
// The lab's tonight is a run of shots; here each is its own small layout, and the skill queues them:
// globe (the moon, then each planet), saturn (with its rings), sign (the moon's sign).
const SPIN = { sun: 6 }; // degrees per second, for show
const each = (list, item) => ({ ...item, each: list });
const layouts = {
  globe: { dur: 2.5, params: { img: text, lit: num, side: num, r: num, spin: num }, items: [{ k: 'ball', r: '$r', img: '$img', lit: '$lit', side: '$side', spin: '$spin' }] },
  saturn: {
    dur: 2.5,
    params: { lit: num, side: num, rings: text },
    items: [
      { k: 'glyph', size: 1.462, asset: '$rings', rot: 180, color: '#ead9a0' },
      { k: 'ball', r: 0.34, img: 'saturn', lit: '$lit', side: '$side', spin: 17 },
      { k: 'glyph', size: 1.462, asset: '$rings', rot: 0, color: '#ead9a0' },
    ],
  },
  sign: { dur: 4, params: { sign: text }, items: [{ k: 'glyph', size: 1.2, asset: '$sign', color: '#ffe9b0' }] },
  // bodies: {g: glyph asset, a: chart angle, r: radius, c: colour}; each aspect list: lines {x1, y1, x2, y2} in its colour
  chart: {
    dur: 0,
    params: { inner: num, bodies: list, ...Object.fromEntries(Object.keys(ASPECT_INK).map((k) => [k, list])) },
    items: [
      ...wheel(), { k: 'ring', r: '$inner', color: DIM },
      ...Object.entries(ASPECT_INK).map(([k, c]) => each(`$${k}`, { k: 'line', pts: [['@x1', '@y1'], ['@x2', '@y2']], color: c })),
      each('$bodies', { k: 'ticks', r: 0.66, len: 0.07, n: 1, a0: '@a', color: GRID }),
      each('$bodies', { k: 'glyph', asset: '@g', size: 0.12, polar: ['@a', '@r'], color: '@c' }),
    ],
  },
  signs: (() => {
    // all twelve, then each in turn, then all twelve again (the lab's timing)
    const grid = (on, off) => SIGNS.map((s, i) => ({ k: 'glyph', on, off, asset: s, size: 0.34, x: n3(-0.63 + (i % 4) * 0.42), y: n3(0.46 - (i >> 2) * 0.46), color: SIGN_INK[i % 3] }));
    const one = SIGNS.map((s, i) => ({ k: 'glyph', on: n3(4.2 + i * 2.3), off: n3(4.2 + (i + 1) * 2.3), asset: s, size: 1.1, x: 0, y: 0, color: SIGN_INK[i % 3] }));
    return { dur: 36, params: {}, items: [...grid(0.2, 4.2), ...one, ...grid(31.8, 35.8)] };
  })(),
  // a, b: the two bodies' chart angles (the arc runs a to b); lines: the aspect's figure and chord {x1, y1, x2, y2, c, on}
  aspect: {
    dur: 5,
    params: { a: num, b: num, c: text, bodies: list, lines: list, deg: text, tx: num, ty: num },
    items: [
      ...wheel(),
      { k: 'ticks', r: 0.1, len: 0.62, n: 12, a0: 0, color: DIM }, { k: 'ring', r: 0.2, color: DIM }, { k: 'ring', r: 0.4, color: DIM }, { k: 'ring', r: 0.6, color: GRID },
      each('$lines', { k: 'line', on: '@on', pts: [['@x1', '@y1'], ['@x2', '@y2']], color: '@c' }),
      { k: 'arc', on: 0.4, r: 0.28, a0: '$a', a1: '$b', color: '$c' },
      { k: 'ticks', r: 0, len: 0.6, n: 1, a0: '$a', color: '$c' },
      { k: 'ticks', on: 0.4, r: 0, len: 0.6, n: 1, a0: '$b', color: '$c' },
      { k: 'text', on: 0.4, x: '$tx', y: '$ty', size: 0.2, str: '$deg', color: '#ffffff' },
      each('$bodies', { k: 'glyph', asset: '@g', size: 0.13, polar: ['@a', '@r'], color: '@c' }),
    ],
  },
  // Events: a body crossing another, from a path of keyframes {x, y, on, off} the skill computes.
  transit: { dur: 6, params: { path: list }, items: [{ k: 'ball', r: 0.55, img: 'sun', spin: SPIN.sun }, { k: 'disc', each: '$path', x: '@x', y: '@y', on: '@on', off: '@off', r: 0.05, color: '#000000' }] },
  'solar-eclipse': { dur: 6, params: { path: list, moon_r: num }, items: [{ k: 'ball', r: 0.5, img: 'sun', spin: SPIN.sun }, { k: 'disc', each: '$path', x: '@x', y: '@y', on: '@on', off: '@off', r: '$moon_r', color: '#000000' }] },
  occultation: { dur: 6, params: { path: list, planet: text }, items: [{ k: 'ball', r: 0.09, img: '$planet', spin: 10 }, { k: 'ball', each: '$path', x: '@x', y: '@y', on: '@on', off: '@off', r: 0.45, img: 'moon', lit: 0.99 }] },
  appulse: { dur: 6, params: { path: list, a: text, b: text }, items: [{ k: 'ball', r: 0.2, img: '$a', spin: 10 }, { k: 'ball', each: '$path', x: '@x', y: '@y', on: '@on', off: '@off', r: 0.13, img: '$b', spin: 10 }] },
  'lunar-eclipse': { dur: 6, params: { path: list, umbra: num, pen: num, moon_r: num }, items: [{ k: 'ring', r: '$pen', color: DIM }, { k: 'ring', r: '$umbra', color: '#8a3a2a' }, { k: 'ball', each: '$path', x: '@x', y: '@y', on: '@on', off: '@off', r: '$moon_r', img: '@img' }] },
};

const pkg = { v: 'molty-pkg/1', id: 'astrology', name: 'Astrology', version: '1.0.0', kind: 'scenes', assets, layouts };
writeFileSync(OUT, JSON.stringify(pkg) + '\n');
console.log(`astrology.json: ${Object.keys(assets).length} assets, ${Object.keys(layouts).length} layouts, ${Buffer.byteLength(JSON.stringify(pkg)) + 1} bytes`);
