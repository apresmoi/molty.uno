#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// molty-creature: the tools for a Molty folder (docs/CREATURE.md): molty.json and its body file.
//   molty-creature check <folder>                  the file, the names, what the device can hold
//   molty-creature build <folder> [--out <file>]   the package Molty installs, and what it holds
//   molty-creature preview <folder> [--port 4174]  a local page: the Molty moving, every mood, gesture and action
// Exits 1 when there are errors. Needs no 3D tool.
import { build, check } from './creature/build.mjs';

const argv = process.argv.slice(2), opt = (k, d) => { const i = argv.indexOf(k); return i < 0 ? d : argv[i + 1]; };
const [cmd, target] = argv.filter((a, i) => !a.startsWith('--') && !['--out', '--port'].includes(argv[i - 1]));
const USAGE = 'usage: molty-creature check|build|preview <Molty folder> [--out file] [--port n]';

/** Prints what check found; true when nothing stops a build. */
function tell({ errors, warnings }) {
  for (const w of warnings) console.log(`${w.at}  note: ${w.message}`);
  for (const e of errors) console.error(`${e.at}  ${e.message}`);
  return !errors.length;
}

try {
  if (!target) throw new Error(USAGE);
  if (cmd === 'check') {
    const r = check(target);
    if (!tell(r)) { console.error(`molty-creature: ${r.errors.length} to fix`); process.exit(1); }
    console.log(`molty-creature: ok (${r.body.nodes.length} bones, ${r.body.shapes.length} shapes, ${r.body.actions.length} actions)`);
  } else if (cmd === 'build') {
    const r = build(target, opt('--out'));
    if (!tell(r)) { console.error(`molty-creature: ${r.errors.length} to fix, nothing written`); process.exit(1); }
    console.log(r.report.join('\n'));
  } else if (cmd === 'preview') {
    const { preview } = await import('./creature/preview.mjs');
    preview(target, +opt('--port', 4174));
  } else throw new Error(USAGE);
} catch (e) {
  console.error(`molty-creature: ${e.message}`);
  process.exit(1);
}
