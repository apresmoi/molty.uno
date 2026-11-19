// SPDX-License-Identifier: MIT
// Display v2 assets (docs/DISPLAY.md): the plain SVG and PNG files of a skill folder into the masks
// and images of a molty-pkg/1 package, and a package's assets back into PNG files (convert).
// The stroke rasteriser and the palette choice are the ones the astrology package was first built
// with (its build.mjs), so its files compile to the same bytes.
import { existsSync, readFileSync, readdirSync } from 'node:fs';
import { basename, extname, join } from 'node:path';
import { deflateSync, inflateSync } from 'node:zlib';
import { LIMITS, NAME_RE } from '../format.mjs';
import { chunk, smooth } from '../play.mjs';

const b64 = (bytes) => Buffer.from(bytes).toString('base64');
const hex = (c) => '#' + c.slice(0, 3).map((v) => Math.round(Math.max(0, Math.min(255, v))).toString(16).padStart(2, '0')).join('');
const segDist = (px, py, [ax, ay], [bx, by]) => { const dx = bx - ax, dy = by - ay, l = dx * dx + dy * dy, t = l ? Math.max(0, Math.min(1, ((px - ax) * dx + (py - ay) * dy) / l)) : 0; return Math.hypot(px - ax - t * dx, py - ay - t * dy); };

// ---- PNG
/** A PNG as { w, h, rgba, clear, idx, palette }: 8-bit pixels; clear when it carries transparency; idx and
 *  palette ([r, g, b, a] each) when it is an indexed PNG. */
export function readPng(buf) {
  if (buf.length < 8 || buf.readUInt32BE(0) !== 0x89504e47) throw new Error('is not a PNG');
  let w, h, depth, type, lace, plte = null, trns = null;
  const idat = [];
  for (let o = 8; o + 8 <= buf.length;) {
    const len = buf.readUInt32BE(o), name = buf.toString('latin1', o + 4, o + 8), data = buf.subarray(o + 8, o + 8 + len);
    if (name === 'IHDR') [w, h, depth, type, lace] = [data.readUInt32BE(0), data.readUInt32BE(4), data[8], data[9], data[12]];
    else if (name === 'PLTE') plte = data;
    else if (name === 'tRNS') trns = data;
    else if (name === 'IDAT') idat.push(data);
    o += len + 12;
  }
  const ch = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 }[type];
  if (!ch || lace || (depth !== 8 && !(depth < 8 && ch === 1))) throw new Error('must be an 8-bit PNG (or indexed or grey at 1, 2 or 4 bits), not interlaced');
  const bpp = Math.max(1, (ch * depth) >> 3), stride = Math.ceil((w * ch * depth) / 8), raw = inflateSync(Buffer.concat(idat)), px = Buffer.alloc(stride * h);
  for (let y = 0; y < h; y++) {   // undo each row's filter
    const f = raw[y * (stride + 1)], row = raw.subarray(y * (stride + 1) + 1), out = px.subarray(y * stride), up = y ? px.subarray((y - 1) * stride) : null;
    for (let i = 0; i < stride; i++) {
      const a = i >= bpp ? out[i - bpp] : 0, b = up ? up[i] : 0, c = up && i >= bpp ? up[i - bpp] : 0, pa = Math.abs(b - c), pb = Math.abs(a - c), pc = Math.abs(a + b - 2 * c);
      out[i] = (row[i] + [0, a, b, (a + b) >> 1, pa <= pb && pa <= pc ? a : pb <= pc ? b : c][f]) & 255;
    }
  }
  const rgba = new Uint8Array(w * h * 4), idx = type === 3 ? new Uint8Array(w * h) : null, top = (1 << depth) - 1;
  const palette = type === 3 ? Array.from({ length: plte.length / 3 }, (_, i) => [plte[i * 3], plte[i * 3 + 1], plte[i * 3 + 2], trns && i < trns.length ? trns[i] : 255]) : null;
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const o = y * stride + x * bpp, n = y * w + x, v = depth < 8 ? (px[y * stride + ((x * depth) >> 3)] >> (8 - depth - ((x * depth) & 7))) & top : px[o];
    let c;
    if (type === 3) { idx[n] = v; c = palette[v] ?? [0, 0, 0, 0]; }
    else if (type === 0) { const g = Math.round((v * 255) / top); c = [g, g, g, trns && trns.readUInt16BE(0) === v ? 0 : 255]; }
    else if (type === 4) c = [v, v, v, px[o + 1]];
    else c = [v, px[o + 1], px[o + 2], type === 6 ? px[o + 3] : trns && trns[1] === v && trns[3] === px[o + 1] && trns[5] === px[o + 2] ? 0 : 255];
    rgba.set(c, n * 4);
  }
  return { w, h, rgba, clear: type === 4 || type === 6 || !!trns, idx, palette };
}

