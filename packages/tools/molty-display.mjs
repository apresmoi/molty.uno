#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// molty-display: the tools for a display v2 skill folder (docs/DISPLAY.md).
//   molty-display check <skill>                    schema, device rules, SKILL.md links
//   molty-display build <skill> [--out <file>]     the package Molty installs, its cost, build/display.md, build/inputs/
//   molty-display convert <package.json> --to <folder>   a molty-pkg/1 scenes package as a skill folder
//   molty-display preview <skill> [--port 4173]    a local page: pick a screen, edit inputs, scrub time, tap zones
// <skill> is the folder that holds SKILL.md and display.yaml. Exits 1 when there are errors.
import { build, check } from './display/build.mjs';

const argv = process.argv.slice(2), opt = (k, d) => { const i = argv.indexOf(k); return i < 0 ? d : argv[i + 1]; };
const [cmd, target] = argv.filter((a, i) => !a.startsWith('--') && !['--out', '--to', '--port'].includes(argv[i - 1]));
const USAGE = 'usage: molty-display check|build|preview <skill folder> [--out file] [--port n], or convert <package.json> --to <folder>';

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
    if (!tell(r)) { console.error(`molty-display: ${r.errors.length} to fix`); process.exit(1); }
    console.log(`molty-display: ok (${Object.keys(r.skill.screens).length} screens, ${Object.keys(r.pkg.assets ?? {}).length} assets)`);
  } else if (cmd === 'build') {
    const r = build(target, opt('--out'));
    if (!tell(r)) { console.error(`molty-display: ${r.errors.length} to fix, nothing written`); process.exit(1); }
    console.log(r.report.join('\n'));
  } else if (cmd === 'convert') {
    if (!opt('--to')) throw new Error(USAGE);
    const { convert } = await import('./display/convert.mjs');
    for (const line of convert(target, opt('--to'))) console.log(line);
  } else if (cmd === 'preview') {
    const { preview } = await import('./display/preview.mjs');
    preview(target, +opt('--port', 4173));
  } else throw new Error(USAGE);
} catch (e) {
  console.error(`molty-display: ${e.message}`);
  process.exit(1);
}
