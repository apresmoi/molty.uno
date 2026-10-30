#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// version.mjs — the root VERSION file is Molty's one product version. Everything
// else reads it at build time (firmware PROJECT_VER, console __MOLTY_VERSION__,
// cli -ldflags, relay --define); only the two private package.json "version"
// fields and the CHANGELOG headings are copies, and `check` keeps them honest.
//
//   node tools/scripts/version.mjs                       print VERSION
//   node tools/scripts/version.mjs check [--tag vX.Y.Z]  copies agree (+ tag matches, CHANGELOG has the section)
//   node tools/scripts/version.mjs bump <patch|minor|major|X.Y.Z>
//                                                  write VERSION + package.json copies, and turn
//                                                  CHANGELOG "## Unreleased" into "## vX.Y.Z — <date>"
//   node tools/scripts/version.mjs notes [X.Y.Z]         print that CHANGELOG section (release notes)
import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '../..');
const at = (p) => join(ROOT, p);
const SEMVER = /^(\d+)\.(\d+)\.(\d+)(?:-[0-9A-Za-z.-]+)?$/;
const PACKAGES = ['device/console/package.json', 'services/relay/package.json'];

const die = (msg) => { console.error(`version: ${msg}`); process.exit(1); };
const read = () => {
  const v = readFileSync(at('VERSION'), 'utf8').trim();
  if (!SEMVER.test(v)) die(`VERSION "${v}" is not X.Y.Z[-pre]`);
  return v;
};
const escape = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
const heading = (v) => new RegExp(`^## v${escape(v)}(?: |$)`, 'm');

/** The body of "## v<version> …" up to the next "## " heading, trimmed. */
function section(version) {
  const text = readFileSync(at('CHANGELOG.md'), 'utf8');
  const m = heading(version).exec(text);
  if (!m) return null;
  const rest = text.slice(m.index + m[0].length);
  const body = rest.slice(rest.indexOf('\n') + 1);
  const next = body.search(/^## /m);
  return (next < 0 ? body : body.slice(0, next)).trim();
}

const [cmd = 'print', ...args] = process.argv.slice(2);

if (cmd === 'print') {
  console.log(read());
} else if (cmd === 'check') {
  const v = read();
  const problems = [];
  for (const p of PACKAGES) {
    const pv = JSON.parse(readFileSync(at(p), 'utf8')).version;
    if (pv !== v) problems.push(`${p} version ${pv} != VERSION ${v} (run: node tools/scripts/version.mjs bump ${v})`);
  }
  const tagAt = args.indexOf('--tag');
  if (tagAt >= 0) {
    const tag = args[tagAt + 1] ?? '';
    if (!/^v/.test(tag) || !SEMVER.test(tag.slice(1))) problems.push(`tag "${tag}" is not vX.Y.Z[-pre]`);
    else if (tag.slice(1) !== v) problems.push(`tag ${tag} does not match VERSION ${v}`);
    const notes = section(v);
    if (!notes) problems.push(`CHANGELOG.md has no non-empty "## v${v}" section`);
  }
  if (problems.length) die(problems.join('\n  '));
  console.log(`version ${v} ok`);
} else if (cmd === 'bump') {
  const cur = read();
  const how = args[0] ?? die('bump needs patch|minor|major|X.Y.Z');
  let next;
  if (SEMVER.test(how)) next = how;
  else {
    const [maj, min, pat] = cur.match(SEMVER).slice(1, 4).map(Number);
    next = { major: `${maj + 1}.0.0`, minor: `${maj}.${min + 1}.0`, patch: `${maj}.${min}.${pat + 1}` }[how];
    if (!next) die(`unknown bump "${how}"`);
  }
  // Validate everything before writing anything.
  if (!SEMVER.test(next)) die(`"${next}" is not X.Y.Z[-pre]`);
  let log = readFileSync(at('CHANGELOG.md'), 'utf8');
  if (!heading(next).test(log)) {
    const m = /^## Unreleased[ \t]*$/m.exec(log);
    if (!m) die('CHANGELOG.md has no "## Unreleased" heading to release');
    const after = log.slice(m.index + m[0].length);
    const end = after.search(/^## /m);
    if (!(end < 0 ? after : after.slice(0, end)).trim()) die('CHANGELOG.md "## Unreleased" is empty; write the release notes first');
    const date = new Date().toISOString().slice(0, 10);
    log = log.slice(0, m.index) + `## Unreleased\n\n## v${next} — ${date}` + after;
  }
  writeFileSync(at('CHANGELOG.md'), log);
  writeFileSync(at('VERSION'), `${next}\n`);
  for (const p of PACKAGES) {
    const src = readFileSync(at(p), 'utf8');
    writeFileSync(at(p), src.replace(/("version":\s*")[^"]*(")/, `$1${next}$2`));
  }
  console.log(`${cur} -> ${next}: VERSION, ${PACKAGES.join(', ')}, CHANGELOG.md`);
} else if (cmd === 'notes') {
  const v = args[0] ?? read();
  const notes = section(v);
  if (!notes) die(`CHANGELOG.md has no non-empty "## v${v}" section`);
  console.log(notes);
} else {
  die(`unknown command "${cmd}"`);
}