/** A package asset as a PNG file: a mask white on black at 1 bit, an image indexed at 4 bits (entry 0 see-through). */
export function assetPng(a) {
  const depth = a.kind === 'mask' ? 1 : 4, src = Buffer.from(a.kind === 'mask' ? a.bits : a.px, 'base64'), stride = Math.ceil((a.w * depth) / 8), rows = Buffer.alloc((stride + 1) * a.h);
  for (let y = 0; y < a.h; y++) for (let x = 0; x < a.w; x++) {
    const i = (y * a.w + x) * depth, v = (src[i >> 3] >> (8 - depth - (i & 7))) & ((1 << depth) - 1);
    rows[y * (stride + 1) + 1 + ((x * depth) >> 3)] |= v << (8 - depth - ((x * depth) & 7));
  }
  const head = Buffer.alloc(13);
  head.writeUInt32BE(a.w, 0); head.writeUInt32BE(a.h, 4); head[8] = depth; head[9] = a.kind === 'mask' ? 0 : 3;
  const colours = a.kind === 'mask' ? [] : [chunk('PLTE', Buffer.from(a.palette.flatMap((c) => [1, 3, 5].map((i) => parseInt(c.slice(i, i + 2), 16))))), chunk('tRNS', Buffer.from([0]))];
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', head), ...colours, chunk('IDAT', deflateSync(rows)), chunk('IEND', Buffer.alloc(0))]);
}

// ---- symbols: 1 bit a pixel
/** A PNG's mask: on where it is opaque, or where it is light when it carries no transparency. */
function pngMask({ w, h, rgba, clear }) {
  const bits = new Uint8Array(Math.ceil((w * h) / 8));
  for (let i = 0; i < w * h; i++) if (clear ? rgba[i * 4 + 3] >= 128 : rgba[i * 4] + rgba[i * 4 + 1] + rgba[i * 4 + 2] >= 384) bits[i >> 3] |= 0x80 >> (i & 7);
  return { kind: 'mask', w, h, bits: b64(bits) };
}

