#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// rhythm.json is built from the molty-rhythm skill folder (skills/molty-rhythm: display.yaml,
// screens/; docs/DISPLAY.md). This runs that build, so `make packages` and
// tools/scripts/check-packages.mjs rebuild it like every other package. Run it after any change
// there and commit the output (then `node tools/scripts/check-packages.mjs --write-index`).
import { fileURLToPath } from 'node:url';
import { build } from '../tools/display/build.mjs';

const r = build(fileURLToPath(new URL('../../skills/molty-rhythm', import.meta.url)));
for (const e of r.errors) console.error(`${e.at}  ${e.message}`);
if (r.errors.length) process.exit(1);
console.log(r.report.join('\n'));
