// Sync the repo's own docs into Starlight (the repo files stay the source of truth).
//
// For each entry in src/config/docs.mjs: read the repo file, drop its leading H1 (the
// title comes from the config), rewrite repo-relative links (a synced doc → its site
// route; anything else → the file on GitHub), and write
// src/content/docs/repo/<slug>.md with `slug` + `editUrl` frontmatter. The output folder
// is gitignored and rebuilt on every `pnpm dev` / `pnpm build`.
import { readFileSync, writeFileSync, mkdirSync, rmSync, existsSync, statSync } from 'node:fs';
import { dirname, join, normalize, relative } from 'node:path';
import { fileURLToPath } from 'node:url';
import { SYNCED_DOCS } from '../src/config/docs.mjs';
import { REPO_URL, REPO_BRANCH } from '../src/config/site.mjs';

const ROOT = fileURLToPath(new URL('../../', import.meta.url));
const OUT = fileURLToPath(new URL('../src/content/docs/repo/', import.meta.url));
const bySource = new Map(SYNCED_DOCS.map((d) => [d.source, d]));

const yaml = (s) => JSON.stringify(s); // JSON strings are valid YAML scalars

function rewriteLink(target, fromSource) {
  if (/^([a-z]+:|#|\/)/i.test(target)) return target;
  const [path, hash = ''] = target.split('#');
  const repoPath = normalize(join(dirname(fromSource), path)).replace(/\\/g, '/');
  const anchor = hash ? `#${hash}` : '';
  const synced = bySource.get(repoPath);
  if (synced) return `/${synced.slug}/${anchor}`;
  const abs = join(ROOT, repoPath);
  const kind = existsSync(abs) && statSync(abs).isDirectory() ? 'tree' : 'blob';
  return `${REPO_URL}/${kind}/${REPO_BRANCH}/${repoPath}${anchor}`;
}

function transform(markdown, source) {
  const lines = markdown.split('\n');
  if (/^# /.test(lines[0] ?? '')) lines.shift();
  while (lines.length && lines[0].trim() === '') lines.shift();
  let fenced = false;
  return lines
    .map((line) => {
      if (/^\s*(```|~~~)/.test(line)) fenced = !fenced;
      if (fenced) return line;
      return line.replace(/(\]\()([^)\s]+)(\))/g, (_, a, target, b) => a + rewriteLink(target, source) + b);
    })
    .join('\n');
}

rmSync(OUT, { recursive: true, force: true });
mkdirSync(OUT, { recursive: true });
for (const doc of SYNCED_DOCS) {
  const body = transform(readFileSync(join(ROOT, doc.source), 'utf8'), doc.source);
  const front = [
    '---',
    `title: ${yaml(doc.title)}`,
    `description: ${yaml(doc.description)}`,
    `slug: ${yaml(doc.slug)}`,
    `editUrl: ${yaml(`${REPO_URL}/edit/${REPO_BRANCH}/${doc.source}`)}`,
    '---',
    '',
    `<!-- Generated from ${doc.source} by website/scripts/sync-docs.mjs. Edit the source, not this file. -->`,
    '',
  ].join('\n');
  const file = join(OUT, doc.slug.replace(/\//g, '--') + '.md');
  writeFileSync(file, front + body);
}
console.log(`synced ${SYNCED_DOCS.length} repo docs → website/${relative(join(ROOT, 'website'), OUT)}`);
