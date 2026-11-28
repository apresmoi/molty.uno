// SPDX-License-Identifier: MIT
// molty-pkg/1 (docs/PACKAGES.md): the item table, a package's static check, and binding a
// layout's params into plain items. Shared by tools/scripts/check-packages.mjs and play.mjs;
// the firmware's pkg_parse / scene_player implement the same rules in C.
import { ACTIONS } from '../../device/creatures/js/actions.js';

export const MAX_PKG_BYTES = 256 * 1024;
/** A package id (its file and install id). */
export const ID_RE = /^[a-z0-9-]{1,32}$/;
/** Asset, layout, param and list field names: what the firmware's parser takes. */
export const NAME_RE = /^[A-Za-z0-9_-]{1,24}$/;
export const COLOR_RE = /^#[0-9a-f]{6}$/;
export const PARAM_TYPES = ['number', 'text', 'list'];
/** The firmware's limits (docs/PACKAGES.md "What the firmware accepts"). */
export const LIMITS = { assets: 64, assetSide: 128, layouts: 32, items: 64, params: 8, points: 16, text: 40, say: 160, sayChars: 4096, scene: 128, letters: 40, list: 32, fields: 12, groups: 32, lights: 4 };

// Field value types: num, text, say (a spot's reading, longer text), color, asset (any asset), image (an image asset);
// group (a highlight group's name), lights (group names, spaces between), when ("lit"); pts holds [x, y] numbers.
const COMMON = { k: 'kind', on: 'num', off: 'num', color: 'color', each: 'list', polar: 'polar', group: 'group', when: 'when' };
const POS = { x: 'num', y: 'num' };
export const ITEMS = {
  ring: { ...POS, r: 'num', w: 'num' },
  arc: { ...POS, r: 'num', w: 'num', a0: 'num', a1: 'num' },
  ticks: { r: 'num', len: 'num', n: 'num', a0: 'num', w: 'num' },
  line: { pts: 'points', w: 'num' },
  curve: { pts: 'points', w: 'num' },
  disc: { ...POS, r: 'num' },
  ball: { ...POS, r: 'num', img: 'image', lit: 'num', side: 'num', spin: 'num' },
  glyph: { ...POS, size: 'num', asset: 'asset', rot: 'num' },
  text: { ...POS, size: 'num', str: 'text' },
  // a place to tap, drawn by nothing: Molty shows `say` as the subtitle, the tap event names `id`; a disc, or
  // within r of a segment (pts, two points); a tap lights its `lights` groups, grown `grow` times (default 1.25); also
  // tappable in a ring sector about the glass centre: `arc` degrees wide round the spot's own direction, radii r0 (0)..r1
  // `open`: a tap opens that layout of the package over this one (opens(), below), for `ttl` seconds when it is a held one
  spot: { ...POS, r: 'num', say: 'say', id: 'text', pts: 'points', lights: 'lights', grow: 'num', arc: 'num', r0: 'num', r1: 'num', open: 'layout', ttl: 'num' },
  // the device's own pieces (docs/PACKAGES.md "Beats and wheels"): the beat player's pattern as a clock (r: its outer
  // ring, size: its dots); a wheel round the edge, grabbed from r0 out, turned in clicks of `step` degrees adding `by`
  // (`fast` turning fast) to its value, min..max from `value`, or stepping through its `labels` (a list param, each
  // element's `label`); `sets` the beat player's tempo or pick; on face 0 (playing) or 1 (stopped); each click a tick
  // of the motor, `tick_ms` at `tick` percent (0: the device's own); a centre that plays and stops the beat; the tone
  // player's chord on one circle (r: the circle), in a beat's place, its wheels setting the `pitch` and its `pick`; the
  // sky atlas's cards (r: 1, the glass), in a beat's place, its wheel stepping through the kinds and the objects
  beat: { r: 'num', size: 'num' },
  wave: { r: 'num' },
  atlas: { r: 'num' },
  dial: { r: 'num', r0: 'num', id: 'text', step: 'num', by: 'num', fast: 'num', min: 'num', max: 'num', value: 'num', face: 'num', sets: 'sets', labels: 'labels', tick_ms: 'num', tick: 'num' },
  centre: { r: 'num' },
};
const REQUIRED = { ring: ['r'], arc: ['r'], ticks: ['r'], line: ['pts'], curve: ['pts'], disc: ['r'], ball: ['r'], glyph: ['asset'], text: ['str'], spot: ['r', 'say'], beat: ['r'], dial: ['id'], centre: ['r'], wave: ['r'], atlas: ['r'] };
/** The device's own pieces: in no group, never repeated. */
const OWN = ['beat', 'dial', 'centre', 'wave', 'atlas'];
/** Defaults a missing field takes (the firmware's, docs/PACKAGES.md). A ball without img is shaded in its colour. */
export const DEFAULTS = { color: '#ffffff', x: 0, y: 0, on: 0, a0: 0, a1: 360, n: 12, len: 0.1, size: 0.12, w: 0.03, lit: 1, side: 1, spin: 0, rot: 0, grow: 1.25 };
// which param type a field of each value type may bind to
const BINDS = { num: 'number', text: 'text', say: 'text', color: 'text', asset: 'text', image: 'text', list: 'list', group: 'text', lights: 'text', layout: 'text', sets: 'text', labels: 'list' };

