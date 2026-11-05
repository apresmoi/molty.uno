// The cat's tail is a jointed chain that bends, never a stick swinging from its base: played through the reference player, the bend
// at each joint (the angle between one segment and the next) must change over time, most toward the tip, in every mood that moves it.
// node --test tools/creatures/cat-tail.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { creaturePlayer } from '../../device/creatures/js/creature-player.js';
import { MoodEngine } from '../../device/creatures/js/mood.js';
import { actionPose, ACTIONS } from '../../device/creatures/js/actions.js';

const cr = JSON.parse(readFileSync(new URL('../../packages/cat/cat.json', import.meta.url), 'utf8')).creature, rest = new Map(cr.body.bones.map((b) => [b.id, b]));
const tip = cr.body.parts.find((p) => p.bone === 'tail4' && p.type === 'tube').path.at(-1), chain = ['tail', 'tail2', 'tail3', 'tail4'];
// three.js bones: local = translate(pos) rotate(euler XYZ) scale, as [3x3 | t]
const local = (g) => { const [a, b, c] = ['x', 'y', 'z'].map((k) => Math.cos(g.rotation[k])), [d, e, f] = ['x', 'y', 'z'].map((k) => Math.sin(g.rotation[k])), s = g.scale;
  return { m: [b * c * s.x, -b * f * s.y, e * s.z, (a * f + d * e * c) * s.x, (a * c - d * e * f) * s.y, -d * b * s.z, (d * f - a * e * c) * s.x, (d * c + a * e * f) * s.y, a * b * s.z], t: [g.position.x, g.position.y, g.position.z] }; };
const mul = (A, B) => ({ m: [0, 1, 2].flatMap((i) => [0, 1, 2].map((j) => A.m[i * 3] * B.m[j] + A.m[i * 3 + 1] * B.m[3 + j] + A.m[i * 3 + 2] * B.m[6 + j])), t: at(A, B.t) });
const at = (A, p) => [0, 1, 2].map((i) => A.m[i * 3] * p[0] + A.m[i * 3 + 1] * p[1] + A.m[i * 3 + 2] * p[2] + A.t[i]);
const angle = (u, v) => Math.acos(Math.max(-1, Math.min(1, u.reduce((s, x, k) => s + x * v[k], 0) / Math.hypot(...u) / Math.hypot(...v))));

/** The bends at tail2, tail3 and tail4 over `sec` seconds of a mood, after it settles. */
function bends(mood, sec = 8) {
  const c = creaturePlayer(cr, { actionPose, ACTIONS }), eng = new MoodEngine(), dt = 1 / 30, out = [];
  const bones = new Map([...rest.keys(), 'root'].map((id) => [id, { rotation: {}, position: {}, scale: {} }])); eng.set(mood);
  for (let f = 0; f < (sec + 5) * 30; f++) {
    const p = eng.update(dt); c.move.move(p, { q: 0, k: 0, go: 0, along: 0, side: 0, fz: 0, up: 0 }); c.pose(p, dt, { action: eng.action, prevAction: eng.prevAction });
    for (const [id, g] of bones) { const b = rest.get(id) ?? {}, r = b.rot ?? [0, 0, 0], q = b.pos ?? [0, 0, 0]; Object.assign(g.rotation, { x: r[0], y: r[1], z: r[2] }); Object.assign(g.position, { x: q[0], y: q[1], z: q[2] }); Object.assign(g.scale, { x: 1, y: 1, z: 1 }); }
    c.apply(bones); if (f < 150) continue;
    const W = new Map(), world = (id) => W.get(id) ?? W.set(id, id === 'root' ? local(bones.get(id)) : mul(world(rest.get(id).parent ?? 'root'), local(bones.get(id)))).get(id);
    const J = [...chain.map((id) => world(id).t), at(world('tail4'), tip)], seg = J.slice(1).map((p, i) => p.map((x, k) => x - J[i][k]));
    out.push(seg.slice(1).map((s, i) => angle(seg[i], s)));
  }
  return [0, 1, 2].map((j) => { const v = out.map((b) => b[j]); return Math.max(...v) - Math.min(...v); });
}

for (const [mood, min] of [['idle', [0.1, 0.3, 0.4]], ['thinking', [0.1, 0.3, 0.4]], ['listening', [0, 0.2, 0.3]], ['happy', [0, 0.25, 0.4]], ['surprised', [0, 0.25, 0.4]], ['sleepy', [0, 0, 0.1]]])
  test(`the cat's tail bends along its length, ${mood}`, () => {
    const s = bends(mood); s.forEach((v, j) => assert.ok(v >= min[j], `${mood}: the bend at ${chain[j + 1]} only moves ${v.toFixed(2)} rad (at least ${min[j]}): a rigid tail`));
  });
