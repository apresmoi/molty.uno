// SPDX-License-Identifier: MIT
// molty-creature check and build (docs/CREATURE.md): a Molty folder (molty.json, and the file that
// holds its body and actions) into the creature package the device plays (docs/CREATURES.md section
// 12). Names do the wiring: a mood or a gesture plays the action of its name, a pose rests in
// <pose>_idle, <a>_to_<b> goes from one pose to another, and what is not named is worked out.
// Needs no 3D tool: whatever made the body file, only the file is read.
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { basename, dirname, join, relative, resolve } from 'node:path';
import { ACTIONS } from '../../../device/creatures/js/actions.js';
import { FEAT, STRUCT } from '../../../device/creatures/js/clip-dots.js';
import { packageText } from '../../../tools/creatures/kit.mjs';
import { packagePath } from '../display/build.mjs';
import { parse, where, whereAt } from '../display/load.mjs';
import { validate } from '../display/schema.mjs';
import { MAX_PKG_BYTES, checkPackage } from '../format.mjs';
import { SCALE, bakeAction, r6 } from './bake.mjs';
import { SHAPES, readGltf, readInline } from './body.mjs';

export const SCHEMA = JSON.parse(readFileSync(new URL('../../../schema/molty-2.json', import.meta.url), 'utf8'));
/** What the device holds (main/creature/creature_clip.h). */
export const LIMITS = { nodes: 80, shapes: 128, actions: 64, poses: 8, ways: 64, gestures: 32, name: 24 };
const VIEW = { front: 0, side: -90, three_quarter: -40 }, BLEND = 0.3, FPS = 24;
const KIND = { blob: 'blob', ellipsoid: 'ell', capsule: 'cap', cone: 'cone', cylinder: 'cyl', patch: 'patch' };
const kb = (n) => `${(n / 1024).toFixed(1)} KB`, list = (names) => names.join(', ') || 'none';
const said = (v) => (typeof v === 'string' ? { about: v } : v);
// the colors the device paints a part with, whatever color its shapes name (clip-dots.js)
const PAINTS = { eye: ['iris', 'shine'], nose: ['nose'], tongue: ['nose'], maw: ['nose'], milk: ['belly'], chest: ['belly'], earIn: ['ear'], whisker: ['whisker'], bowl: ['bowl'], yarn: ['bowl'] };

/** A shape as the device takes it. The fields' order is the one each kind has always had in the cat's package. */
function shape(s, nodes) {
  const r = (v) => v.map(r6), k = KIND[s.shape], on = { b: s.node, g: s.outline, c: s.color };
  const body = k === 'blob' ? { p: r(s.at), q: r(s.turn), r: r(s.radii), st: r6(s.stiffness ?? 2) }
    : k === 'ell' || k === 'patch' ? { p: r(s.at), q: r(s.turn), r: r(s.radii) }
    : k === 'cone' ? { p: r(s.at), p1: r(s.to), r: r([s.radius, s.flat ?? 1]), fa: r(s.flat_axis ?? [0, 0, 1]) }
    : { p: r(s.at), p1: r(s.to), r: r([s.radius].flat().length === 2 ? s.radius : [s.radius, s.radius]) };
  return ['blob', 'patch', 'cyl'].includes(k) || (s.node > 0 && nodes[s.node].parent < 0) ? { k, ...on, ...body } : { ...on, k, ...body };
}

/**
 * Everything wrong with the Molty folder: { errors, warnings, doc, pkg, bytes, body }. errors and warnings are
 * [{ at: "file:line" or the file, message }]; pkg only when nothing stops it.
 */
