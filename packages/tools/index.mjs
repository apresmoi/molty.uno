// SPDX-License-Identifier: MIT
// One packages/index.json entry (docs/PACKAGES.md section 1), shared by every writer and checker:
// the file's facts, its one-line pitch (packages/<id>/description.txt), and for a scenes package
// its layouts and the skills that play it (the skill folder whose display.yaml builds it, docs/DISPLAY.md;
// or one whose molty.json names it under "packages").
import { createHash } from 'node:crypto';
import { existsSync, readFileSync, readdirSync } from 'node:fs';
import { parse } from './display/load.mjs';

const ROOT = new URL('../../', import.meta.url);
/** What an agent reads to pitch a package to its human: one line of printable ASCII. */
export const DESCRIPTION_RE = /^[\x20-\x7e]{1,160}$/;

export const description = (id) => {
  const f = new URL(`packages/${id}/description.txt`, ROOT);
  return existsSync(f) ? readFileSync(f, 'utf8').trim() : '';
};

/** The packages a skill folder installs: the one its display.yaml builds, and those its molty.json sidecar names. */
function packagesOf(skill) {
  const file = (f) => new URL(`skills/${skill}/${f}`, ROOT), text = (f) => readFileSync(file(f), 'utf8');
  return [...(existsSync(file('display.yaml')) ? [parse(text('display.yaml'), 'display.yaml')?.package?.id] : []), ...(existsSync(file('molty.json')) ? JSON.parse(text('molty.json')).packages ?? [] : [])];
}
/** Skills that install this package, by directory name. */
export const skillsFor = (id) => readdirSync(new URL('skills/', ROOT), { withFileTypes: true })
  .filter((e) => e.isDirectory() && packagesOf(e.name).includes(id))
  .map((e) => e.name).sort();

export function indexEntry(pkg, bytes) {
  const id = pkg.id, skills = skillsFor(id);
  return {
    name: pkg.name, version: pkg.version, kind: pkg.kind, description: description(id),
    ...(pkg.kind === 'scenes' ? { layouts: Object.keys(pkg.layouts ?? {}) } : {}),
    ...(skills.length ? { skills } : {}),
    path: `packages/${id}/${id}.json`, size: bytes.length, sha256: createHash('sha256').update(bytes).digest('hex'),
  };
}