const SVG_TAGS = ['svg', 'title', 'desc', 'line', 'polyline', 'polygon'];
/** An SVG's strokes: { box: [x, y, w, h] (its viewBox), width (its stroke-width), lines }. Lines only. */
function readSvg(text) {
  const tags = [...text.replace(/<!--[\s\S]*?-->|<\?[\s\S]*?\?>/g, '').matchAll(/<([A-Za-z][\w:-]*)\b([^>]*)>/g)].map((m) => [m[1], m[2]]);
  const attr = (s, k) => new RegExp(`(?:^|\\s)${k}\\s*=\\s*["']([^"']*)["']`).exec(s)?.[1], nums = (s) => (s ?? '').trim().split(/[\s,]+/).filter(Boolean).map(Number);
  const other = tags.find(([n]) => !SVG_TAGS.includes(n)), root = tags.find(([n]) => n === 'svg')?.[1];
  if (root === undefined) throw new Error('is not an SVG');
  if (other) throw new Error(`uses <${other[0]}>: a symbol's SVG is made of line, polyline and polygon only (or export it as a PNG)`);
  if (tags.some(([, a]) => /\btransform\s*=/.test(a))) throw new Error('uses transform: write the points where they are drawn');
  const box = nums(attr(root, 'viewBox'));
  if (box.length !== 4 || !box.every(Number.isFinite) || box[2] <= 0 || box[3] <= 0) throw new Error('needs a viewBox="x y width height"');
  const pairs = (s) => { const n = nums(s); return Array.from({ length: n.length >> 1 }, (_, i) => [n[i * 2], n[i * 2 + 1]]); };
  const lines = tags.flatMap(([n, a]) => (n === 'line' ? [[[+attr(a, 'x1'), +attr(a, 'y1')], [+attr(a, 'x2'), +attr(a, 'y2')]]] : n === 'polyline' ? [pairs(attr(a, 'points'))] : n === 'polygon' ? [((p) => [...p, p[0]])(pairs(attr(a, 'points')))] : []));
  if (!lines.length || lines.some((l) => l.length < 2 || !l.flat().every(Number.isFinite))) throw new Error('has no line to draw (each needs at least two points)');
  return { box, width: +attr(root, 'stroke-width'), lines };
}
/** Strokes -> a w x h mask: a pixel is on when its centre is within half the width of a line (width in the SVG's units). */
function strokeMask({ box: [x0, y0, bw, bh], lines }, w, h, width) {
  const bits = new Uint8Array(Math.ceil((w * h) / 8));
  for (let j = 0; j < h; j++) for (let i = 0; i < w; i++) {
    const x = x0 + ((i + 0.5) / w) * bw, y = y0 + ((j + 0.5) / h) * bh;
    if (lines.some((l) => l.some((p, k) => k > 0 && segDist(x, y, l[k - 1], p) <= width / 2))) bits[(j * w + i) >> 3] |= 0x80 >> ((j * w + i) & 7);
  }
  return { kind: 'mask', w, h, bits: b64(bits) };
}

// ---- pictures and maps: 4 bits a pixel, 15 colours and see-through
/** rgb pixels -> 15 colours (k-means, deterministic): { pal, near } */
function quantise(px) {
  let pal = Array.from({ length: 15 }, (_, k) => px[Math.floor(((k + 0.5) / 15) * px.length)].slice());
  const near = (p) => { let best = 0, bd = Infinity; pal.forEach((c, k) => { const d = (p[0] - c[0]) ** 2 + (p[1] - c[1]) ** 2 + (p[2] - c[2]) ** 2; if (d < bd) { bd = d; best = k; } }); return best; };
  for (let it = 0; it < 12; it++) {
    const sum = pal.map(() => [0, 0, 0, 0]);
    px.forEach((p) => { const k = near(p); sum[k][0] += p[0]; sum[k][1] += p[1]; sum[k][2] += p[2]; sum[k][3]++; });
    pal = pal.map((c, k) => (sum[k][3] ? sum[k].slice(0, 3).map((v) => v / sum[k][3]) : c));
  }
  return { pal, near };
}
/** A PNG's image: its own palette when it is indexed with entry 0 see-through (what convert writes), else its
 *  colours as they come when 15 hold them, else the 15 that fit it best. */
function pngImage({ w, h, rgba, idx, palette }) {
  const bytes = new Uint8Array(Math.ceil((w * h) / 2)), put = (i, v) => { bytes[i >> 1] |= v << (i & 1 ? 0 : 4); };
  if (idx && palette.length <= 16 && palette[0][3] === 0 && palette.slice(1).every((c) => c[3] === 255)) {
    idx.forEach((v, i) => put(i, v));
    return { kind: 'image', w, h, px: b64(bytes), palette: palette.map(hex) };
  }
  const at = (i) => [rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2]], solid = Array.from({ length: w * h }, (_, i) => i).filter((i) => rgba[i * 4 + 3] >= 128);
  let pal = [...new Set(solid.map((i) => hex(at(i))))], index = (i) => pal.indexOf(hex(at(i)));
  if (pal.length > 15) { const q = quantise(solid.map(at)); pal = q.pal.map(hex); index = (i) => q.near(at(i)); }
  for (const i of solid) put(i, index(i) + 1);
  return { kind: 'image', w, h, px: b64(bytes), palette: ['#000000', ...pal] };
}