export function check(dir) {
  const errors = [], warnings = [], bad = (at, message) => void errors.push({ at, message }), warn = (at, message) => void warnings.push({ at, message });
  if (!existsSync(join(dir, 'molty.json'))) return { errors: [{ at: 'molty.json:1', message: `no molty.json in ${dir}` }], warnings };
  let doc;
  try { doc = parse(readFileSync(join(dir, 'molty.json'), 'utf8'), 'molty.json'); } catch (e) { return { errors: [{ at: `molty.json:${e.line ?? 1}`, message: e.message }], warnings }; }
  for (const e of validate(SCHEMA, doc)) bad(whereAt(doc, 'key' in e ? [...e.path, e.key] : e.path), `${e.path.reduce((s, k) => (typeof k === 'number' ? `${s}[${k}]` : s ? `${s}.${k}` : k), '') || 'molty.json'} ${e.message}`);
  if (errors.length) return { errors, warnings, doc };

  // the body, whatever it was written as
  const fps = doc.fps ?? FPS, file = typeof doc.body === 'string' ? doc.body : 'molty.json', here = where(doc, 'body');
  let body;
  try {
    if (typeof doc.body === 'string' && !existsSync(join(dir, doc.body))) throw new Error('does not exist');
    body = typeof doc.body === 'string' ? readGltf(join(dir, doc.body)) : readInline(doc.body, fps);
  } catch (e) { bad(here, `${file}: ${e.message}`); return { errors, warnings, doc }; }
  const { nodes, shapes } = body, actions = new Map(body.actions.map((a) => [a.name, a])), names = [...actions.keys()], node = (n) => nodes.findIndex((x) => x.name === n);
  const over = (n, most, what) => { if (n > most) bad(here, `${file} has ${n} ${what}: a Molty holds ${most}`); };
  over(nodes.length, LIMITS.nodes, 'bones'); over(shapes.length, LIMITS.shapes, 'shapes'); over(names.length, LIMITS.actions, 'actions');
  for (const n of [...nodes.map((x) => x.name), ...names]) if (n.length > LIMITS.name) bad(here, `${file}: the name ${n} is ${n.length} letters: ${LIMITS.name} at most`);
  for (const [i, s] of shapes.entries()) {
    if (!SHAPES.includes(s.shape)) bad(here, `${file}: shape ${i} is a "${s.shape}": a shape is one of ${list(SHAPES)}`);
    else if (!(s.color in doc.colors)) bad(here, `${file}: a ${s.shape} on ${nodes[s.node].name} wears the color ${s.color}, which colors does not have (colors: ${list(Object.keys(doc.colors))})`);
    else if (![...STRUCT, ...FEAT].includes(s.outline)) bad(here, `${file}: a ${s.shape} on ${nodes[s.node].name} is part of "${s.outline}", which Molty does not know how to draw. Outlined parts: ${list(STRUCT)}. Features: ${list(FEAT)}`);
    else if (['capsule', 'cylinder', 'cone'].includes(s.shape) && (!s.to || s.radius === undefined)) bad(here, `${file}: a ${s.shape} on ${nodes[s.node].name} needs to (its other end) and radius`);
  }
  for (const c of new Set(['body', ...shapes.flatMap((s) => PAINTS[s.outline] ?? [])])) if (!(c in doc.colors)) bad(where(doc, 'colors'), `colors needs ${c}: Molty paints ${c === 'body' ? 'every outlined part' : `the ${list(Object.keys(PAINTS).filter((p) => PAINTS[p].includes(c) && shapes.some((s) => s.outline === p)))}`} with it`);
  if (!shapes.length) bad(here, `${file} has no shapes, so nothing would be drawn: mark what to draw (docs/CREATURE.md, "Shapes")`);

  // poses rest in <pose>_idle, or <pose>, and the first in idle; moods and resting actions loop
  const poses = doc.poses ?? ['sit'], rest = {}, moods = {}, gestures = {};
  poses.forEach((p, i) => {
    rest[p] = [`${p}_idle`, p, ...(i ? [] : ['idle'])].find((n) => actions.has(n));
    if (!rest[p]) bad(where(doc, 'poses'), `pose ${p} has no resting action: name one ${p}_idle${i ? '' : ' (or idle, for the first pose)'} (actions: ${list(names)})`);
  });
  const plays = (group, target) => {
    for (const [n, v] of Object.entries(doc[group] ?? {})) {
      target[n] = said(v).plays ?? n;
      if (!actions.has(target[n])) bad(where(doc[group], n), `${group.slice(0, -1)} ${n}: there is no action named ${target[n]}. Name an action ${n}, or say which one it plays: { "about": ..., "plays": ... } (actions: ${list(names)})`);
    }
  };
  plays('moods', moods); plays('gestures', gestures);
  for (const key of ['feet', 'center']) (doc[key] ?? []).forEach((n, i) => { if (node(n) < 0) bad(where(doc[key], i), `${key}: there is no bone named ${n} (bones: ${list(nodes.map((x) => x.name))})`); });
  if (errors.length) return { errors, warnings, doc, body };

  const loops = new Set([...Object.values(rest), ...Object.values(moods)]), baked = new Map();
  for (const a of body.actions) baked.set(a.name, bakeAction(nodes, a, fps, loops.has(a.name)));
  // which pose an action starts and ends in: the one whose resting action starts nearest (the root's place left out)
  const far = (p, q) => { let d = 0; for (let n = 0; n < nodes.length; n++) { const o = n * 10; if (n) for (let c = 0; c < 3; c++) d += (p[o + c] - q[o + c]) ** 2; let dot = 0; for (let c = 3; c < 7; c++) dot += p[o + c] * q[o + c]; d += 1 - dot * dot; } return d; };
  const nearest = (pose) => poses.reduce((best, p) => (far(pose, baked.get(rest[p]).first) < far(pose, baked.get(rest[best]).first) ? p : best));
  const ends = new Map(body.actions.map((a) => {
    const b = baked.get(a.name), given = [a.from, a.to].filter(Boolean).find((p) => !poses.includes(p));
    if (given) bad(here, `${file}: action ${a.name} says pose ${given}, which poses does not list (poses: ${list(poses)})`);
    return [a.name, [a.from ?? nearest(b.first), a.to ?? (loops.has(a.name) ? a.from ?? nearest(b.first) : nearest(b.last))]];
  }));
  // a mood's own way in from a pose: <pose>_<mood>
  const enter = {};
  for (const [m, a] of Object.entries(moods)) for (const p of poses) if (actions.has(`${p}_${m}`)) (enter[a] ??= {})[p] = `${p}_${m}`;
  // the way from one pose to another: <a>_to_<b>, or the one action that goes that way and that nothing else plays
  const taken = new Set([...Object.values(rest), ...Object.values(moods), ...Object.values(gestures), ...Object.values(enter).flatMap((e) => Object.values(e))]), edges = {};
  for (const a of poses) for (const b of poses) {
    if (a === b) continue;
    const named = `${a}_to_${b}`, free = names.filter((n) => !taken.has(n) && ends.get(n)[0] === a && ends.get(n)[1] === b);
    if (actions.has(named)) edges[`${a}>${b}`] = named;
    else if (free.length === 1) [edges[`${a}>${b}`]] = free;
    else if (free.length) bad(where(doc, 'poses'), `${free.length} actions go from ${a} to ${b} (${list(free)}): name the one to use ${named}`);
  }
  for (const m of Object.keys(moods)) { const b = baked.get(moods[m]), gap = far(b.first, b.last); if (gap > 0.5) warn(where(doc.moods, m), `mood ${m}: its action ${moods[m]} loops, but ends far from the pose it starts in`); }

  const clips = {};
  for (const a of body.actions) {
    const b = baked.get(a.name), [from, to] = ends.get(a.name);
    clips[a.name] = { dur: b.dur, loop: loops.has(a.name), from, to, view: typeof a.view === 'number' ? a.view : VIEW[a.view ?? 'front'], follow: a.follow === true, frames: b.frames, ch: b.ch };
    if (clips[a.name].view === undefined) bad(here, `${file}: action ${a.name} is seen from "${a.view}": say front, side, three_quarter or degrees`);
  }
  const hold = Object.fromEntries(Object.entries(doc.gestures ?? {}).filter(([, v]) => said(v).hold !== undefined).map(([g, v]) => [g, v.hold]));
  const sync = Object.fromEntries(body.actions.filter((a) => a.stride !== undefined).map((a) => [a.name, a.stride]));
  const lasts = (g) => said(doc.gestures[g]).lasts ?? Math.min(10, Math.max(0.2, Math.round(clips[gestures[g]].dur * 10) / 10));
  const first = Math.max(0, nodes.findIndex((n) => n.parent === 0));
  const rig = {
    fps,
    nodes: nodes.map((n) => ({ id: n.name, parent: n.parent, t: n.t.map(r6), r: n.r.map(r6), s: n.s.map(r6) })),
    shapes: shapes.map((s) => shape(s, nodes)),
    contacts: (doc.feet ?? []).map((n) => [node(n), (nodes[node(n)].foot ?? [0, 0, 0]).map(r6)]),
    clips,
    graph: { families: poses, rest, edges, blend: doc.blend ?? BLEND, center: doc.center ? doc.center.map(node) : [first, first], moods, gestures,
      ...(Object.keys(hold).length ? { hold } : {}), ...(Object.keys(enter).length ? { enter } : {}), ...(Object.keys(sync).length ? { sync } : {}) },
    palette: doc.colors,
    scale: SCALE,
  };
  const pkg = { v: 'molty-pkg/1', id: doc.id, name: doc.name, version: doc.version, kind: 'creature',
    creature: {
      about: { look: doc.look, gestures: Object.fromEntries(Object.entries(doc.gestures ?? {}).map(([g, v]) => [g, said(v).about])), moods: Object.fromEntries(Object.entries(doc.moods).map(([m, v]) => [m, said(v).about])) },
      // the channel layers stay empty: everything a rigged Molty does is in its clips
      body: { palette: doc.colors, bones: [], parts: [] },
      // a gesture every Molty shares keeps its own length; one of its own says how long it plays
      gestures: Object.fromEntries(Object.keys(gestures).map((g) => [g, g in ACTIONS ? { ch: [] } : { dur: lasts(g), ch: [] }])),
      moods: Object.fromEntries(Object.keys(moods).map((m) => [m, { ch: [] }])),
      rig,
    } };
  const bytes = Buffer.from(packageText(pkg));
  if (!errors.length) for (const p of checkPackage(JSON.parse(bytes), doc.id)) bad('molty.json:1', `${p} (the device's own check of the built Molty)`);
  if (bytes.length > MAX_PKG_BYTES) {
    const size = Object.entries(clips).map(([n, c]) => [n, JSON.stringify(c).length]).sort((a, b) => b[1] - a[1]).slice(0, 3);
    bad(here, `the built Molty is ${kb(bytes.length)}: the device takes ${kb(MAX_PKG_BYTES)}. Its largest actions: ${size.map(([n, s]) => `${n} ${kb(s)}`).join(', ')}. Shorten or drop some`);
  }
  for (const n of names) if (!taken.has(n) && !Object.values(edges).includes(n)) warn(here, `${file}: nothing plays the action ${n}: name a gesture or a mood after it, or leave it out (it costs ${kb(JSON.stringify(clips[n]).length)})`);
  return { errors, warnings, doc, body, pkg, bytes, clips };
}

