#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// Reference player for molty-pkg/1 scenes: draws a layout with its params onto Molty's braille
// dot lattice (52x32 cells of 2x4 dots on the 412x412 round panel) and writes a PNG, so a package
// can be checked without hardware and the firmware's scene_player compared against it.
//
//   play.mjs <pkg> <layout> [--params '<json>'|file] [--t 1.5] [-o out.png]
//   play.mjs --commands <file|-> --stills <dir> [--every 1]
//     replays the scene.play / scene.queue commands an agent sent (JSON lines, {name, args}),
//     one still every `--every` seconds of each scene
//   --hd [--size 466] [--caption word]: the calm view in high definition instead, as the firmware draws
//     a scene with no touch on it (fluid_draw.c's shapes): anti-aliased at the panel's own pixels, a
//     ball or an image on the dots, a hairline one fine pixel (not the fluid's faint dust around it, which the
//     device adds); --caption puts a subtitle word at the bottom
// <pkg> is a package id (packages/<id>/<id>.json) or a path to its JSON. No dependencies.
import { mkdirSync, readFileSync, writeFileSync, existsSync } from 'node:fs';
import { deflateSync } from 'node:zlib';
import { join } from 'node:path';
import { bind, ParamError } from './format.mjs';

const ROOT = new URL('../../', import.meta.url).pathname;
export const CELLS_W = 52, CELLS_H = 32, PANEL = 412;
const W = CELLS_W * 2, H = CELLS_H * 4, CW = PANEL / CELLS_W, CH = PANEL / CELLS_H, DEG = Math.PI / 180;
const BAYER = [0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5].map((v) => (v + 0.5) / 16);

// the lattice (as device/creatures/js/fluid's braille): dot centres in panel px, and glass units (radius 1, y up)
const dotPx = (sx, sy) => [((sx >> 1) + (sx & 1 ? 0.7 : 0.3)) * CW, ((sy >> 2) + 0.14 + 0.24 * (sy & 3)) * CH];
const glass = (sx, sy) => { const [px, py] = dotPx(sx, sy); return [(px / PANEL) * 2 - 1, 1 - (py / PANEL) * 2]; };
const snap = (x, y) => { const px = ((x + 1) / 2) * PANEL, py = ((1 - y) / 2) * PANEL, cx = Math.floor(px / CW), cy = Math.floor(py / CH); return [cx * 2 + (px / CW - cx > 0.5 ? 1 : 0), cy * 4 + Math.max(0, Math.min(3, Math.round((py / CH - cy - 0.14) / 0.24)))]; };
const PITCH = [2 / W, 2 / H]; // dot spacing in glass units, roughly
const hash = (a, b) => { let h = (Math.imul(a, 374761393) + Math.imul(b, 668265263)) | 0; h = Math.imul(h ^ (h >>> 13), 1274126177); return ((h ^ (h >>> 16)) >>> 0) / 4294967296; };
const EARTHSHINE = [58, 62, 76];
const hex = (c) => [1, 3, 5].map((i) => parseInt(c.slice(i, i + 2), 16));

/** Dots inside a glass-unit box: fn(sx, sy, x, y). */
function each([x0, x1, y0, y1], fn) {
  const [a, b] = snap(x0, y1), [c, d] = snap(x1, y0);
  for (let sy = Math.max(0, b - 2); sy <= Math.min(H - 1, d + 2); sy++) for (let sx = Math.max(0, a - 2); sx <= Math.min(W - 1, c + 2); sx++) fn(sx, sy, ...glass(sx, sy));
}

