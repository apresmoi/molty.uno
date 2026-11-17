#!/usr/bin/env node
// Goldens for the C clip player and dots (main/creature/creature_clip.c, creature_dots.c): plays every rigged creature
// package through the reference (device/creatures/js/clip-player.js, clip-dots.js) on a schedule of moods, gestures and the
// walk, and writes each frame for test/host/clip_test.c to replay and compare.
//   node tools/creatures/clip-golden.mjs <out dir> [seconds]      writes <out dir>/<id>.clip.golden
// Lines:  m <mood> | a <gesture>                       what the agent asked, before the frame
//         f <clip> <t> <cam x z yaw> <world 12 a node>   after the frame
//         d <cells: W*H braille bytes, hex>              every 10th frame, after the frame's draw
import { readFileSync, writeFileSync, mkdirSync, readdirSync, existsSync } from 'node:fs';
import { clipPlayer, MOODS } from '../../device/creatures/js/clip-player.js';
import { clipDots } from '../../device/creatures/js/clip-dots.js';

const [out, secArg] = process.argv.slice(2); if (!out) throw new Error('usage: clip-golden.mjs <out dir> [seconds]');
const SEC = +(secArg ?? 90), ROOT = new URL('../../packages/', import.meta.url), DT = 1 / 30, W = 52, H = 32;
const n = (v) => (Object.is(v, -0) ? 0 : +(+v).toPrecision(7));
mkdirSync(out, { recursive: true });
for (const id of readdirSync(ROOT).filter((d) => existsSync(new URL(`${d}/${d}.json`, ROOT)))) {
  const pkg = JSON.parse(readFileSync(new URL(`${id}/${id}.json`, ROOT))); if (pkg.kind !== 'creature' || !pkg.creature.rig) continue;
  const rig = pkg.creature.rig, p = clipPlayer(rig), r = clipDots(rig, W, H), gest = Object.keys(rig.graph.gestures);
  const L = [`rig ${id} nodes ${rig.nodes.length} frames ${SEC * 30}`];
  for (let f = 0; f < SEC * 30; f++) {
    if (f % 240 === 0) { const m = MOODS[(f / 240) % MOODS.length]; L.push(`m ${m}`); p.mood(m); }
    if (f % 240 === 120) { const g = gest[(f / 240 | 0) % gest.length]; L.push(`a ${g}`); p.act(g); }
    p.update(DT);
    const c = p.cam;
    L.push(['f', p.clip, n(p.t), n(c.x), n(c.z), n(c.yaw), ...Array.from(p.world, n)].join(' '));
    if (f % 10 === 0) { r.draw(p.world, c, f); L.push('d ' + Buffer.from(r.cells().dots).toString('hex')); }
  }
  writeFileSync(`${out}/${id}.clip.golden`, L.join('\n') + '\n'); console.log(`${id}: ${SEC * 30} frames`);
}
