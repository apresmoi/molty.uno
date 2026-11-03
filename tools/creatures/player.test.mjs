// Every creature package plays: through the device's own mood engine, every mood and gesture, walking and still, for a while.
// Checks: it fits (32 KB), every op and reference resolves, no channel or bone output goes NaN or wild, and how much work a frame is.
// node --test tools/creatures/player.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync, existsSync, statSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { creaturePlayer } from '../../device/creatures/js/creature-player.js';
import { MoodEngine, MOODS } from '../../device/creatures/js/mood.js';
import { actionPose, ACTIONS } from '../../device/creatures/js/actions.js';

const ROOT = new URL('../../packages/', import.meta.url);
const creatures = readdirSync(ROOT).filter((d) => existsSync(new URL(`${d}/${d}.json`, ROOT))).map((d) => ({ id: d, file: new URL(`${d}/${d}.json`, ROOT) }))
  .map((c) => ({ ...c, pkg: JSON.parse(readFileSync(c.file, 'utf8')) })).filter((c) => c.pkg.kind === 'creature');
const bone = () => ({ rotation: { x: 0, y: 0, z: 0 }, position: { x: 0, y: 0, z: 0 }, scale: { x: 1, y: 1, z: 1 } });

/** Plays a creature for `sec` seconds at 30 fps through a schedule of moods, gestures and walking. Returns the frame stats. */
export function play(pkg, sec = 120) {
  const c = creaturePlayer(pkg.creature, { actionPose, ACTIONS }), eng = new MoodEngine(), bones = new Map(pkg.creature.body.bones.map((b) => [b.id, bone()]).concat([['root', bone()]]));
  const acts = Object.keys(ACTIONS).filter((a) => a !== 'inspect'), dt = 1 / 30; let worst = 0, sum = 0, n = 0, wild = null, q = 0;
  for (let f = 0; f < sec * 30; f++) {
    const t = f * dt; if (f % 90 === 0) eng.set(MOODS[(f / 90) % MOODS.length]); if (f % 45 === 20) eng.act(acts[(f / 45 | 0) % acts.length]);
    const p = eng.update(dt), walking = Math.floor(t / 10) % 3 === 1, k = walking ? 1 : 0; q = (q + dt * c.move.hz * k) % 1;
    c.move.move(p, { q, k, go: k, along: 0.3 * Math.sin(t), side: 0.2 * Math.sin(t * 0.7), fz: 0, up: k });
    c.pose(p, dt, { action: eng.action, prevAction: eng.prevAction });
    for (const g of bones.values()) { g.rotation.x = g.rotation.y = g.rotation.z = g.position.x = g.position.y = g.position.z = 0; g.scale.x = g.scale.y = g.scale.z = 1; }
    c.apply(bones); c.tints(pkg.creature.body.palette, 1);
    worst = Math.max(worst, c.factors); sum += c.factors; n++;
    for (const [id, g] of bones) for (const v of [g.rotation.x, g.rotation.y, g.rotation.z, g.position.x, g.position.y, g.position.z, g.scale.x, g.scale.y, g.scale.z]) if (!Number.isFinite(v) || Math.abs(v) > 20) wild ??= `${id} ${v} at ${t.toFixed(2)}s`;
    for (const k2 of ['pos', 'rot', 'head']) for (const a of ['x', 'y', 'z']) if (!Number.isFinite(p[k2][a])) wild ??= `pose ${k2}.${a} at ${t.toFixed(2)}s`;
  }
  return { worst, mean: sum / n, wild };
}

/** The format's limits (docs/CREATURES.md section 7): what a C player sizes its tables for. */
export function limits(cr) {
  const n = { channels: 0, terms: 0, factors: 0, wide: 0, hist: 0 }, walk = (l) => { for (const x of l ?? []) { if (x.each) { walk(x.ch); continue; } n.channels++; if ((x[2]?.hist ?? 0) > 1) n.hist++; for (const t of x[1]) { n.terms++; n.factors += t.length - 1; if (t.length - 1 > 8) n.wide++; } } };
  walk(cr.walk?.ch); walk(cr.base); walk(cr.post); for (const b of [...Object.values(cr.gestures ?? {}), ...Object.values(cr.moods ?? {})]) walk(b.ch);
  for (const e of Object.values(cr.events ?? {})) { walk(e.ch); for (const m of Object.values(e.pick ?? {})) walk(m.ch); }
  const g = Object.values(cr.groups ?? {});
  return [[cr.body.bones.length, 96, 'bones'], [cr.body.parts.length, 96, 'parts'], [n.channels, 512, 'channels'], [n.terms, 1024, 'terms'], [n.factors, 2048, 'factors'], [n.wide, 0, 'terms over 8 factors'],
    [Math.max(0, ...g.map((x) => x.bones.length)), 64, 'group instances'], [Math.max(0, ...g.map((x) => Object.keys(x.attr).length)), 32, 'group attributes'], [Object.keys(cr.events ?? {}).length, 16, 'events'], [n.hist, 0, 'histories over 1 s']];
}

for (const { id, file, pkg } of creatures) test(`creature package ${id}`, () => {
  assert.equal(pkg.v, 'molty-pkg/1'); assert.equal(pkg.id, id);
  assert.ok(statSync(file).size <= 32 * 1024, `${id} is ${statSync(file).size} bytes, over 32 KB`);
  for (const [v, max, what] of limits(pkg.creature)) assert.ok(v <= max, `${id}: ${v} ${what}, limit ${max}`);
  const idx = JSON.parse(readFileSync(new URL('index.json', ROOT)))[id], bytes = readFileSync(file);
  assert.deepEqual(idx, { name: pkg.name, version: pkg.version, kind: 'creature', path: `packages/${id}/${id}.json`, size: bytes.length, sha256: createHash('sha256').update(bytes).digest('hex') });
  const s = play(pkg, 90); assert.equal(s.wild, null, s.wild);
  assert.ok(s.worst < 6000, `${id}: ${s.worst} factors in one frame`);
});