// a stroke no wider than a dot is a hairline (the firmware's scene_player.c hair_*): a DDA in dot coordinates, one
// dot a step along its major axis and 8-connected, as drawille; along a path a corner dot (its neighbours touch) drops
const HAIR = 1 / Math.min(W, H), rnd = (v) => Math.floor(v + 0.5);
function hair(pts, rgb, put, skip = 0) {
  let a = null, b = null, pen = null;
  const commit = () => { if (skip) skip--; else put(b[0], b[1], rgb); };
  const dot = (x, y) => {
    if (b && x === b[0] && y === b[1]) return;
    if (a && Math.abs(x - a[0]) <= 1 && Math.abs(y - a[1]) <= 1) { if (x === a[0] && y === a[1]) a = null; b = [x, y]; return; }
    if (b) { commit(); a = b; }
    b = [x, y];
  };
  for (const [x, y] of pts) {
    const X = ((x + 1) / 2) * W - 0.5, Y = ((1 - y) / 2) * H - 0.5;
    if (pen) {
      const dx = X - pen[0], dy = Y - pen[1], xm = Math.abs(dx) >= Math.abs(dy), p = xm ? pen[0] : pen[1], q = xm ? pen[1] : pen[0], d = xm ? dx : dy, k = d ? (xm ? dy : dx) / d : 0;
      const m1 = rnd(p + d), st = m1 >= rnd(p) ? 1 : -1;
      for (let m = rnd(p); ; m += st) { const o = rnd(q + k * (m - p)); if (xm) dot(m, o); else dot(o, m); if (m === m1) break; }
    } else dot(rnd(X), rnd(Y));
    pen = [X, Y];
  }
  if (b) commit();
}
// a hairline ring or arc as the firmware walks it: a whole ring from one step before its start to one after, its first dot left to the end
function hairRing(it, whole, rgb, put) {
  const span = whole ? 360 : (((it.a1 - it.a0) % 360) + 360) % 360, n = 8 + Math.floor(span * DEG * it.r * 0.5 * Math.max(W, H)), pts = [];
  for (let q = whole ? -1 : 0; q <= (whole ? n + 1 : n); q++) { const a = (it.a0 + (span * q) / n) * DEG; pts.push([it.r * Math.cos(a), it.r * Math.sin(a)]); }
  hair(pts, rgb, put, whole ? 1 : 0);
}

function stroke(pts, c, put, w = 0) {
  const rgb = hex(c), r = Math.max(0, w / 2);
  if (r <= HAIR) return hair(pts, rgb, put);
  for (let i = 0; i + 1 < pts.length; i++) {
    const [a, b] = [pts[i], pts[i + 1]], n = Math.max(1, Math.ceil(Math.hypot(b[0] - a[0], b[1] - a[1]) / 0.006));
    for (let s = 0; s <= n; s++) {
      const x = a[0] + ((b[0] - a[0]) * s) / n, y = a[1] + ((b[1] - a[1]) * s) / n;
      each([x - r, x + r, y - r, y + r], (sx, sy, gx, gy) => { if (Math.hypot(gx - x, gy - y) <= r) put(sx, sy, rgb); });
    }
  }
}
const arcPts = (r, a0, a1) => { const n = Math.max(2, Math.ceil((Math.abs(a1 - a0) * DEG * r) / 0.02)); return Array.from({ length: n + 1 }, (_, i) => { const a = (a0 + ((a1 - a0) * i) / n) * DEG; return [r * Math.cos(a), r * Math.sin(a)]; }); };
const smooth = (p) => { if (p.length < 3) return p; const out = []; for (let i = 0; i + 1 < p.length; i++) { const a = p[Math.max(0, i - 1)], b = p[i], c = p[i + 1], d = p[Math.min(p.length - 1, i + 2)]; for (let k = 0; k < 8; k++) { const t = k / 8, f = (j) => 0.5 * (2 * b[j] + (c[j] - a[j]) * t + (2 * a[j] - 5 * b[j] + 4 * c[j] - d[j]) * t * t + (3 * b[j] - a[j] - 3 * c[j] + d[j]) * t * t * t); out.push([f(0), f(1)]); } } out.push(p.at(-1)); return out; };

// assets, decoded once per package
const decoded = new WeakMap();
function asset(pkg, name) {
  let m = decoded.get(pkg); if (!m) decoded.set(pkg, (m = new Map()));
  if (!m.has(name)) {
    const a = pkg.assets[name], b = Buffer.from(a.kind === 'mask' ? a.bits : a.px, 'base64');
    const at = a.kind === 'mask' ? (i, j) => (b[(j * a.w + i) >> 3] >> (7 - ((j * a.w + i) & 7))) & 1 : (i, j) => (b[(j * a.w + i) >> 1] >> ((j * a.w + i) & 1 ? 0 : 4)) & 15;
    m.set(name, { ...a, at, rgb: a.palette?.map(hex) });
  }
  return m.get(name);
}

