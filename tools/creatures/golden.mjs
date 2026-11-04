#!/usr/bin/env node
// Goldens for the C creature player (device/firmware/main/creature/creature_player.c): plays each creature package through the
// reference player (device/creatures/js/creature-player.js) for every mood, every gesture and a walk, and writes what went in
// and what came out of each frame, for test/host/creature_test.c to replay and compare.
//   node tools/creatures/golden.mjs <out dir> [seconds]      writes <out dir>/<id>.golden for every creature package
// Per frame:  i dt <pose 18> eye mood act act_t act_n prev prev_t prev_w prev_n <walk 7>   (- for no action)
//             o <pose 18> <every bone then root: rx ry rz px py pz sx sy sz> <tints: per depth, per palette entry 0xrrggbb>
//             a <marks: kind x y size color d rot>... | <impulses: x y k r>... | <push: x y vx vy r>      (octopus: ambient)
// The ambient sees a fixed coverage (a disc), so it does not depend on a renderer.
import { readFileSync, writeFileSync, mkdirSync, readdirSync, existsSync } from 'node:fs';
import { creaturePlayer } from '../../device/creatures/js/creature-player.js';
import { MoodEngine, MOODS } from '../../device/creatures/js/mood.js';
import { actionPose, ACTIONS } from '../../device/creatures/js/actions.js';

const [out, secArg] = process.argv.slice(2); if (!out) throw new Error('usage: golden.mjs <out dir> [seconds]');
const SEC = +(secArg ?? 40), ROOT = new URL('../../packages/', import.meta.url), DT = 1 / 30, W = 52, H = 32, WIN = 0.85;
const POSE = ['pos.x', 'pos.y', 'pos.z', 'rot.x', 'rot.y', 'rot.z', 'head.x', 'head.y', 'head.z', 'spine.x', 'spine.y', 'spine.z', 'squash', 'lid', 'mouth', 'look.x', 'look.y', 'prowl'];
const EYES = ['normal', 'wide', 'happy', 'sleepy', 'squint'], WALK = ['q', 'k', 'go', 'along', 'side', 'fz', 'up'];
const get = (p, k) => { const [a, b] = k.split('.'); return (b ? p[a][b] : p[a]) ?? 0; };
const n = (v) => (Object.is(v, -0) ? 0 : +v.toPrecision(9));
// Math.random drives the mood engine's jitter: seeded, so the goldens are the same on every run
let seed = 7; Math.random = () => ((seed = (seed * 1103515245 + 12345) >>> 0) / 4294967296);
const cov = new Float32Array(W * H); for (let j = 0; j < H; j++) for (let i = 0; i < W; i++) { const x = ((i + 0.5) / W * 2 - 1) * WIN, y = (1 - (j + 0.5) / H * 2) * WIN; cov[j * W + i] = Math.hypot(x, y + 0.1) < 0.42 ? 1 : 0; }

mkdirSync(out, { recursive: true });
for (const id of readdirSync(ROOT).filter((d) => existsSync(new URL(`${d}/${d}.json`, ROOT)))) {
  const pkg = JSON.parse(readFileSync(new URL(`${id}/${id}.json`, ROOT))); if (pkg.kind !== 'creature') continue;
  const cr = pkg.creature, c = creaturePlayer(cr, { actionPose, ACTIONS }), eng = new MoodEngine(), ids = cr.body.bones.map((b) => b.id).concat('root');
  const depths = [...new Set(cr.body.bones.map((b) => b.d ?? 0))].sort((a, b) => a - b), pal = Object.keys(cr.body.palette);
  const bone = () => ({ rotation: { x: 0, y: 0, z: 0 }, position: { x: 0, y: 0, z: 0 }, scale: { x: 1, y: 1, z: 1 } });
  const acts = Object.keys(ACTIONS).filter((a) => a !== 'inspect'); let q = 0, plays = 0;
  const L = [`creature ${id} bones ${ids.length} ${ids.join(' ')} depths ${depths.length} ${depths.join(' ')} palette ${pal.length} ${pal.join(' ')}`];
  for (let f = 0; f < SEC * 30; f++) {
    const t = f * DT; if (f % 90 === 0) eng.set(MOODS[(f / 90) % MOODS.length]); if (f % 45 === 20) { eng.act(acts[(f / 45 | 0) % acts.length]); plays++; eng.action.n = plays; }
    const p = eng.update(DT), walking = Math.floor(t / 10) % 3 === 1, k = walking ? 1 : 0; q = (q + DT * c.move.hz * k) % 1;
    const w = { q, k, go: k, along: 0.3 * Math.sin(t), side: 0.2 * Math.sin(t * 0.7), fz: 0, up: k }, a = eng.action, pa = eng.prevAction;
    L.push(['i', DT, ...POSE.map((k) => n(get(p, k))), Math.max(0, EYES.indexOf(p.eye)), MOODS.indexOf(p.mood), a ? a.name : '-', n(a?.t ?? 0), a?.n ?? 0, pa ? pa.name : '-', n(pa?.t ?? 0), n(pa?.w ?? 0), pa?.n ?? 0, ...WALK.map((k) => n(w[k]))].join(' '));
    c.move.move(p, w); c.pose(p, DT, { action: a, prevAction: pa });
    const bones = new Map(ids.map((b) => [b, bone()])); c.apply(bones);
    const o = ['o', ...POSE.map((k) => n(get(p, k)))];
    for (const b of ids) { const g = bones.get(b); o.push(...[g.rotation.x, g.rotation.y, g.rotation.z, g.position.x, g.position.y, g.position.z, g.scale.x, g.scale.y, g.scale.z].map(n)); }
    for (const d of depths) { const tn = c.tints(cr.body.palette, d); o.push(...pal.map((k) => '0x' + tn[k].slice(1))); }
    L.push(o.join(' '));
    if (cr.ambient) { const marks = [], imp = [], push = [];
      c.ambient({ dt: DT, cov, W, H, win: WIN, mark: (m) => marks.push([m.kind, m.x, m.y, m.size, m.color, m.d, m.rot ?? 0]), impulse: (...v) => imp.push(v), push: (...v) => push.push(v) });
      L.push(['a', marks.length, ...marks.flat().map((v) => (typeof v === 'number' ? n(v) : v.replace('#', '0x'))), imp.length, ...imp.flat().map(n), push.length, ...push.flat().map(n)].join(' ')); }
  }
  writeFileSync(`${out}/${id}.golden`, L.join('\n') + '\n'); console.log(`${id}: ${SEC * 30} frames, ${plays} gestures`);
}
