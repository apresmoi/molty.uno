#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// act-golden.mjs — the creature actions' curves (device/creatures/js/actions.js), sampled
// every 50 ms of each action, as the golden the firmware's twin (cute_act.h) is held to:
// device/firmware/test/host/act_golden.txt. The firmware host test (cute_build.sh) runs
// this with --check (the committed file must be what the JS writes) and compares its own
// samples with it.
//   node device/creatures/tools/act-golden.mjs          rewrite the golden
//   node device/creatures/tools/act-golden.mjs --check  exit 1 if it differs
import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { ACTIONS, actionPose, actionRow } from '../js/actions.js';

const OUT = resolve(dirname(fileURLToPath(import.meta.url)), '../../firmware/test/host/act_golden.txt');
const lines = [
  '# act_golden.txt — creature action curves, written by device/creatures/tools/act-golden.mjs',
  '# from device/creatures/js/actions.js (do not edit by hand). Columns: action t then the pose',
  '# offset: pos xyz, rot xyz, head xyz, spine xyz, squash, lid, look xy, mouth.',
];
for (const [name, dur] of Object.entries(ACTIONS)) {
  const ts = [];
  for (let k = 0; k * 0.05 < dur - 1e-9; k++) ts.push(+(k * 0.05).toFixed(2));
  ts.push(dur);
  for (const t of ts) lines.push(`${name} ${t.toFixed(2)} ${actionRow(actionPose(name, t)).map((v) => (Math.abs(v) < 5e-7 ? 0 : v).toFixed(6)).join(' ')}`);
}
const text = lines.join('\n') + '\n';
if (process.argv.includes('--check')) {
  let cur = '';
  try { cur = readFileSync(OUT, 'utf8'); } catch { /* missing */ }
  if (cur !== text) { console.error(`${OUT} is stale: run node device/creatures/tools/act-golden.mjs`); process.exit(1); }
  console.log(`act golden: ${lines.length - 3} samples match actions.js`);
} else {
  writeFileSync(OUT, text);
  console.log(`wrote ${OUT} (${lines.length - 3} samples)`);
}
