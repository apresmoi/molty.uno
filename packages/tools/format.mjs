// SPDX-License-Identifier: MIT
// molty-pkg/1 (docs/PACKAGES.md): the item table, a package's static check, and binding a
// layout's params into plain items. Shared by tools/scripts/check-packages.mjs and play.mjs;
// the firmware's pkg_parse / scene_player implement the same rules in C.

export const MAX_PKG_BYTES = 256 * 1024;
/** A package id (its file and install id). */
export const ID_RE = /^[a-z0-9-]{1,32}$/;
/** Asset, layout, param and list field names: what the firmware's parser takes. */
export const NAME_RE = /^[A-Za-z0-9_-]{1,24}$/;
export const COLOR_RE = /^#[0-9a-f]{6}$/;
export const PARAM_TYPES = ['number', 'text', 'list'];
/** The firmware's limits (docs/PACKAGES.md "What the firmware accepts"). */
export const LIMITS = { assets: 64, assetSide: 128, layouts: 32, items: 64, params: 8, points: 16, text: 40, scene: 128, letters: 40, list: 32, fields: 8 };

// Field value types: num, text, color, asset (any asset), image (an image asset); pts holds [x, y] numbers.
const COMMON = { k: 'kind', on: 'num', off: 'num', color: 'color', each: 'list', polar: 'polar' };
const POS = { x: 'num', y: 'num' };
export const ITEMS = {
  ring: { r: 'num', w: 'num' },
  arc: { r: 'num', w: 'num', a0: 'num', a1: 'num' },
  ticks: { r: 'num', len: 'num', n: 'num', a0: 'num', w: 'num' },
  line: { pts: 'points', w: 'num' },
  curve: { pts: 'points', w: 'num' },
  disc: { ...POS, r: 'num' },
  ball: { ...POS, r: 'num', img: 'image', lit: 'num', side: 'num', spin: 'num' },
  glyph: { ...POS, size: 'num', asset: 'asset', rot: 'num' },
  text: { ...POS, size: 'num', str: 'text' },
};
const REQUIRED = { ring: ['r'], arc: ['r'], ticks: ['r'], line: ['pts'], curve: ['pts'], disc: ['r'], ball: ['r'], glyph: ['asset'], text: ['str'] };
/** Defaults a missing field takes (the firmware's, docs/PACKAGES.md). A ball without img is shaded in its colour. */
export const DEFAULTS = { color: '#ffffff', x: 0, y: 0, on: 0, a0: 0, a1: 360, n: 12, len: 0.1, size: 0.12, w: 0.03, lit: 1, side: 1, spin: 0, rot: 0 };
// which param type a field of each value type may bind to
const BINDS = { num: 'number', text: 'text', color: 'text', asset: 'text', image: 'text', list: 'list' };

const isNum = (v) => typeof v === 'number' && Number.isFinite(v);
/** The range each numeric field must lie in, literal or bound (the firmware's pkg_num_ok); angles otherwise. */
const RANGE = { x: [-2, 2], y: [-2, 2], r: [0, 2], w: [0, 0.5], len: [-1, 1], n: [1, 120], size: [0, 2], lit: [0, 1], spin: [-720, 720], on: [0, 600], off: [0, 600] };
const inRange = (f, v) => { if (f === 'side') return v === -1 || v === 1; if (f === 'n' && !Number.isInteger(v)) return false; if (f === 'size' && v <= 0) return false; const [lo, hi] = RANGE[f] ?? [-720, 720]; return v >= lo && v <= hi; };
const ref = (v) => (typeof v === 'string' && /^[$@][A-Za-z0-9_-]{1,24}$/.test(v) ? v : null);
const isText = (v) => typeof v === 'string' && /^[\x20-\x7e]*$/.test(v) && v.length <= LIMITS.text && !/^[$@]/.test(v);

/** A literal's check against a field type; returns an error string or ''. */
function literal(type, v, assets, f) {
  switch (type) {
    case 'num': return !isNum(v) ? 'must be a number' : f && !inRange(f, v) ? `is out of range (${v})` : '';
    case 'text': return isText(v) ? '' : `must be printable ASCII of at most ${LIMITS.text} characters, not starting with $ or @`;
    case 'color': return typeof v === 'string' && COLOR_RE.test(v) ? '' : 'must be #rrggbb';
    case 'asset': return assets[v] ? '' : `names no asset (${JSON.stringify(v)})`;
    case 'image': return assets[v]?.kind === 'image' ? '' : `names no image asset (${JSON.stringify(v)})`;
    default: return 'cannot be a literal';
  }
}

