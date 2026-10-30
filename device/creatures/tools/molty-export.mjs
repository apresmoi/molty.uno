#!/usr/bin/env node
// molty-export.mjs — convert cute-kit creatures (js/cute/*.js) to .molty files for the
// device runtime (device/firmware/main/creature/cute_*.c). Spec: device/creatures/MOLTY-FORMAT.md
//
//   node device/creatures/tools/molty-export.mjs <id|all> [outdir] [--json]
//     id      cute-duck | duck | all
//     outdir  default: device/creatures/molty/
//     --json  also write <name>.molty.json (the resolved spec, for review)
//
// The exporter imports the REAL kit specs (three.js is stubbed: no geometry is built
// here) and writes the creature fully RESOLVED: kit defaults applied, sockets placed,
// neck/jaw points computed — so the device needs no default logic. It never writes
// meshes: the device generates every part from its primitive parameters.
// Per-creature JS hooks (build/update) cannot ship; their signature behaviours are
// re-expressed declaratively in molty-behaviours.mjs (see MOLTY-FORMAT.md §Behaviours).

import { register } from 'node:module';
import { writeFileSync, mkdirSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));

// ---- stub 'three' (kit.js only needs Vector3 at module level) ----
const STUB = 'export class Vector3 { constructor(x = 0, y = 0, z = 0) { this.x = x; this.y = y; this.z = z; } }\nexport class Color {}\n';
const HOOKS = `export async function resolve(spec, ctx, next) {
  if (spec === 'three') return { url: 'data:text/javascript,' + encodeURIComponent(${JSON.stringify(STUB)}), shortCircuit: true };
  return next(spec, ctx);
}`;
register('data:text/javascript,' + encodeURIComponent(HOOKS));

const kit = await import(resolve(HERE, '../js/cute/kit.js'));
await import(resolve(HERE, '../js/cute/index.js'));
const { BEHAVIOURS } = await import(resolve(HERE, './molty-behaviours.mjs'));
const { REGISTRY, DEFAULT_PALETTE, SOCKETS, HEAD_ITEMS, NECK_ITEMS } = kit;

// ---- format constants (keep in sync with MOLTY-FORMAT.md and cute_spec.h) ----
export const MAGIC = 'MLTY', VERSION = 1;
export const Q = 4096;                          // int16 fixed point: value = q / 4096 (±8)
export const Q8 = 256;                          // rates / periods: value = q / 256 (±128)
export const MOODS = ['idle', 'listening', 'thinking', 'speaking', 'happy', 'sleepy', 'surprised'];
export const PTYPE = { ellipsoid: 0, sphere: 0, teardrop: 1, capsule: 2, cone: 3, cylinder: 4, torus: 5, box: 6, disc: 7 };
export const MOTION = { none: 0, waddle: 1, hop: 2, slither: 3, float: 4, flutter: 5, wiggle: 6 };
export const EYESHAPE = { round: 0, tall: 1, wide: 2 };
export const MOUTH = { smile: 0, cat: 1, jaw: 2, none: 3 };
export const ON = { root: 0, body: 1, head: 2 };
export const ROT_ORDER = { XYZ: 0, YXZ: 1, ZXY: 2, XZY: 3, YZX: 4, ZYX: 5 };
export const BKIND = { part: 0, socket: 1, root: 2, head: 3, eyes: 4, body: 5 };
export const BCHAN = { rx: 0, ry: 1, rz: 2, px: 3, py: 4, pz: 5, s: 6, sx: 7, sy: 8, sz: 9, sxz: 10, lid: 11 };
export const BOP = { add: 0, set: 1 };
export const BDRV = { const: 0, sin: 1, abssin: 2, possin: 3, square: 4, mouth: 5, lagx: 6, lagz: 7, pulse: 8, spin: 9 };
export const BFLAG = { sideSign: 1, sidePhase: 2, ease: 4, gainB: 8 };
const ALIAS = { wing: 'side', arm: 'side', fin: 'side', leg: 'foot' };
const PAIRED = new Set(['ear', 'side', 'foot', 'antenna', 'horn']);

