// The player's rules (docs/CREATURES.md), each on a tiny package: if one breaks, a creature quietly loses a behaviour.
// node --test tools/creatures/semantics.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { creaturePlayer } from '../../device/creatures/js/creature-player.js';

const pose = (mood = 'idle') => ({ pos: { x: 0, y: 0, z: 0 }, rot: { x: 0, y: 0, z: 0 }, head: { x: 0, y: 0, z: 0 }, spine: { x: 0, y: 0, z: 0 }, squash: 0, lid: 0, mouth: 0, look: { x: 0, y: 0 }, prowl: 0, mood });
const bone = () => ({ rotation: { x: 0, y: 0, z: 0 }, position: { x: 0, y: 0, z: 0 }, scale: { x: 1, y: 1, z: 1 } });
const run = (cr, frames = 1, opts = {}, ctx = () => ({}), mood) => { const c = creaturePlayer({ body: { bones: [], parts: [], palette: {} }, ...cr }, opts); let p;
  for (let f = 0; f < frames; f++) { p = pose(typeof mood === 'function' ? mood(f) : mood); c.pose(p, 0.05, ctx(f)); } const bones = new Map(['a', 'b', 'root'].map((k) => [k, bone()])); c.apply(bones); return { c, p, bones }; };

test('a term is gain times its factors, a channel the sum of its terms', () => {
  const { p } = run({ post: [['pos.x', [[2], [3, ['lin', '@pos.x', 4, 0]]]]] }); assert.equal(p.pos.x, 14);
});
test('group instances read their own attributes and their own locals', () => {
  const { bones } = run({ groups: { g: { bones: ['a', 'b'], attr: { k: [1, 2] } } }, post: [{ each: 'g', ch: [['%c', [['$k']]], ['%d', [[10, '@%c']]], [':rx', [[1, '@%d']]], [':ry', [['$i']]]] }] });
  assert.equal(bones.get('a').rotation.x, 10); assert.equal(bones.get('b').rotation.x, 20); assert.equal(bones.get('b').rotation.y, 1);
});
test('scale outputs multiply; = sets; sq keeps volume', () => {
  const { bones } = run({ post: [['a:sy', [[0.5]]], ['a:sy', [[1]]], ['b:s', [[0.3]], { op: '=' }], ['root:sq', [[0.44]]]] });
  assert.equal(bones.get('a').scale.y, 3); assert.ok(Math.abs(bones.get('b').scale.x - 0.3) < 1e-6); assert.ok(Math.abs(bones.get('root').scale.x - 1 / 1.2) < 1e-6);
});
test('a mood block is weighted, its scratch channels are not', () => {
  const { c, p } = run({ moods: { happy: { ch: [['_k', [[1]]], ['pos.x', [[1, '@_k']]]] } }, post: [['pos.y', [[1, '@_k']]]] }, 3, {}, () => ({}), 'happy');
  assert.ok(p.pos.x > 0 && p.pos.x < 1, `weighted ${p.pos.x}`); assert.equal(p.pos.y, 1); void c;
});
test('max and multiply ops', () => {
  const { p } = run({ base: [['lid', [[0.3]], { op: 'max' }], ['lid', [[0.2]], { op: 'max' }], ['pos.x', [[4]]], ['pos.x', [[-0.5]], { op: '*' }]] }); assert.equal(p.lid, 0.3); assert.equal(p.pos.x, 2);
});
test('ranges with one slot share one draw; each play draws again', () => {
  const g = { ch: [['pos.x', [[1, ['lin', 1, 0, [0, 1, 3]]]]], ['pos.y', [[[0, 2, 3]]]], ['pos.z', [[1, '~3']]]] }, A = { x: 1 }, actionPose = () => ({ pos: { x: 0, y: 0, z: 0 }, rot: { x: 0, y: 0, z: 0 }, head: { x: 0, y: 0, z: 0 }, spine: { x: 0, y: 0, z: 0 }, squash: 0, look: { x: 0, y: 0 } });
  const c = creaturePlayer({ body: { bones: [], parts: [], palette: {} }, gestures: { x: g } }, { actionPose, ACTIONS: A }), seen = new Set();
  for (let n = 0; n < 4; n++) { const p = pose(); c.pose(p, 0.05, { action: { name: 'x', t: 0.1 } }); assert.ok(Math.abs(p.pos.y - 2 * p.pos.x) < 1e-9); assert.ok(Math.abs(p.pos.z - p.pos.x) < 1e-9); seen.add(p.pos.x); }
  assert.equal(seen.size, 4);
});
test('a gesture removes the shared gesture it replaces', () => {
  const shared = { pos: { x: 0.5, y: 0, z: 0 }, rot: { x: 0, y: 0, z: 0 }, head: { x: 0, y: 0, z: 0 }, spine: { x: 0, y: 0, z: 0 }, squash: 0, look: { x: 0, y: 0 } };
  const c = creaturePlayer({ body: { bones: [], parts: [], palette: {} }, gestures: { nod: { ch: [] } } }, { actionPose: () => shared, ACTIONS: { nod: 1 } }), p = pose(); p.pos.x = 0.5;
  c.pose(p, 0.05, { action: { name: 'nod', t: 0.2 } }); assert.equal(p.pos.x, 0);
});
test('stateful channels: int integrates, lp smooths, keep carries a value into the next frame', () => {
  const { p } = run({ events: { e: { first: 0, gap: 0, len: 9, gate: [[1, '@_k']], ch: [['pos.z', [[1]]]] } }, base: [['_ph', [[2]], { int: 1 }], ['pos.x', [[1, '@_ph']]], ['pos.y', [[1]], { lp: 1e9 }]], post: [['_k', [[1]], { keep: 1 }]] }, 10);
  assert.ok(Math.abs(p.pos.x - 1) < 1e-9); assert.ok(Math.abs(p.pos.y - 1) < 1e-6); assert.equal(p.pos.z, 1);
});
test('a delayed channel reads its own past', () => {
  const { p } = run({ base: [['_x', [[1, '@t']], { hist: 1 }], ['pos.x', [[1, ['dl', '@_x', 0.2]]]], ['pos.y', [[1, '@t']]]] }, 20);
  assert.ok(Math.abs(p.pos.y - p.pos.x - 0.2) < 0.051, `${p.pos.y} ${p.pos.x}`);
});
test('an event group plays one member at a time, after its gap, while its gate is open', () => {
  let on = 0; const c = creaturePlayer({ body: { bones: [], parts: [], palette: {} }, events: { g: { first: 0.5, gap: [1, 1], len: 0.5, gate: [[1, '@is.idle']], pick: { a: { ch: [['pos.x', [[1]]]] }, b: { ch: [['pos.y', [[1]]]] } } } } });
  const starts = []; for (let f = 0; f < 100; f++) { const p = pose(f < 60 ? 'idle' : 'happy'); c.pose(p, 0.05); const now = p.pos.x + p.pos.y; assert.ok(now <= 1); if (now && !on) starts.push(f * 0.05); on = now; }
  assert.ok(starts[0] >= 0.5 && starts.length >= 2 && starts.every((t, i) => !i || t - starts[i - 1] >= 1.45) && starts.at(-1) < 3.05, starts.join(' '));
});
test('tints lerp the palette by channel, both ways', () => {
  const c = creaturePlayer({ body: { bones: [], parts: [], palette: {} }, tints: [{ ch: 'flush', to: { body: '#ffffff' }, neg: { body: '#000000' } }], post: [['flush', [[-0.5]]]] }); c.pose(pose(), 0.05);
  assert.deepEqual(c.tints({ body: '#808080', eye: '#808080' }), { body: '#404040', eye: '#808080' });
});
test('unknown ops, props and groups are refused', () => {
  assert.throws(() => run({ post: [['pos.x', [[1, ['nope', '@t']]]]] })); assert.throws(() => run({ post: [['a:wobble', [[1]]]] })); assert.throws(() => run({ post: [{ each: 'none', ch: [] }] }));
});