const b64len = (n) => Math.ceil(n / 3) * 4;
/** Every problem with a parsed package, as strings (empty = valid). */
export function checkPackage(pkg, id) {
  const bad = [];
  const top = ['v', 'id', 'name', 'version', 'kind', 'assets', 'layouts', 'creature'];
  for (const k of Object.keys(pkg)) if (!top.includes(k)) bad.push(`unknown top-level field ${k}`);
  if (pkg.v !== 'molty-pkg/1') bad.push('v must be "molty-pkg/1"');
  if (!ID_RE.test(pkg.id ?? '') || (id && pkg.id !== id)) bad.push(`id must be ${id ?? '[a-z0-9-]{1,32}'}`);
  if (typeof pkg.name !== 'string' || !pkg.name || pkg.name.length > 32) bad.push('name must be 1..32 characters');
  if (!/^\d+\.\d+\.\d+$/.test(pkg.version ?? '')) bad.push('version must be X.Y.Z');
  if (pkg.kind !== 'scenes' && pkg.kind !== 'creature') bad.push('kind must be scenes or creature');
  const assets = pkg.assets ?? {};
  if (Object.keys(assets).length > LIMITS.assets) bad.push(`at most ${LIMITS.assets} assets`);
  if (Object.keys(pkg.layouts ?? {}).length > LIMITS.layouts) bad.push(`at most ${LIMITS.layouts} layouts`);
  for (const [name, a] of Object.entries(assets)) {
    const at = `asset ${name}`;
    if (!NAME_RE.test(name)) bad.push(`${at}: name must be [A-Za-z0-9_-]{1,24}`);
    if (![a.w, a.h].every((v) => Number.isInteger(v) && v >= 1 && v <= LIMITS.assetSide)) { bad.push(`${at}: w, h must be 1..${LIMITS.assetSide}`); continue; }
    if (a.kind === 'mask') {
      if (Object.keys(a).sort().join() !== 'bits,h,kind,w') bad.push(`${at}: a mask has exactly kind, w, h, bits`);
      if (typeof a.bits !== 'string' || a.bits.length !== b64len(Math.ceil((a.w * a.h) / 8))) bad.push(`${at}: bits must be base64 of ceil(w*h/8) bytes`);
    } else if (a.kind === 'image') {
      if (Object.keys(a).sort().join() !== 'h,kind,palette,px,w') bad.push(`${at}: an image has exactly kind, w, h, px, palette`);
      if (!Array.isArray(a.palette) || a.palette.length < 1 || a.palette.length > 16 || !a.palette.every((c) => COLOR_RE.test(c))) bad.push(`${at}: palette must be 1..16 #rrggbb`);
      else if (typeof a.px !== 'string' || a.px.length !== b64len(Math.ceil((a.w * a.h) / 2))) bad.push(`${at}: px must be base64 of ceil(w*h/2) bytes`);
      else { const px = Buffer.from(a.px, 'base64'); for (let i = 0; i < a.w * a.h; i++) if (((px[i >> 1] >> (i & 1 ? 0 : 4)) & 15) >= a.palette.length) { bad.push(`${at}: pixel ${i} indexes past the palette`); break; } }
    } else bad.push(`${at}: kind must be mask or image`);
  }
  if (pkg.kind === 'scenes' && (!pkg.layouts || !Object.keys(pkg.layouts).length)) bad.push('a scenes package needs layouts');
  for (const [name, l] of Object.entries(pkg.layouts ?? {})) {
    const at = `layout ${name}`;
    if (!NAME_RE.test(name)) bad.push(`${at}: name must be [A-Za-z0-9_-]{1,24}`);
    for (const k of Object.keys(l)) if (!['dur', 'params', 'items'].includes(k)) bad.push(`${at}: unknown field ${k}`);
    if (!isNum(l.dur) || l.dur < 0) bad.push(`${at}: dur must be a number >= 0`);
    const params = l.params ?? {}, used = new Set();
    if (Object.keys(params).length > LIMITS.params) bad.push(`${at}: at most ${LIMITS.params} params`);
    for (const [p, t] of Object.entries(params)) {
      if (!NAME_RE.test(p)) bad.push(`${at}: param name ${p} must be [A-Za-z0-9_-]{1,24}`);
      if (!PARAM_TYPES.includes(t)) bad.push(`${at}: param ${p} type must be one of ${PARAM_TYPES.join(', ')}`);
    }
    if (!Array.isArray(l.items) || !l.items.length || l.items.length > LIMITS.items) { bad.push(`${at}: items must be 1..${LIMITS.items}`); continue; }
    l.items.forEach((it, i) => {
      const where = `${at} item ${i}`, fields = { ...COMMON, ...ITEMS[it.k] };
      if (!ITEMS[it.k]) return bad.push(`${where}: unknown k ${JSON.stringify(it.k)}`);
      const value = (f, type, v, rf = f) => {
        const r = ref(v);
        if (!r) { const e = literal(type, v, assets, rf); if (e) bad.push(`${where}: ${f} ${e}`); return; }
        if (r[0] === '$') {
          const p = r.slice(1); used.add(p);
          if (!params[p]) bad.push(`${where}: ${f} reads $${p}, which is not a param`);
          else if (params[p] !== BINDS[type]) bad.push(`${where}: ${f} needs a ${BINDS[type]} param, $${p} is ${params[p]}`);
        } else if (it.each === undefined) bad.push(`${where}: ${f} reads ${r} outside an each`);
      };
      for (const [f, v] of Object.entries(it)) {
        const type = fields[f];
        if (!type) { bad.push(`${where}: unknown field ${f} for ${it.k}`); continue; }
        if (f === 'k') continue;
        if (f === 'each') { if (!ref(v)?.startsWith('$')) bad.push(`${where}: each must be "$<list param>"`); else value(f, 'list', v); continue; }
        if (f === 'pts') { if (!Array.isArray(v) || v.length < 2 || v.length > LIMITS.points || !v.every((p) => Array.isArray(p) && p.length === 2)) bad.push(`${where}: pts must be 2..${LIMITS.points} [x, y] points`); else v.flat().forEach((e, i) => value(f, 'num', e, i & 1 ? 'y' : 'x')); continue; }
        if (f === 'polar') { if (!Array.isArray(v) || v.length !== 2) bad.push(`${where}: polar must be [angle, radius]`); else v.forEach((e, i) => value(f, 'num', e, i ? 'r' : 'a0')); continue; }
        value(f, type, v);
      }
      if ('polar' in it && ('x' in it || 'y' in it)) bad.push(`${where}: polar replaces x, y: give one or the other`);
      for (const f of REQUIRED[it.k]) if (!(f in it)) bad.push(`${where}: ${it.k} needs ${f}`);
    });
    for (const p of Object.keys(params)) if (!used.has(p)) bad.push(`${at}: param ${p} is never read`);
  }
  return bad;
}