// ---- kit math, ported (resolution only) ----
const clamp = (v, a, b) => (v < a ? a : v > b ? b : v);
const size3 = (s) => (Array.isArray(s) ? [s[0], s[1] ?? s[0], s[2] ?? s[0]] : [s, s, s]);
const autoSeg = (r) => { const w = clamp(Math.round(4 + r * 18), 5, 10); return [w, clamp(Math.round(w * 0.7), 3, 7)]; };
function surfPoint(faceR, faceOff, u, v, lift) {
  const [rx, ry, rz] = faceR;
  let d = u * u + v * v; if (d > 0.96) { const k = Math.sqrt(0.96 / d); u *= k; v *= k; d = 0.96; }
  const x = u * rx, y = v * ry, z = rz * Math.sqrt(1 - d);
  const n = [x / (rx * rx), y / (ry * ry), z / (rz * rz)], nl = Math.hypot(...n);
  return [x + n[0] / nl * lift + faceOff[0], y + n[1] / nl * lift + faceOff[1], z + n[2] / nl * lift + faceOff[2]];
}

/** Resolve a kit spec into the fully-explicit structure the device reads. */
export function resolveSpec(S) {
  const warn = [];
  const pal = { ...DEFAULT_PALETTE, ...(S.palette || {}) };
  const palette = Object.entries(pal).map(([k, hex]) => ({ key: k, hex }));
  const colorIdx = (c) => {
    const key = c || 'body';
    let i = palette.findIndex((p) => p.key === key);
    if (i >= 0) return i;
    if (key.startsWith('#')) { palette.push({ key, hex: key }); return palette.length - 1; }
    warn.push(`unknown colour key "${key}" → magenta`); palette.push({ key, hex: '#ff00ff' }); return palette.length - 1;
  };
  const hr = size3(S.head.size), H = S.head.at || [0, 0.62, 0];
  const br = S.body ? size3(S.body.size) : null, B = S.body ? (S.body.at || [0, br[1], 0]) : null;
  const neck = S.head.pivot || (B ? [0, B[1] + br[1] * 0.7, (H[2] + B[2]) / 2] : [0, 0, H[2]]);
  const faceR = S.face?.r ? size3(S.face.r) : hr, faceOff = S.face?.offset || [0, 0, 0];
  const M = { type: 'smile', y: -0.32, w: 0.05, open: 0.5, ...(S.mouth || {}) };
  const E = { x: 0.4, y: -0.05, r: 0.075, shape: 'round', shine: true, color: 'eye', tilt: 0, ...(S.eyes || {}) };
  const C = S.cheeks === false ? null : { x: 0.62, y: -0.28, r: 0.05, ...(S.cheeks || {}) };
  const mo = typeof S.motion === 'object' ? { amp: 1, rate: 1, ...S.motion } : { type: S.motion || 'none', amp: 1, rate: 1 };
  const limbs = { side: 1, ear: 1, tail: 1, foot: 1, antenna: 1, ...(S.limbs || {}) };

  // sockets (kit _build def table + overrides), creature space, rest pose, +x side
  const def = {
    root: { on: 'root', pos: [0, 0, 0] }, body: { on: 'body', pos: [0, 0, 0] }, head: { on: 'head', pos: H },
    ear: { on: 'head', pos: [hr[0] * 0.55, H[1] + hr[1] * 0.72, H[2] - hr[2] * 0.1] },
    antenna: { on: 'head', pos: [hr[0] * 0.3, H[1] + hr[1] * 0.92, H[2]] },
    horn: { on: 'head', pos: [hr[0] * 0.45, H[1] + hr[1] * 0.8, H[2] + hr[2] * 0.1] },
    top: { on: 'head', pos: [0, H[1] + hr[1], H[2]] },
    side: B ? { on: 'body', pos: [br[0] * 0.92, B[1] + br[1] * 0.12, B[2]] } : { on: 'head', pos: [hr[0] * 0.92, H[1] - hr[1] * 0.35, H[2]] },
    foot: { on: 'root', pos: [(B ? br[0] : hr[0]) * 0.42, 0, (B ? B[2] + br[2] * 0.35 : H[2] + hr[2] * 0.3)] },
    tail: B ? { on: 'body', pos: [0, B[1] + br[1] * 0.2, B[2] - br[2] * 0.92] } : { on: 'head', pos: [0, H[1] - hr[1] * 0.4, H[2] - hr[2] * 0.95] },
    neck: { on: B ? 'body' : 'head', pos: [0, B ? neck[1] : H[1] - hr[1] * 0.5, B ? neck[2] : H[2]] },
    back: B ? { on: 'body', pos: [0, B[1] + br[1] * 0.65, B[2] - br[2] * 0.45] } : { on: 'head', pos: [0, H[1] + hr[1] * 0.4, H[2] - hr[2] * 0.7] },
  };
  { const p = surfPoint(faceR, faceOff, 0, M.y, 0); def.jaw = { on: 'head', pos: [p[0] + H[0], p[1] + H[1], p[2] + H[2]] }; }
  const sockets = SOCKETS.map((name) => {
    const d = def[name], o = S.sockets?.[name];
    const cfg = { rot: [0, 0, 0], ...d, ...(Array.isArray(o) ? { pos: o } : o || {}) };
    return { name, on: cfg.on, pos: cfg.pos, rot: cfg.rot || [0, 0, 0], paired: PAIRED.has(name) };
  });

  // neck ring (items): horizontal cross-section of body ∪ head at the neck socket height
  const ringAt = (y) => {
    const cs = (Cc, r) => { const dd = 1 - ((y - Cc[1]) / r[1]) ** 2; return dd > 0 ? [r[0] * Math.sqrt(dd), r[2] * Math.sqrt(dd)] : [0, 0]; };
    const a = B ? cs(B, br) : [0, 0], b = cs(H, hr); return [Math.max(a[0], b[0]), Math.max(a[1], b[1])];
  };
  const nk = sockets.find((s) => s.name === 'neck');
  const [ex0, ez0] = ringAt(nk.pos[1]);

  // parts: body + head + spec parts (as kit _build)
  const names = [];
  const nameIdx = (n) => { let i = names.indexOf(n); if (i < 0) { names.push(n); i = names.length - 1; } return i; };
  const all = [];
  if (B) all.push({ name: 'body', type: 'ellipsoid', color: 'body', ...S.body, size: br, at: 'body', pos: B });
  all.push({ name: 'head', type: 'ellipsoid', color: 'body', ...S.head, size: hr, at: 'head', pos: [0, 0, 0] });
  all.push(...(S.parts || []));
  let approx = 0, ui = 0;
  const parts = [];
  for (const p0 of all) {
    let p = p0, type = p.type || 'ellipsoid';
    if (type === 'custom') {
      // custom geometry cannot ship; approximate from the declared size (rough shape)
      approx++;
      if (p.size) type = p.pinch != null ? 'teardrop' : 'ellipsoid';
      else if (p.r != null && p.h != null) type = 'cylinder';
      else { warn.push(`part "${p.name}" is custom geometry with no size — skipped`); continue; }
      warn.push(`part "${p.name}" is custom geometry — approximated as ${type}`);
    }
    if (!(type in PTYPE)) { warn.push(`part type ${type} unsupported — skipped`); continue; }
    const at = ALIAS[p.at] || p.at || 'body';
    const seg = Array.isArray(p.seg) ? p.seg : null;
    let segA = 0, segB = 0, params = [];
    switch (type) {
      case 'ellipsoid': case 'sphere': { const s = size3(p.size ?? 0.1); const [w, h] = seg || autoSeg(Math.max(...s)); segA = w; segB = h; params = s; break; }
      case 'teardrop': { const s = size3(p.size ?? 0.1); const [w, h] = seg || autoSeg(Math.max(...s)); segA = w; segB = h; params = [...s, p.pinch ?? 0.85]; break; }
      case 'capsule': segA = typeof p.seg === 'number' ? p.seg : 7; segB = p.capSeg ?? 2; params = [p.r ?? 0.05, p.len ?? 0.1]; break;
      case 'cone': segA = typeof p.seg === 'number' ? p.seg : 7; segB = 1; params = [p.r ?? 0.05, p.h ?? 0.12]; break;
      case 'cylinder': segA = typeof p.seg === 'number' ? p.seg : 8; segB = 1; params = [p.rTop ?? p.r ?? 0.05, p.rBottom ?? p.r ?? 0.05, p.h ?? 0.1]; break;
      case 'torus': segA = p.radial ?? 3; segB = p.tubular ?? 10; params = [p.R ?? 0.1, p.tube ?? 0.02, p.arc ?? Math.PI * 2]; break;
      case 'box': segA = 1; segB = 1; params = size3(p.size ?? 0.1); break;
      case 'disc': segA = typeof p.seg === 'number' ? p.seg : 10; segB = 1; params = [p.r ?? 0.05]; break;
    }
    parts.push({
      name: p.name || `_${ui++}`, nameIdx: nameIdx(p.name || `_${ui}`), type: PTYPE[type], typeName: type,
      socket: SOCKETS.indexOf(at), color: colorIdx(p.color), mirror: !!p.mirror && !PAIRED.has(at), hidden: p.visible === false,
      rotOrder: ROT_ORDER[p.rotOrder || 'XYZ'], segA, segB, pos: p.pos || [0, 0, 0], rot: p.rot || [0, 0, 0],
      scale: p.scale ? size3(p.scale) : null, offset: p.offset || null, params,
    });
  }

  const behaviours = (BEHAVIOURS[S.id] || []).map((b) => {
    const kind = BKIND[b.target.kind], index = b.target.kind === 'part' ? nameIdx(b.target.name)
      : b.target.kind === 'socket' ? SOCKETS.indexOf(ALIAS[b.target.name] || b.target.name) : 0;
    if (index < 0) throw new Error(`${S.id}: behaviour target ${JSON.stringify(b.target)} not found`);
    return { ...b, kind, index };
  });
  const hooks = !!(S.build || S.update);
  return {
    id: S.id, name: S.name, palette, names, parts, behaviours, warn, approx, hooks,
    head: { size: hr, at: H, pivot: neck, squash: S.head.squash ?? (B ? 0.5 : 1), pitch: S.head.pitch ?? 0.65 },
    body: B ? { size: br, at: B } : null,
    face: { r: faceR, offset: faceOff },
    eyes: { x: E.x, y: E.y, r: E.r, shape: EYESHAPE[E.shape] ?? 0, tilt: E.tilt || 0, shine: E.shine !== false, color: colorIdx(E.color) },
    mouth: { type: MOUTH[M.type] ?? 0, y: M.y, w: M.w, open: M.open },
    cheeks: C, motion: { type: MOTION[mo.type] ?? 0, amp: mo.amp, rate: mo.rate, height: mo.height ?? 0.08 },
    limbs, sockets, neckRing: [Math.max(0.05, ex0) * 1.04, Math.max(0.05, ez0) * 1.04],
    items: { head: Math.max(0, HEAD_ITEMS.indexOf(S.items?.head || 'none')), neck: Math.max(0, NECK_ITEMS.indexOf(S.items?.neck || 'none')) },
    face_colors: { shine: colorIdx('shine'), white: colorIdx('white'), mouth: colorIdx('mouth'), tongue: colorIdx('tongue'), cheek: colorIdx('cheek') },
  };
}