/** A sphere wearing an equirectangular image, lit from `side` with `lit` of its disc lit; the dark side hides what is behind. */
function ball(pkg, it, t, put) {
  const img = it.img ? asset(pkg, it.img) : null, plain = hex(it.color), pa = Math.acos(Math.max(-1, Math.min(1, 2 * it.lit - 1))), side = it.side < 0 ? -1 : 1, L = [Math.sin(pa) * side, Math.cos(pa)];
  // a nearly full body has no terminator to show its roundness: the form light sits off to one side (the lab's rule)
  const off = Math.max(0, 1 - pa / 0.9), K0 = [L[0] - 0.5 * off, 0.5 * off, L[1]], kn = Math.hypot(...K0), K = K0.map((v) => v / kn), turn = it.spin * t * DEG;
  each([it.x - it.r, it.x + it.r, it.y - it.r, it.y + it.r], (sx, sy, gx, gy) => {
    const u = (gx - it.x) / it.r, v = (gy - it.y) / it.r, d2 = u * u + v * v;
    if (d2 > 1) return;
    const z = Math.sqrt(1 - d2), lit = u * L[0] + z * L[1];
    // the dark side hides what is behind it, with a faint earthshine so a thin crescent still reads as a ball
    if (lit <= 0) return put(sx, sy, hash(sx, sy) < 0.05 ? EARTHSHINE : null);
    const lon = Math.atan2(u, z) + turn, lat = Math.asin(Math.max(-1, Math.min(1, v)));
    const k = img ? img.at(Math.floor((((lon / (2 * Math.PI) + 0.5) % 1) + 1) % 1 * img.w), Math.min(img.h - 1, Math.floor((0.5 - lat / Math.PI) * img.h))) : 1;
    if (!k) return put(sx, sy, null);
    const col = img ? img.rgb[k] : plain, lum = (0.3 * col[0] + 0.59 * col[1] + 0.11 * col[2]) / 255;
    const form = Math.max(0, u * K[0] + v * K[1] + z * K[2]), tone = Math.min(1.08, (0.1 + 1.05 * form ** 0.85) * (0.55 + 0.45 * z ** 0.5));
    const density = Math.min(1, 0.72 + 0.4 * Math.sqrt(lit)) * Math.min(1, 0.3 + lum);
    if (density <= BAYER[((sy & 3) << 2) | (sx & 3)]) return put(sx, sy, null);
    const hot = Math.max(0, tone - 0.82) * 1.6, m = Math.min(1, tone);
    put(sx, sy, [Math.min(255, col[0] * m + 128 * hot), Math.min(255, col[1] * m + 122 * hot), Math.min(255, col[2] * m + 112 * hot)]);
  });
}

/** A mask (in the item's colour) or image, `size` wide, centred on x, y, turned `rot` degrees. */
function glyph(pkg, it, put) {
  const a = asset(pkg, it.asset), w = it.size, h = (it.size * a.h) / a.w, c = Math.cos(-it.rot * DEG), s = Math.sin(-it.rot * DEG), R = Math.hypot(w, h) / 2, rgb = hex(it.color);
  const sample = (gx, gy) => { const dx = gx - it.x, dy = gy - it.y, lx = dx * c - dy * s, ly = dx * s + dy * c, i = Math.floor((lx / w + 0.5) * a.w), j = Math.floor((0.5 - ly / h) * a.h); return i < 0 || j < 0 || i >= a.w || j >= a.h ? 0 : a.at(i, j); };
  each([it.x - R, it.x + R, it.y - R, it.y + R], (sx, sy, gx, gy) => {
    if (a.kind === 'image') { const k = sample(gx, gy); if (k) put(sx, sy, a.rgb[k]); return; }
    // a dot is on when the mask covers any part of it: thin strokes survive a small size
    for (const ox of [-1, 0, 1]) for (const oy of [-1, 0, 1]) if (sample(gx + (ox * PITCH[0]) / 4, gy + (oy * PITCH[1]) / 4)) return put(sx, sy, rgb);
  });
}