/**
 * check(), then write the package: to `out`, else packages/<id>/<id>.json for a folder under the repo's moltys/, else
 * the folder's own build/. Returns check()'s result plus { out, report }; nothing is written when there are errors.
 */
export function build(dir, out) {
  const r = check(dir);
  if (r.errors.length) return r;
  const { doc, pkg, bytes, body } = r, rig = pkg.creature.rig, rel = (f) => (relative(process.cwd(), f).startsWith('..') ? f : relative(process.cwd(), f));
  out = resolve(out ?? packagePath(dir, doc.id));
  mkdirSync(dirname(out), { recursive: true });
  writeFileSync(out, bytes);
  // the catalog's line (packages/index.json reads it) sits beside a package that lands in packages/
  if (basename(dirname(dirname(out))) === 'packages') writeFileSync(join(dirname(out), 'description.txt'), `${doc.about}\n`);
  const frames = Object.values(rig.clips).reduce((n, c) => n + c.frames, 0);
  const report = [
    `body      ${rig.nodes.length} bones, ${rig.shapes.length} shapes, ${Object.keys(doc.colors).length} colors`,
    `actions   ${body.actions.length}, ${frames} frames, ${kb(JSON.stringify(rig.clips).length)}; ${Object.keys(rig.graph.moods).length} moods, ${Object.keys(rig.graph.gestures).length} gestures, ${rig.graph.families.length} poses, ${Object.keys(rig.graph.edges).length} ways between them`,
    `package   ${rel(out)}  ${kb(bytes.length)} of ${kb(MAX_PKG_BYTES)}  ok`,
  ];
  return { ...r, out, report };
}