// ---- binary writer ----
class W {
  constructor() { this.b = []; }
  u8(v) { this.b.push(v & 255); }
  u16(v) { this.u8(v); this.u8(v >> 8); }
  u32(v) { this.u16(v & 0xffff); this.u16(v >>> 16); }
  q(v) {
    if (!Number.isFinite(v)) throw new Error(`non-finite value ${v}`);
    const r = Math.round(v * Q);
    if (r < -32768 || r > 32767) throw new Error(`value ${v} out of Q12 range (±8)`);
    this.u16(r & 0xffff);
  }
  q8(v) {
    const r = Math.round(v * Q8);
    if (!Number.isFinite(v) || r < -32768 || r > 32767) throw new Error(`value ${v} out of Q8 range (±128)`);
    this.u16(r & 0xffff);
  }
  q3(a) { this.q(a[0]); this.q(a[1]); this.q(a[2]); }
  str(s) { const e = Buffer.from(s, 'utf8'); if (e.length > 255) throw new Error('string too long'); this.u8(e.length); for (const c of e) this.u8(c); }
}
const crcTable = new Uint32Array(256).map((_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
export function crc32(bytes) { let c = 0xffffffff; for (const x of bytes) c = crcTable[(c ^ x) & 255] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; }

export function encode(R) {
  const w = new W();
  for (const ch of MAGIC) w.u8(ch.charCodeAt(0));
  w.u8(VERSION); w.u8((R.body ? 1 : 0) | (R.cheeks ? 2 : 0) | (R.eyes.shine ? 4 : 0)); w.u16(0); // length patched below
  w.str(R.id); w.str(R.name);
  w.u8(R.palette.length);
  for (const p of R.palette) { w.str(p.key); const h = p.hex.replace('#', ''); w.u8(parseInt(h.slice(0, 2), 16)); w.u8(parseInt(h.slice(2, 4), 16)); w.u8(parseInt(h.slice(4, 6), 16)); }
  w.q3(R.head.size); w.q3(R.head.at); w.q3(R.head.pivot); w.q(R.head.squash); w.q(R.head.pitch);
  if (R.body) { w.q3(R.body.size); w.q3(R.body.at); }
  w.q3(R.face.r); w.q3(R.face.offset);
  w.q(R.eyes.x); w.q(R.eyes.y); w.q(R.eyes.r); w.u8(R.eyes.shape); w.q(R.eyes.tilt); w.u8(R.eyes.color);
  w.u8(R.mouth.type); w.q(R.mouth.y); w.q(R.mouth.w); w.q(R.mouth.open);
  const C = R.cheeks || { x: 0, y: 0, r: 0 }; w.q(C.x); w.q(C.y); w.q(C.r);
  const fc = R.face_colors; w.u8(fc.shine); w.u8(fc.white); w.u8(fc.mouth); w.u8(fc.tongue); w.u8(fc.cheek);
  w.u8(R.motion.type); w.q(R.motion.amp); w.q(R.motion.rate); w.q(R.motion.height);
  for (const k of ['side', 'ear', 'tail', 'foot', 'antenna']) w.q(R.limbs[k]);
  w.u8(R.sockets.length);
  for (const s of R.sockets) { w.u8(ON[s.on]); w.q3(s.pos); w.q3(s.rot); }
  w.q(R.neckRing[0]); w.q(R.neckRing[1]);
  w.u8(R.items.head); w.u8(R.items.neck);
  w.u8(R.names.length); for (const n of R.names) w.str(n);
  w.u8(R.parts.length);
  for (const p of R.parts) {
    w.u8(p.type); w.u8(p.socket); w.u8(p.color); w.u8(p.nameIdx);
    w.u8((p.mirror ? 1 : 0) | (p.hidden ? 2 : 0) | (p.scale ? 4 : 0) | (p.offset ? 8 : 0));
    w.u8(p.rotOrder); w.u8(p.segA); w.u8(p.segB);
    w.q3(p.pos); w.q3(p.rot);
    if (p.scale) w.q3(p.scale);
    if (p.offset) w.q3(p.offset);
    w.u8(p.params.length); for (const v of p.params) w.q(v);
  }
  w.u8(R.behaviours.length);
  for (const b of R.behaviours) {
    w.u8(b.kind); w.u8(b.index); w.u8(BCHAN[b.channel]); w.u8(BOP[b.op || 'add']); w.u8(b.terms.length);
    for (const t of b.terms) {
      const gB = t.gainB ? BFLAG.gainB : 0;
      w.u8(BDRV[t.driver]); w.u8((t.sideSign ? 1 : 0) | (t.sidePhase ? 2 : 0) | (t.ease ? 4 : 0) | gB);
      w.q(t.amp ?? 1); w.q8(t.freq ?? 0); w.q(t.phase ?? 0); w.q8(t.ease ?? 0); w.q8(t.p0 ?? 0); w.q8(t.p1 ?? 0);
      const gain = (g) => { const v = [g?.k ?? (g ? 0 : 1)]; for (const m of MOODS) v.push(g?.[m] ?? 0); return v; };
      for (const v of gain(t.gain)) w.q(v);
      if (gB) for (const v of gain(t.gainB)) w.q(v);
    }
  }
  const len = w.b.length + 4;
  if (len > 0xffff) throw new Error('creature too large');
  w.b[6] = len & 255; w.b[7] = len >> 8;
  w.u32(crc32(w.b));
  return Uint8Array.from(w.b);
}

// ---- CLI ----
if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const args = process.argv.slice(2).filter((a) => !a.startsWith('--'));
  const json = process.argv.includes('--json');
  const want = args[0] || 'all';
  const out = resolve(args[1] || join(HERE, '../molty'));
  mkdirSync(out, { recursive: true });
  const list = want === 'all' ? REGISTRY : REGISTRY.filter((s) => s.id === want || s.id === `cute-${want}`);
  if (!list.length) { console.error(`no creature "${want}" (have: ${REGISTRY.map((s) => s.id).join(', ')})`); process.exit(1); }
  for (const S of list) {
    const R = resolveSpec(S);
    const bin = encode(R);
    const base = S.id.replace(/^cute-/, '');
    writeFileSync(join(out, `${base}.molty`), bin);
    if (json) writeFileSync(join(out, `${base}.molty.json`), JSON.stringify(R, null, 1));
    const notes = [];
    if (R.approx) notes.push(`${R.approx} custom part(s) approximated`);
    if (R.hooks) notes.push(R.behaviours.length ? `hooks → ${R.behaviours.length} behaviour(s)` : 'JS hooks NOT expressed (no behaviours yet)');
    console.log(`${base}.molty  ${bin.length} B  ${R.parts.length} parts  ${notes.join(' · ')}`);
    for (const w of R.warn) console.log(`   ! ${w}`);
  }
}