export class ParamError extends Error {}

/**
 * The items of `layout` with `params` bound, as plain values: what a player draws (`each` expanded,
 * polar turned into x, y, defaults filled). Throws ParamError (the device's bad_params).
 */
export function bind(pkg, layoutName, params = {}) {
  const l = pkg.layouts?.[layoutName];
  if (!l) throw new ParamError(`no_layout ${layoutName}`);
  const types = l.params ?? {};
  for (const p of Object.keys(params)) if (!types[p]) throw new ParamError(`unknown param ${p}`);
  for (const [p, t] of Object.entries(types)) {
    const v = params[p];
    if (v === undefined) throw new ParamError(`missing param ${p}`);
    if (t === 'number' && !isNum(v)) throw new ParamError(`${p} must be a number`);
    if (t === 'text' && !isText(v)) throw new ParamError(`${p} must be printable ASCII of at most ${LIMITS.text} characters`);
    if (t === 'list' && (!Array.isArray(v) || v.length > LIMITS.list)) throw new ParamError(`${p} must be a list of at most ${LIMITS.list}`);
    // each element a flat object of a few numbers or texts
    if (t === 'list' && !v.every((e) => e && typeof e === 'object' && !Array.isArray(e) && Object.keys(e).length <= LIMITS.fields && Object.entries(e).every(([k, x]) => NAME_RE.test(k) && (isNum(x) || isText(x))))) throw new ParamError(`${p} elements must be flat objects of at most ${LIMITS.fields} numbers or texts`);
  }
  const out = [];
  for (const it of l.items) {
    const fields = { ...COMMON, ...ITEMS[it.k] };
    const rows = it.each ? params[it.each.slice(1)] : [null];
    for (const row of rows) {
      if (it.each && (row === null || typeof row !== 'object' || Array.isArray(row))) throw new ParamError(`${it.each} elements must be objects`);
      const get = (f, type, v, rf = f) => {
        const r = ref(v);
        if (!r) return v;
        const got = r[0] === '$' ? params[r.slice(1)] : row[r.slice(1)];
        if (got === undefined) throw new ParamError(`${f}: ${r} is missing`);
        const e = literal(type, got, pkg.assets, rf);
        if (e) throw new ParamError(`${f} (${r}) ${e}`);
        return got;
      };
      const o = { k: it.k };
      for (const [f, v] of Object.entries(it)) {
        if (f === 'k' || f === 'each') continue;
        if (f === 'pts') { o.pts = v.map((pt) => pt.map((e, i) => get(f, 'num', e, i ? 'y' : 'x'))); continue; }
        if (f === 'polar') { const [a, r] = v.map((e, i) => get(f, 'num', e, i ? 'r' : 'a0')); o.x = r * Math.cos((a * Math.PI) / 180); o.y = r * Math.sin((a * Math.PI) / 180); continue; }
        o[f] = get(f, fields[f], v);
      }
      out.push({ ...DEFAULTS, off: l.dur > 0 ? l.dur : Infinity, ...o });
    }
  }
  if (out.length > LIMITS.scene) throw new ParamError(`the scene expands to ${out.length} items (at most ${LIMITS.scene})`);
  const letters = out.filter((it) => it.k === 'text').reduce((n, it) => n + it.str.length, 0);
  if (letters > LIMITS.letters) throw new ParamError(`the scene has ${letters} letters of text (at most ${LIMITS.letters})`);
  return out;
}