// the device's own 5x7 ASCII font (device/firmware/main/fluid/glyph_font.c)
let FONT;
function font() {
  if (FONT) return FONT;
  const src = readFileSync(join(ROOT, 'device/firmware/main/fluid/glyph_font.c'), 'utf8'), rows = (name) => [...new RegExp(`${name}\\[\\d+\\]\\[7\\] = \\{([\\s\\S]*?)\\};`).exec(src)[1].matchAll(/\{([^}]*)\}/g)].map((m) => m[1].split(',').map(Number));
  const main = rows('FONT'), punct = rows('PUNCT'), full = [31, 31, 31, 31, 31, 31, 31];
  return (FONT = (ch) => { let c = ch.charCodeAt(0); if (ch === 'x') return [0, 0, 17, 10, 4, 10, 17]; if (c >= 97 && c <= 122) c -= 32; return c >= 32 && c <= 90 ? main[c - 32] : c >= 91 && c <= 96 ? punct[c - 91] : c >= 123 && c <= 126 ? punct[c - 117] : full; });
}
function text(it, put) {
  const f = font(), px = it.size / 7, len = it.str.length * 6 - 1, x0 = it.x - (len * px) / 2, y0 = it.y + it.size / 2, rgb = hex(it.color);
  each([x0, x0 + len * px, y0 - it.size, y0], (sx, sy, gx, gy) => {
    const col = Math.floor((gx - x0) / px), row = Math.floor((y0 - gy) / px), ch = Math.floor(col / 6), cx = col % 6;
    if (ch < 0 || ch >= it.str.length || cx > 4 || row < 0 || row > 6) return;
    if ((f(it.str[ch])[row] >> (4 - cx)) & 1) put(sx, sy, rgb);
  });
}

/** The dots of `layout` at `t` seconds: Map(dot index -> rgb). Throws ParamError for bad params.
 *  kept (a Map): also the dots no high-definition shape draws (a ball's, an image's), as they show. */
export function frame(pkg, layout, params, t, kept = null) {
  let keep = false;
  const dots = new Map(), put = (sx, sy, c) => {
    if (sx < 0 || sy < 0 || sx >= W || sy >= H) return;
    const k = sy * W + sx;
    if (c === null) dots.delete(k); else dots.set(k, c);
    if (kept && keep && c !== null) kept.set(k, c); else kept?.delete(k);
  };
  for (const it of bind(pkg, layout, params)) {
    if (t < it.on || t >= it.off) continue;
    keep = it.k === 'ball' || (it.k === 'glyph' && pkg.assets[it.asset].kind === 'image');
    const rgb = hex(it.color);
    if ((it.k === 'ring' || it.k === 'arc') && it.w / 2 <= HAIR) hairRing(it, it.k === 'ring' || it.a1 - it.a0 >= 360, rgb, put);
    else if (it.k === 'ring') stroke(arcPts(it.r, 0, 360), it.color, put, it.w);
    else if (it.k === 'arc') stroke(arcPts(it.r, it.a0, it.a1), it.color, put, it.w);
    else if (it.k === 'ticks') for (let i = 0; i < it.n; i++) { const a = (it.a0 + (360 * i) / it.n) * DEG, r1 = it.r + it.len; stroke([[it.r * Math.cos(a), it.r * Math.sin(a)], [r1 * Math.cos(a), r1 * Math.sin(a)]], it.color, put); }
    else if (it.k === 'line') stroke(it.pts, it.color, put, it.w);
    else if (it.k === 'curve') stroke(smooth(it.pts), it.color, put, it.w);
    else if (it.k === 'disc') each([it.x - it.r, it.x + it.r, it.y - it.r, it.y + it.r], (sx, sy, gx, gy) => { if (Math.hypot(gx - it.x, gy - it.y) <= it.r) put(sx, sy, it.color === '#000000' ? null : rgb); });
    else if (it.k === 'ball') ball(pkg, it, t, put);
    else if (it.k === 'glyph') glyph(pkg, it, put);
    else if (it.k === 'text') text(it, put);
  }
  return dots;
}

