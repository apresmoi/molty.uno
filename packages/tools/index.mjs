// SPDX-License-Identifier: MIT
// One packages/index.json entry (docs/PACKAGES.md section 1), shared by every writer and checker:
// the file's facts, its one-line pitch (packages/<id>/description.txt), and for a scenes package
// its layouts and the skills that play it (skills/*/molty.json "packages").
import { createHash } from 'node:crypto';
import { existsSync, readFileSync, readdirSync } from 'node:fs';

const ROOT = new URL('../../', import.meta.url);
/** What an agent reads to pitch a package to its human: one line of printable ASCII. */
export const DESCRIPTION_RE = /^[\x20-\x7e]{1,160}$/;

export const description = (id) => {
  const f = new URL(`packages/${id}/description.txt`, ROOT);
  return existsSync(f) ? readFileSync(f, 'utf8').trim() : '';
};

/** Skills whose molty.json sidecar installs this package, by directory name. */
export const skillsFor = (id) => readdirSync(new URL('skills/', ROOT), { withFileTypes: true })
  .filter((e) => e.isDirectory() && existsSync(new URL(`skills/${e.name}/molty.json`, ROOT)))
  .filter((e) => (JSON.parse(readFileSync(new URL(`skills/${e.name}/molty.json`, ROOT), 'utf8')).packages ?? []).includes(id))
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
