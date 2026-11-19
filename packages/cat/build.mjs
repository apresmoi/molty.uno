#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// cat.json is built from the cat's Molty folder (moltys/cat: molty.json and cat.glb; docs/CREATURE.md).
// This runs that build, so `make packages` and tools/scripts/check-packages.mjs rebuild it like every
// other package. It needs no Blender: cat.glb is written from cat.blend by tools/blender/molty_export.py
// when the cat changes, and committed.
import { fileURLToPath } from 'node:url';
import { build } from '../tools/creature/build.mjs';

const r = build(fileURLToPath(new URL('../../moltys/cat', import.meta.url)));
for (const e of r.errors) console.error(`${e.at}  ${e.message}`);
if (r.errors.length) process.exit(1);
console.log(r.report.join('\n'));
