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

function stroke(pts, c, put, w = 0) {
  const rgb = hex(c), r = Math.max(0, w / 2);
  for (let i = 0; i + 1 < pts.length; i++) {
    const [a, b] = [pts[i], pts[i + 1]], n = Math.max(1, Math.ceil(Math.hypot(b[0] - a[0], b[1] - a[1]) / 0.006));
    for (let s = 0; s <= n; s++) {
      const x = a[0] + ((b[0] - a[0]) * s) / n, y = a[1] + ((b[1] - a[1]) * s) / n;
      if (r < PITCH[0] / 2) put(...snap(x, y), rgb);
      else each([x - r, x + r, y - r, y + r], (sx, sy, gx, gy) => { if (Math.hypot(gx - x, gy - y) <= r) put(sx, sy, rgb); });
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

/** The dots of `layout` at `t` seconds: Map(dot index -> rgb). Throws ParamError for bad params. */
export function frame(pkg, layout, params, t) {
  const dots = new Map(), put = (sx, sy, c) => { if (sx < 0 || sy < 0 || sx >= W || sy >= H) return; const k = sy * W + sx; if (c === null) dots.delete(k); else dots.set(k, c); };
  for (const it of bind(pkg, layout, params)) {
    if (t < it.on || t >= it.off) continue;
    const rgb = hex(it.color);
    if (it.k === 'ring') stroke(arcPts(it.r, 0, 360), it.color, put, it.w);
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
/** The dots as the panel shows them: round dots on black, inside the round glass. */
export function png(dots) {
  const S = PANEL, raw = Buffer.alloc((S * 3 + 1) * S), R = 1.35;
  const set = (x, y, c) => { if (Math.hypot(x + 0.5 - S / 2, y + 0.5 - S / 2) > S / 2) return; const o = y * (S * 3 + 1) + 1 + x * 3; raw[o] = c[0]; raw[o + 1] = c[1]; raw[o + 2] = c[2]; };
  for (let a = 0; a < 720; a++) set(Math.round(S / 2 + (S / 2 - 1) * Math.cos((a * Math.PI) / 360)), Math.round(S / 2 + (S / 2 - 1) * Math.sin((a * Math.PI) / 360)), [40, 40, 40]);
  for (const [k, c] of dots) { const [px, py] = dotPx(k % W, Math.floor(k / W)); for (let y = Math.floor(py - R); y <= py + R; y++) for (let x = Math.floor(px - R); x <= px + R; x++) if ((x + 0.5 - px) ** 2 + (y + 0.5 - py) ** 2 <= R * R && x >= 0 && y >= 0 && x < S && y < S) set(x, y, c.map(Math.round)); }
  const head = Buffer.alloc(13); head.writeUInt32BE(S, 0); head.writeUInt32BE(S, 4); head[8] = 8; head[9] = 2;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', head), chunk('IDAT', deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
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
      const [id, layout] = argv.filter((a, i) => !a.startsWith('-') && !['--params', '--t', '-o'].includes(argv[i - 1]));
      if (!id || !layout) throw new Error('usage: play.mjs <pkg> <layout> [--params json|file] [--t s] [-o out.png]');
      writeFileSync(opt('-o', `${layout}.png`), png(frame(loadPackage(id), layout, json(opt('--params')), +opt('--t', 1))));
    }
  } catch (e) {
    console.error(e instanceof ParamError ? `bad_params: ${e.message}` : e.message);
    process.exit(1);
  }
}
