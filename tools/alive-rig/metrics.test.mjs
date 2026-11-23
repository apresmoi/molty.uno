// The rig's numbers on made-up input, so the metric itself can be trusted before it scores a Molty.
// node --test tools/alive-rig/metrics.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { loopEcho, stillShare, rhythmSpread, bodySpread, boneMotion, pops } from './metrics.mjs';

let seed = 1;
const rnd = () => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) / 2 ** 32);
const frames = (n, f) => Array.from({ length: n }, (_, t) => Float32Array.from({ length: 64 }, (_, d) => f(t, d)));

test('loop echo: a sequence that repeats every 120 frames scores 1 at 120', () => {
  const period = 120, base = frames(period, () => rnd() * 255);
  const e = loopEcho(Array.from({ length: 1800 }, (_, t) => base[t % period]), 30, 900, 120);
  assert.equal(e.lag, 120);
  assert.ok(e.echo > 0.999, `echo ${e.echo}`);
  assert.ok(e.standout > 0.5, `standout ${e.standout}`);
});

test('loop echo: noise scores low, and no lag stands out', () => {
  seed = 2;
  const e = loopEcho(frames(1800, () => rnd() * 255), 30, 900, 120);
  assert.ok(Math.abs(e.echo) < 0.05, `echo ${e.echo}`);
  assert.ok(e.standout < 0.05, `standout ${e.standout}`);
});

test('loop echo: one big gesture does not hide the loop under it', () => {
  const period = 120, base = frames(period, () => rnd() * 10);
  const pics = Array.from({ length: 1800 }, (_, t) => (t >= 400 && t < 450 ? Float32Array.from(base[0], () => 255) : base[t % period]));
  const e = loopEcho(pics, 30, 900, 120);
  assert.equal(e.lag, 120);
  assert.ok(e.echo > 0.99, `echo ${e.echo}`);
});

test('loop echo: a picture that never changes has none', () => {
  assert.equal(loopEcho(frames(300, () => 7), 30, 900), null);
});

test('still share and rhythm spread', () => {
  assert.equal(stillShare([0, 0, 0.5, 0.005, 0.02]), 0.5);
  // a burst every 30 frames exactly: no spread
  const even = Array.from({ length: 600 }, (_, t) => (t % 30 < 5 ? 0.2 : 0));
  assert.ok(rhythmSpread(even).spread < 1e-9);
  // bursts at uneven gaps: spread
  const gaps = [20, 60, 25, 90, 30, 70], uneven = new Array(400).fill(0);
  let at = 0;
  for (const g of gaps) { at += g; for (let k = 0; k < 5; k++) uneven[at + k] = 0.2; }
  assert.ok(rhythmSpread(uneven).spread > 0.35);
});

test('body spread and pops: a tail that swings, a body that stays', () => {
  // root, pelvis, tail1 (child of pelvis): tail1 turns about z each frame, the rest is still
  const nodes = [{ id: 'root', parent: -1 }, { id: 'pelvis', parent: 0 }, { id: 'tail1', parent: 1 }];
  const I = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0];
  const turn = (a) => [Math.cos(a), -Math.sin(a), 0, 0, Math.sin(a), Math.cos(a), 0, 0.2, 0, 0, 1, 0];
  const bones = Array.from({ length: 60 }, (_, t) => Float32Array.from([...I, ...I, ...turn(0.3 * Math.sin(t / 5))]));
  const s = bodySpread(boneMotion(bones, nodes), nodes);
  assert.equal(s.tail, 1);
  assert.equal(s.body, 0);
  assert.equal(pops(boneMotion(bones, nodes)).length, 0);
  for (let t = 30; t < 60; t++) bones[t] = Float32Array.from([...I, ...I, ...turn(1.2 + 0.3 * Math.sin(t / 5))]);   // snaps to a new place
  assert.deepEqual(pops(boneMotion(bones, nodes)), [30]);
  assert.deepEqual(pops(boneMotion(bones, nodes), [25]), []);    // ...unless a step asked for it
});