// ---- the skill's assets
/** The files a group names: {a,b} lists alternatives in that order, * matches inside a file name (sorted). */
function expand(dir, pattern) {
  const m = /^(.*)\{([^{}]*)\}(.*)$/.exec(pattern);
  if (m) return m[2].split(',').flatMap((alt) => expand(dir, m[1] + alt.trim() + m[3]));
  if (!pattern.includes('*')) return [pattern];
  const cut = pattern.lastIndexOf('/') + 1, folder = pattern.slice(0, cut), re = new RegExp(`^${pattern.slice(cut).replace(/[.+?^$()|[\]\\]/g, '\\$&').replace(/\*/g, '.*')}$`);
  return existsSync(join(dir, folder)) ? readdirSync(join(dir, folder)).filter((n) => re.test(n)).sort().map((n) => folder + n) : [];
}

/**
 * The package's assets from the skill's `assets` groups, in the order written: { assets: { name: asset },
 * files: { name: { file, as } } }. Problems go to bad(group index, message).
 */
export function buildAssets(dir, groups, bad) {
  const assets = {}, files = {};
  groups.forEach((g, n) => {
    const named = [g.files ?? g.file ?? []].flat().flatMap((p) => { const f = expand(dir, p); if (!f.length) bad(n, `${p} matches no file`); return f; });
    for (const file of named) {
      const ext = extname(file).toLowerCase(), name = basename(file, extname(file)), fail = (m) => bad(n, `${file} ${m}`);
      if (!existsSync(join(dir, file))) { fail('does not exist'); continue; }
      if (!NAME_RE.test(name)) { fail(`has a name the device cannot take: use 1 to 24 of a-z, 0-9, - and _ (rename the file)`); continue; }
      if (files[name]) { fail(`has the same name as ${files[name].file}: an asset is named by its file name alone, so rename one`); continue; }
      const [pw, ph] = [g.pixels ?? []].flat();
      let a;
      try {
        if (ext === '.svg') {
          if (g.as !== 'symbol') throw new Error(`is an SVG: a ${g.as} comes from a PNG`);
          const svg = readSvg(readFileSync(join(dir, file), 'utf8')), width = g.stroke !== undefined ? (g.stroke * svg.box[2]) / 2 : svg.width;
          if (!pw) throw new Error('needs pixels (its width on the device): an SVG has no size of its own');
          if (!(width > 0)) throw new Error('needs a line width: stroke-width on its <svg>, or stroke on the group');
          a = strokeMask(g.smooth ? { ...svg, lines: svg.lines.map(smooth) } : svg, pw, ph ?? Math.max(1, Math.round((pw * svg.box[3]) / svg.box[2])), width);
        } else if (ext === '.png') {
          const png = readPng(readFileSync(join(dir, file)));
          if (pw && (pw !== png.w || (ph ?? png.h) !== png.h)) throw new Error(`is ${png.w} x ${png.h} px but the group says pixels ${[g.pixels].flat().join(' x ')}: resize the file (a PNG is used at its own size) or drop pixels`);
          a = g.as === 'symbol' ? pngMask(png) : pngImage(png);
        } else throw new Error('must be an .svg or a .png');
      } catch (e) { fail(e.message); continue; }
      if (a.w > LIMITS.assetSide || a.h > LIMITS.assetSide) { fail(`is ${a.w} x ${a.h} px: the device takes at most ${LIMITS.assetSide} a side`); continue; }
      assets[name] = a;
      files[name] = { file, as: g.as };
    }
  });
  return { assets, files };
}
