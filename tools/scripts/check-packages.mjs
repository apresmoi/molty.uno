#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// check-packages.mjs: every package under packages/ against docs/PACKAGES.md (run by hygiene.sh):
//   1. packages/index.json lists exactly the packages/<id>/<id>.json files, with their true size and sha256
//   2. each file is at most 256 KB and its build is current (build.mjs reproduces it byte for byte)
//   3. schema: top-level fields, assets (sizes, encodings, palettes), layouts, items, params, and
//      every reference resolves ($param declared and type-compatible, @field only inside each, assets exist)
// --write-index rewrites packages/index.json from the files (after a build). Exits non-zero listing every problem.
import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { existsSync, readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { MAX_PKG_BYTES, NAME_RE, checkPackage } from '../../packages/tools/format.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const PKGS = join(ROOT, 'packages');
const INDEX = join(PKGS, 'index.json');
const problems = [];
const bad = (m) => problems.push(m);

const ids = readdirSync(PKGS, { withFileTypes: true }).filter((e) => e.isDirectory() && existsSync(join(PKGS, e.name, `${e.name}.json`))).map((e) => e.name).sort();
const entries = {};
for (const id of ids) {
  const rel = `packages/${id}/${id}.json`, bytes = readFileSync(join(ROOT, rel));
  if (!NAME_RE.test(id)) bad(`${rel}: the id must be [a-z0-9-]{1,32}`);
  if (bytes.length > MAX_PKG_BYTES) bad(`${rel}: ${bytes.length} bytes, over the ${MAX_PKG_BYTES} byte limit`);
  let pkg;
  try { pkg = JSON.parse(bytes); } catch (e) { bad(`${rel}: ${e.message}`); continue; }
  for (const p of checkPackage(pkg, id)) bad(`${rel}: ${p}`);
  const build = join(PKGS, id, 'build.mjs');
  if (existsSync(build)) {
    execFileSync(process.execPath, [build], { stdio: 'ignore' });
    if (!readFileSync(join(ROOT, rel)).equals(bytes)) { bad(`${rel} is stale: run node ${`packages/${id}/build.mjs`} and commit the output`); writeFileSync(join(ROOT, rel), bytes); }
  }
  entries[id] = { name: pkg.name, version: pkg.version, kind: pkg.kind, path: rel, size: bytes.length, sha256: createHash('sha256').update(bytes).digest('hex') };
}

if (process.argv.includes('--write-index')) writeFileSync(INDEX, JSON.stringify(entries, null, 2) + '\n');
const index = existsSync(INDEX) ? JSON.parse(readFileSync(INDEX, 'utf8')) : {};
for (const id of new Set([...Object.keys(index), ...ids])) {
  if (!entries[id]) { bad(`index.json lists ${id}, which has no packages/${id}/${id}.json`); continue; }
  if (JSON.stringify(index[id]) !== JSON.stringify(entries[id])) bad(`index.json entry for ${id} is out of date (run node tools/scripts/check-packages.mjs --write-index)`);
}

if (problems.length) {
  for (const p of problems) console.log(`::error::packages: ${p}`);
  console.log(`check-packages: FAILED (${problems.length})`);
  process.exit(1);
}
console.log(`check-packages: ok (${ids.map((id) => `${id} ${entries[id].size} B`).join(', ')})`);