const isNum = (v) => typeof v === 'number' && Number.isFinite(v);
/** The range each numeric field must lie in, literal or bound (the firmware's pkg_num_ok); angles otherwise. */
const RANGE = { x: [-2, 2], y: [-2, 2], r: [0, 2], w: [0, 0.5], len: [-1, 1], n: [1, 120], size: [0, 2], lit: [0, 1], spin: [-720, 720], on: [0, 600], off: [0, 600], grow: [1, 3], arc: [0, 360], r0: [0, 2], r1: [0, 2], ttl: [1, 600],
  step: [0, 360], by: [-1000, 1000], fast: [-1000, 1000], min: [-10000, 10000], max: [-10000, 10000], value: [-10000, 10000], face: [0, 1], tick_ms: [0, 60], tick: [0, 100] };
const inRange = (f, v) => { if (f === 'side') return v === -1 || v === 1; if (f === 'tick_ms') return v === 0 || (v >= 5 && v <= 60); if (f === 'tick') return v === 0 || (v >= 25 && v <= 100); if (f === 'face') return v === 0 || v === 1; if (f === 'n' && !Number.isInteger(v)) return false; if ((f === 'size' || f === 'arc' || f === 'step') && v <= 0) return false; const [lo, hi] = RANGE[f] ?? [-720, 720]; return v >= lo && v <= hi; };
const ref = (v) => (typeof v === 'string' && /^[$@][A-Za-z0-9_-]{1,24}$/.test(v) ? v : null);
const isText = (v, max = LIMITS.text) => typeof v === 'string' && /^[\x20-\x7e]*$/.test(v) && v.length <= max && !/^[$@]/.test(v);

/** A literal's check against a field type; returns an error string or ''. */
function literal(type, v, assets, f, layouts) {
  switch (type) {
    case 'layout': return typeof v === 'string' && (!layouts || layouts[v]) ? '' : `names no layout (${JSON.stringify(v)})`;
    case 'num': return !isNum(v) ? 'must be a number' : f && !inRange(f, v) ? `is out of range (${v})` : '';
    case 'text': return isText(v) ? '' : `must be printable ASCII of at most ${LIMITS.text} characters, not starting with $ or @`;
    case 'say': return isText(v, LIMITS.say) && v ? '' : `must be printable ASCII of 1..${LIMITS.say} characters, not starting with $ or @`;
    case 'color': return typeof v === 'string' && COLOR_RE.test(v) ? '' : 'must be #rrggbb';
    case 'asset': return assets[v] ? '' : `names no asset (${JSON.stringify(v)})`;
    case 'image': return assets[v]?.kind === 'image' ? '' : `names no image asset (${JSON.stringify(v)})`;
    case 'group': return typeof v === 'string' && NAME_RE.test(v) ? '' : 'must be a name [A-Za-z0-9_-]{1,24}';
    case 'lights': { const g = typeof v === 'string' ? v.split(' ').filter(Boolean) : []; return g.length && g.length <= LIMITS.lights && g.every((n) => NAME_RE.test(n)) ? '' : `must be 1..${LIMITS.lights} names, spaces between`; }
    case 'when': return v === 'lit' ? '' : 'must be "lit"';
    case 'sets': return v === 'tempo' || v === 'pick' || v === 'pitch' ? '' : 'must be "tempo", "pick" or "pitch"';
    case 'labels': return Array.isArray(v) && v.length <= LIMITS.list && v.every((e) => isText(e?.label, 24) && e.label) ? '' : 'must be a list param whose elements each have a label (1..24 characters)';
    default: return 'cannot be a literal';
  }
}