// ---- PNG
const CRC = Array.from({ length: 256 }, (_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
const crc32 = (b) => { let c = 0xffffffff; for (const x of b) c = CRC[(c ^ x) & 255] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; };
const chunk = (type, data) => { const t = Buffer.from(type), len = Buffer.alloc(4), crc = Buffer.alloc(4); len.writeUInt32BE(data.length); crc.writeUInt32BE(crc32(Buffer.concat([t, data]))); return Buffer.concat([len, t, data, crc]); };
/** The dots as the panel shows them: round dots on black, inside the round glass; over rgb (an S x S
 *  image of floats, hd's) when given. */
export function png(dots, rgb = null, S = PANEL) {
  const raw = Buffer.alloc((S * 3 + 1) * S), R = 1.35 * (S / PANEL);
  const set = (x, y, c) => { if (Math.hypot(x + 0.5 - S / 2, y + 0.5 - S / 2) > S / 2) return; const o = y * (S * 3 + 1) + 1 + x * 3; raw[o] = c[0]; raw[o + 1] = c[1]; raw[o + 2] = c[2]; };
  if (rgb) for (let y = 0; y < S; y++) for (let x = 0; x < S; x++) set(x, y, [0, 1, 2].map((q) => Math.round(rgb[(y * S + x) * 3 + q])));
  for (let a = 0; a < 720; a++) set(Math.round(S / 2 + (S / 2 - 1) * Math.cos((a * Math.PI) / 360)), Math.round(S / 2 + (S / 2 - 1) * Math.sin((a * Math.PI) / 360)), [40, 40, 40]);
  for (const [k, c] of dots) { const [px, py] = dotPx(k % W, Math.floor(k / W)).map((v) => (v * S) / PANEL); for (let y = Math.floor(py - R); y <= py + R; y++) for (let x = Math.floor(px - R); x <= px + R; x++) if ((x + 0.5 - px) ** 2 + (y + 0.5 - py) ** 2 <= R * R && x >= 0 && y >= 0 && x < S && y < S) set(x, y, c.map(Math.round)); }
  const head = Buffer.alloc(13); head.writeUInt32BE(S, 0); head.writeUInt32BE(S, 4); head[8] = 8; head[9] = 2;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', head), chunk('IDAT', deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}

// ---- high definition: the calm scene as the firmware's shapes draw it (scene_player.c makes them,
// fluid_draw.c's hd_row draws them): each item over what is under it by its anti-aliased coverage
const HD_EDGE = 0.42, ACCENT = [255, 140, 60], ORP = (n) => (n <= 1 ? 0 : n <= 5 ? 1 : n <= 9 ? 2 : n <= 13 ? 3 : 4);
const edge = (d) => (d >= 0.5 ? 0 : d <= -0.5 ? 1 : 0.5 - d), sstep = (a, b, x) => { const t = Math.max(0, Math.min(1, (x - a) / (b - a))); return t * t * (3 - 2 * t); };
/** The calm scene at S x S px (RGB floats) and the dots it keeps (a ball's, an image's): { rgb, kept }. */
export function hd(pkg, layout, params, t, S = 466, caption = '') {
  const rgb = new Float32Array(S * S * 3), k = S / 2, X = (x) => (x + 1) * k, Y = (y) => (1 - y) * k, hw = (w) => (w / 2 <= HAIR ? 0.5 : Math.max(0.5, (w / 2) * k));   // a hairline: one fine pixel
  // one shape: its box, and its coverage of the pixel centred at (px, py)
  const paint = (x0, y0, x1, y1, c, cover) => {
    for (let y = Math.max(0, Math.floor(y0 - 1)); y < Math.min(S, Math.ceil(y1 + 1)); y++)
      for (let x = Math.max(0, Math.floor(x0 - 1)); x < Math.min(S, Math.ceil(x1 + 1)); x++) {
        const a = cover(x + 0.5, y + 0.5);
        if (a > 0) for (let q = 0; q < 3; q++) rgb[(y * S + x) * 3 + q] += (c[q] - rgb[(y * S + x) * 3 + q]) * a;
      }
  };
  const seg = (a, b, w, c) => {
    const [ax, ay, bx, by] = [X(a[0]), Y(a[1]), X(b[0]), Y(b[1])], h = hw(w), vx = bx - ax, vy = by - ay, l2 = vx * vx + vy * vy || 1;
    paint(Math.min(ax, bx) - h, Math.min(ay, by) - h, Math.max(ax, bx) + h, Math.max(ay, by) + h, c, (px, py) => {
      const u = Math.max(0, Math.min(1, ((px - ax) * vx + (py - ay) * vy) / l2));
      return edge(Math.hypot(px - ax - u * vx, py - ay - u * vy) - h);
    });
  };
  const disc = (x, y, r, c) => paint(X(x) - r * k, Y(y) - r * k, X(x) + r * k, Y(y) + r * k, c, (px, py) => edge(Math.hypot(px - X(x), py - Y(y)) - r * k));
  const ring = (it, whole) => {
    const r = it.r * k, h = hw(it.w), span = whole ? 360 : (((it.a1 - it.a0) % 360) + 360) % 360, end = (a) => [k + r * Math.cos(a * DEG), k - r * Math.sin(a * DEG)];
    paint(k - r - h, k - r - h, k + r + h, k + r + h, hex(it.color), (px, py) => {
      let d = Math.abs(Math.hypot(px - k, py - k) - r) - h;
      if (!whole && d < 0.5 && ((((Math.atan2(k - py, px - k) / DEG - it.a0) % 360) + 360) % 360) > span) d = Math.min(...[end(it.a0), end(it.a0 + span)].map(([ex, ey]) => Math.hypot(px - ex, py - ey))) - h;
      return edge(d);
    });
  };
  // a 1-bit image (bit(i, j)) in a w x h box centred on (cx, cy) px, turned rot degrees: read bilinear, a smooth edge
  const mask = (bit, mw, mh, cx, cy, w, h, rot, c) => {
    const co = Math.cos(rot * DEG), si = Math.sin(rot * DEG), e = Math.min(0.45, Math.max(0.02, 0.5 * Math.max(mw / w, mh / h))), ex = 0.5 * (Math.abs(co) * w + Math.abs(si) * h), ey = 0.5 * (Math.abs(si) * w + Math.abs(co) * h);
    const b = (i, j) => (i < 0 || j < 0 || i >= mw || j >= mh ? 0 : bit(i, j));
    paint(cx - ex, cy - ey, cx + ex, cy + ey, c, (px, py) => {
      const dx = px - cx, dy = py - cy, u = ((co * dx - si * dy) / w + 0.5) * mw - 0.5, v = ((si * dx + co * dy) / h + 0.5) * mh - 0.5, i = Math.floor(u), j = Math.floor(v), tu = u - i, tv = v - j;
      const top = b(i, j) + (b(i + 1, j) - b(i, j)) * tu, bot = b(i, j + 1) + (b(i + 1, j + 1) - b(i, j + 1)) * tu;
      return sstep(HD_EDGE - e, HD_EDGE + e, top + (bot - top) * tv);
    });
  };
  // the firmware's text runs: letters size * 0.6 apart, each its 5 x 7 glyph in 0.8 x 0.72 of its box
  const run = (str, x, y, size, c, orp = -1) => {
    const f = font(), fs = size * k, adv = fs * 0.6, lh = fs * 1.05, x0 = X(x) - (orp < 0 ? str.length / 2 : orp + 0.5) * adv;
    [...str].forEach((ch, q) => { if (ch !== ' ') mask((i, j) => (f(ch)[j] >> (4 - i)) & 1, 5, 7, x0 + (q + 0.5) * adv, Y(y), adv * 0.8, lh * 0.72, 0, q === orp ? ACCENT : c); });
  };
  for (const it of bind(pkg, layout, params)) {
    if (t < it.on || t >= it.off) continue;
    const c = hex(it.color);
    if (it.k === 'ring' || it.k === 'arc') ring(it, it.k === 'ring' || it.a1 - it.a0 >= 360);
    else if (it.k === 'ticks') for (let i = 0; i < it.n; i++) { const a = (it.a0 + (360 * i) / it.n) * DEG, r1 = it.r + it.len; seg([it.r * Math.cos(a), it.r * Math.sin(a)], [r1 * Math.cos(a), r1 * Math.sin(a)], it.w, c); }
    else if (it.k === 'line' || it.k === 'curve') { const p = it.k === 'curve' ? smooth(it.pts) : it.pts; for (let i = 0; i + 1 < p.length; i++) seg(p[i], p[i + 1], it.w, c); }
    else if (it.k === 'disc') disc(it.x, it.y, it.r, c);
    else if (it.k === 'ball') disc(it.x, it.y, it.r, [0, 0, 0]);                  // its dots go on top (kept)
    else if (it.k === 'glyph' && pkg.assets[it.asset].kind === 'mask') { const a = asset(pkg, it.asset); mask(a.at, a.w, a.h, X(it.x), Y(it.y), it.size * k, ((it.size * a.h) / a.w) * k, it.rot, c); }
    else if (it.k === 'text') run(it.str, it.x, it.y, it.size, c);
  }
  if (caption) {   // scene_player.c's caption(): shrunk until its farthest corner is inside the glass
    const orp = Math.min(ORP(caption.length), caption.length - 1), reach = Math.max(orp + 0.5, caption.length - orp - 0.5);
    let size = 0.16;
    while (size >= 0.04 && (reach * 0.6 * size) ** 2 + (-0.66 - 0.525 * size) ** 2 > 0.95 ** 2) size *= 0.92;
    run(caption, 0, -0.66, size, [255, 255, 255], orp);
  }
  const kept = new Map();
  frame(pkg, layout, params, t, kept);
  return { rgb, kept };
}

export const loadPackage = (p) => JSON.parse(readFileSync(existsSync(p) ? p : join(ROOT, 'packages', p, `${p}.json`), 'utf8'));

if (process.argv[1] === new URL(import.meta.url).pathname) {
  const argv = process.argv.slice(2), opt = (k, d) => { const i = argv.indexOf(k); return i < 0 ? d : argv[i + 1]; };
  const json = (v) => (v === undefined ? {} : JSON.parse(existsSync(v) ? readFileSync(v, 'utf8') : v));
  try {
    if (argv.includes('--commands')) {
      const src = opt('--commands'), dir = opt('--stills'), every = +opt('--every', 1), lines = readFileSync(src === '-' ? 0 : src, 'utf8').split('\n').filter((l) => l.startsWith('{'));
      if (!dir) throw new Error('--commands needs --stills <dir>');
      mkdirSync(dir, { recursive: true });
      const cache = {};
      lines.map((l) => JSON.parse(l)).filter((c) => c.name === 'scene.play' || c.name === 'scene.queue').forEach((c, n) => {
        const { pkg: id, layout, params } = c.args, pkg = (cache[id] ??= loadPackage(id)), dur = pkg.layouts[layout]?.dur || 1;
        for (let t = Math.min(0.5, dur / 2); t < dur; t += every) writeFileSync(join(dir, `${String(n).padStart(2, '0')}-${layout}-${t.toFixed(1)}.png`), png(frame(pkg, layout, params, t)));
        console.log(`${c.name} ${layout}: ${JSON.stringify(c.args).length} B`);
      });
    } else {
      const [id, layout] = argv.filter((a, i) => !a.startsWith('-') && !['--params', '--t', '-o', '--size', '--caption'].includes(argv[i - 1]));
      if (!id || !layout) throw new Error('usage: play.mjs <pkg> <layout> [--params json|file] [--t s] [-o out.png] [--hd [--size px] [--caption word]]');
      const pkg = loadPackage(id), params = json(opt('--params')), t = +opt('--t', 1), out = opt('-o', `${layout}.png`);
      if (argv.includes('--hd')) { const S = +opt('--size', 466), v = hd(pkg, layout, params, t, S, opt('--caption', '')); writeFileSync(out, png(v.kept, v.rgb, S)); }
      else writeFileSync(out, png(frame(pkg, layout, params, t)));
    }
  } catch (e) {
    console.error(e instanceof ParamError ? `bad_params: ${e.message}` : e.message);
    process.exit(1);
  }
}
