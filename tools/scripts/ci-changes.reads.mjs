#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// ci-changes.reads.mjs: the files outside its own folder that each CI job reads, found from its code, so
// ci-changes.test.sh can check that every one of them triggers that job in ci-changes.sh.
// Prints `<job> <path>` lines. What it follows, from each job's entry points: relative and aliased imports
// (import ... from, import(), new URL('...', import.meta.url), resolve(HERE, '...')) of files, the website
// tests' quoted repo paths ('packages/cat/cat.json'), the files the firmware host tests hand to node and C,
// the creature packages the firmware image bakes (packages/index.json) and the sources of the website's
// Molty (device/firmware/web/built/molty.json). Folder reads (the website's docs and skills markdown, the
// packages the host tests list) and CMake/Go reads are not followed: ci-changes.test.sh spells those out.
import { execFileSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { dirname, join, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const TRACKED = new Set(execFileSync('git', ['ls-files'], { cwd: ROOT, encoding: 'utf8' }).split('\n').filter(Boolean));
const TOP = [...new Set([...TRACKED].filter((f) => f.includes('/')).map((f) => f.split('/')[0]))];
const ALIAS = { '@viewer': 'device/creatures/js', '@packages': 'packages', '@skills': 'skills', '@molty-wasm': 'device/firmware/web/built' };
const rel = (abs) => relative(ROOT, abs);
const under = (dir) => [...TRACKED].filter((f) => f.startsWith(dir + '/') || f === dir);

/** Every tracked file reachable from `entries` (repo paths, files or folders); `literals`: quoted repo paths too. */
function reads(entries, literals = false) {
  const seen = new Set(), queue = entries.flatMap(under);
  const add = (from, spec) => {
    spec = spec.split('?')[0];
    for (const [a, p] of Object.entries(ALIAS)) if (spec === a || spec.startsWith(a + '/')) spec = join(ROOT, p) + spec.slice(a.length);
    if (!spec.startsWith('.') && !spec.startsWith('/')) return;
    const abs = resolve(dirname(join(ROOT, from)), spec);
    const tries = [abs, abs.replace(/\.js$/, '.ts'), ...['.ts', '.tsx', '.js', '.mjs', '.astro'].map((e) => abs + e)];
    const hit = tries.map(rel).find((f) => TRACKED.has(f));
    if (hit) queue.push(hit);
  };
  while (queue.length) {
    const f = queue.pop();
    if (seen.has(f)) continue;
    seen.add(f);
    if (!/\.(m?js|cjs|ts|tsx|astro)$/.test(f)) continue;
    const src = readFileSync(join(ROOT, f), 'utf8');
    for (const m of src.matchAll(/(?:\bfrom|\bimport\s*\(|\bimport)\s*['"]([^'"\n]+)['"]/g)) add(f, m[1]);
    for (const m of src.matchAll(/new URL\(\s*['"]([^'"$\n]+)['"]\s*,\s*import\.meta\.url/g)) add(f, m[1]);
    for (const m of src.matchAll(/resolve\(HERE,\s*['"]([^'"\n]+)['"]\)/g)) add(f, m[1]);
    if (literals) for (const m of src.matchAll(/['"]([A-Za-z0-9_.-]+\/[A-Za-z0-9_./-]+)['"]/g)) if (TOP.includes(m[1].split('/')[0]) && TRACKED.has(m[1])) queue.push(m[1]);
  }
  return [...seen];
}

/** What the firmware host test scripts and C tests name under the repo root. */
function hostTestEntries() {
  const dir = 'device/firmware/test/host', out = [];
  for (const f of under(dir)) {
    const src = readFileSync(join(ROOT, f), 'utf8');
    if (f.endsWith('.sh')) {
      for (const m of src.matchAll(/\$(?:REPO|ROOT)\/([A-Za-z0-9_./-]+)/g)) out.push(m[1]);
      for (const m of src.matchAll(/\$HERE\/((?:\.\.\/)+[A-Za-z0-9_./-]+)/g)) out.push(rel(resolve(ROOT, dir, m[1])));
    } else if (f.endsWith('.c')) {
      for (const m of src.matchAll(/"%s\/([A-Za-z0-9_./-]+)"/g)) out.push(m[1]);
    }
  }
  return [...new Set(out)].filter((p) => TRACKED.has(p));
}

const index = JSON.parse(readFileSync(join(ROOT, 'packages/index.json'), 'utf8'));
const web = JSON.parse(readFileSync(join(ROOT, 'device/firmware/web/built/molty.json'), 'utf8'));
const console_ = reads(['device/console/src', 'device/console/tools', 'device/console/vite.config.ts']);
const jobs = {
  console: console_,
  console_e2e: console_,
  creatures: reads(['device/creatures/tools/molty-export.mjs']),
  firmware: ['packages/index.json', ...Object.values(index).filter((p) => p.kind === 'creature').map((p) => p.path)],
  firmware_test: reads(hostTestEntries()),
  // what CI's website job runs: pnpm check, test and build (sync-docs, astro build, llms.txt)
  website: [...reads(['apps/website/src', 'apps/website/astro.config.mjs', 'apps/website/scripts/checks', 'apps/website/scripts/tests',
    'apps/website/scripts/sync-docs.mjs', 'apps/website/scripts/generate-llms-txt.mjs'], true), ...web.sources.files, web.fidelity.script],
  relay: reads(['services/relay/src', 'services/relay/test', 'services/relay/tools', 'services/relay/vitest.config.ts']),
  // tools/sim/tests/test_reflex_turns.py runs the relay's reflex-turns tool under node
  python: reads(['services/relay/tools/reflex-turns.ts', 'services/relay/tools/ts-resolve.mjs']),
};
for (const [job, files] of Object.entries(jobs)) {
  for (const f of [...new Set(files)].sort()) if (TRACKED.has(f)) console.log(`${job} ${f}`);
}