const b64len = (n) => Math.ceil(n / 3) * 4;
/** A creature's gestures (docs/CREATURES.md section 1): a shared one (actions.js) keeps its length; any other name is the
 *  creature's own and says how long it plays. The firmware's creature_player.c takes the same. */
export const GESTURE_RE = /^[a-z][a-z0-9_]{0,23}$/;
export const GESTURE_DUR = [0.2, 10];
function gestureProblems(cr) {
  const bad = [];
  for (const [n, g] of Object.entries(cr?.gestures ?? {})) {
    if (!GESTURE_RE.test(n)) bad.push(`gesture ${n}: name must be [a-z][a-z0-9_]{0,23}`);
    else if (n in ACTIONS) { if (g?.dur !== undefined) bad.push(`gesture ${n}: a shared gesture keeps its own length (no dur)`); }
    else if (!(typeof g?.dur === 'number' && g.dur >= GESTURE_DUR[0] && g.dur <= GESTURE_DUR[1])) bad.push(`gesture ${n}: its own gesture needs dur, ${GESTURE_DUR[0]}..${GESTURE_DUR[1]} s`);
  }
  return bad;
}
/** A creature's `about` (docs/CREATURES.md section 1): its look, and one line for every gesture and mood it plays (idle always). */
export const ABOUT_MAX = { look: 240, line: 100 };
function aboutProblems(cr) {
  const ab = cr?.about, bad = [], says = (v, max) => typeof v === 'string' && /^[\x20-\x7e]+$/.test(v) && v.length <= max;
  if (!ab || typeof ab !== 'object') return ['a creature needs about { look, gestures, moods }'];
  for (const k of Object.keys(ab)) if (!['look', 'gestures', 'moods'].includes(k)) bad.push(`about: unknown field ${k}`);
  if (!says(ab.look, ABOUT_MAX.look)) bad.push(`about.look must be printable ASCII of 1..${ABOUT_MAX.look} characters`);
  const want = { gestures: Object.keys(cr.gestures ?? {}), moods: [...new Set(['idle', ...Object.keys(cr.moods ?? {})])] };
  for (const [k, names] of Object.entries(want)) {
    const lines = ab[k] ?? {};
    for (const n of names) if (!(n in lines)) bad.push(`about.${k} has no line for ${n}`);
    for (const [n, v] of Object.entries(lines)) {
      if (!names.includes(n)) bad.push(`about.${k}.${n}: the creature does not play ${n}`);
      else if (!says(v, ABOUT_MAX.line)) bad.push(`about.${k}.${n} must be printable ASCII of 1..${ABOUT_MAX.line} characters`);
    }
  }
  return bad;
}
/** Every problem with a parsed package, as strings (empty = valid). */
export function checkPackage(pkg, id) {
  const bad = [];
  const top = ['v', 'id', 'name', 'version', 'kind', 'assets', 'layouts', 'creature'];
  for (const k of Object.keys(pkg)) if (!top.includes(k)) bad.push(`unknown top-level field ${k}`);
  // molty-pkg/2: a creature whose rig packs its clips (docs/CREATURES.md section 12); firmware before it refuses the file
  if (pkg.v !== 'molty-pkg/1' && !(pkg.v === 'molty-pkg/2' && pkg.creature?.rig)) bad.push('v must be "molty-pkg/1" ("molty-pkg/2" for a creature with a rig)');
  if (!ID_RE.test(pkg.id ?? '') || (id && pkg.id !== id)) bad.push(`id must be ${id ?? '[a-z0-9-]{1,32}'}`);
  if (typeof pkg.name !== 'string' || !pkg.name || pkg.name.length > 32) bad.push('name must be 1..32 characters');
  if (!/^\d+\.\d+\.\d+$/.test(pkg.version ?? '')) bad.push('version must be X.Y.Z');
  if (pkg.kind !== 'scenes' && pkg.kind !== 'creature') bad.push('kind must be scenes or creature');
  if (pkg.kind === 'creature') bad.push(...aboutProblems(pkg.creature), ...gestureProblems(pkg.creature));
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
    for (const k of Object.keys(l)) if (!['dur', 'params', 'items', 'caption_y'].includes(k)) bad.push(`${at}: unknown field ${k}`);
    if (!isNum(l.dur) || l.dur < 0) bad.push(`${at}: dur must be a number >= 0`);
    if (l.caption_y !== undefined && !(isNum(l.caption_y) && l.caption_y >= -1 && l.caption_y <= 1)) bad.push(`${at}: caption_y must be a number in -1..1`);
    const params = l.params ?? {}, used = new Set();
    if (Object.keys(params).length > LIMITS.params) bad.push(`${at}: at most ${LIMITS.params} params`);
    for (const [p, t] of Object.entries(params)) {
      if (!NAME_RE.test(p)) bad.push(`${at}: param name ${p} must be [A-Za-z0-9_-]{1,24}`);
      if (!PARAM_TYPES.includes(t)) bad.push(`${at}: param ${p} type must be one of ${PARAM_TYPES.join(', ')}`);
    }
    if (!Array.isArray(l.items) || !l.items.length || l.items.length > LIMITS.items) { bad.push(`${at}: items must be 1..${LIMITS.items}`); continue; }
    l.items.forEach((it, i) => {
      const where = `${at} item ${i}`, fields = { ...COMMON, ...ITEMS[it.k] };
      if (it.k === 'spot' || OWN.includes(it.k)) { delete fields.group; delete fields.when; }   // a spot lights groups, it is in none
      if (!ITEMS[it.k]) return bad.push(`${where}: unknown k ${JSON.stringify(it.k)}`);
      const value = (f, type, v, rf = f) => {
        const r = ref(v);
        if (!r) { const e = literal(type, v, assets, rf, pkg.layouts); if (e) bad.push(`${where}: ${f} ${e}`); return; }
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
        if (f === 'when') { if (v !== 'lit') bad.push(`${where}: when must be "lit"`); continue; }
        if (f === 'pts') { if (!Array.isArray(v) || v.length < 2 || v.length > (it.k === 'spot' ? 2 : LIMITS.points) || !v.every((p) => Array.isArray(p) && p.length === 2)) bad.push(`${where}: pts must be 2..${it.k === 'spot' ? 2 : LIMITS.points} [x, y] points`); else v.flat().forEach((e, i) => value(f, 'num', e, i & 1 ? 'y' : 'x')); continue; }
        if (f === 'polar') { if (!Array.isArray(v) || v.length !== 2) bad.push(`${where}: polar must be [angle, radius]`); else v.forEach((e, i) => value(f, 'num', e, i ? 'r' : 'a0')); continue; }
        value(f, type, v);
      }
      if ('polar' in it && ('x' in it || 'y' in it)) bad.push(`${where}: polar replaces x, y: give one or the other`);
      for (const f of REQUIRED[it.k]) if (!(f in it)) bad.push(`${where}: ${it.k} needs ${f}`);
      if (it.k === 'spot' && (('arc' in it) !== ('r1' in it) || ('r0' in it && !('arc' in it)))) bad.push(`${where}: a sector is arc and r1 (and r0)`);
      if (OWN.includes(it.k) && 'each' in it) bad.push(`${where}: a ${it.k} is not repeated`);
      if ('ttl' in it && !('open' in it)) bad.push(`${where}: ttl is for open`);
      // a layout a spot opens takes its params from this one's by name: those count as read
      if (pkg.layouts?.[it.open]) for (const p of Object.keys(pkg.layouts[it.open].params ?? {})) used.add(p);
    });
    const own = (k, f = () => true) => l.items.filter((it) => it.k === k && f(it)).length;
    if (own('beat') + own('wave') + own('atlas') > 1 || own('centre') > 1 || own('dial', (it) => !it.face) > 1 || own('dial', (it) => it.face === 1) > 1) bad.push(`${at}: one beat, one centre and one dial a face`);
    if (own('centre') > own('beat') + own('wave') + own('atlas')) bad.push(`${at}: a centre plays and stops a beat: the layout has none`);
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
    // text values reach a spot's reading; a text item still takes LIMITS.text (its field's check)
    if (t === 'text' && !isText(v, LIMITS.say)) throw new ParamError(`${p} must be printable ASCII of at most ${LIMITS.say} characters`);
    if (t === 'list' && (!Array.isArray(v) || v.length > LIMITS.list)) throw new ParamError(`${p} must be a list of at most ${LIMITS.list}`);
    // each element a flat object of a few numbers or texts
    if (t === 'list' && !v.every((e) => e && typeof e === 'object' && !Array.isArray(e) && Object.keys(e).length <= LIMITS.fields && Object.entries(e).every(([k, x]) => NAME_RE.test(k) && (isNum(x) || isText(x, LIMITS.say))))) throw new ParamError(`${p} elements must be flat objects of at most ${LIMITS.fields} numbers or texts`);
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
        const e = literal(type, got, pkg.assets, rf, pkg.layouts);
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
  const said = out.filter((it) => it.k === 'spot').reduce((n, it) => n + it.say.length + (it.id?.length ?? 0), 0);
  if (said > LIMITS.sayChars) throw new ParamError(`the scene's spots hold ${said} characters (at most ${LIMITS.sayChars})`);
  const groups = new Set(out.flatMap((it) => [it.group, ...(it.lights ?? '').split(' ')]).filter(Boolean));
  if (groups.size > LIMITS.groups) throw new ParamError(`the scene names ${groups.size} groups (at most ${LIMITS.groups})`);
  return out;
}

/**
 * What a tap on spot `id` of `layout` (played with `params`) opens, as the firmware's scene_open_build: null, or
 * { layout, params, ttl } to play over it. The opened layout's params come by name: each from the tapped spot's list
 * element if it has such a field, else from this layout's own params; a list keeps only the elements whose `of` (if they
 * have one) is the spot's id. ttl: the spot's (a held layout stays that long; undefined: until a tap beside it).
 */
export function opens(pkg, layoutName, params, id) {
  for (const it of pkg.layouts?.[layoutName]?.items ?? []) {
    if (it.k !== 'spot' || it.open === undefined) continue;
    for (const row of it.each ? params[it.each.slice(1)] : [null]) {
      const val = (v) => { const r = ref(v); return !r ? v : r[0] === '$' ? params[r.slice(1)] : row?.[r.slice(1)]; };
      if (val(it.id) !== id) continue;
      const name = val(it.open), child = pkg.layouts[name], out = {};
      if (!child) throw new ParamError(`no_layout ${name}`);
      for (const p of Object.keys(child.params ?? {})) {
        const v = row && row[p] !== undefined && typeof row[p] !== 'object' ? row[p] : params[p];
        if (v === undefined) throw new ParamError(`missing param ${p}`);
        out[p] = Array.isArray(v) ? v.filter((e) => typeof e?.of !== 'string' || e.of === id) : v;
      }
      return { layout: name, params: out, ttl: val(it.ttl) };
    }
  }
  return null;
}
