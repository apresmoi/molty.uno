// Generates dist/llms.txt (index), dist/llms-full.txt (all docs) and raw dist/docs/<slug>.md
// copies. Run after `astro build`. Reads src/content/docs, including the synced repo docs
// (whose route comes from their `slug` frontmatter).
import { readFileSync, writeFileSync, readdirSync, statSync, mkdirSync } from 'node:fs';
import { join, relative, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { SITE_URL, REPO_URL, SITE_DESCRIPTION } from '../src/config/site.mjs';

const DOCS_DIR = fileURLToPath(new URL('../src/content/docs', import.meta.url));
const DIST_DIR = fileURLToPath(new URL('../dist', import.meta.url));

const walk = (dir) =>
  readdirSync(dir).flatMap((entry) => {
    const full = join(dir, entry);
    if (statSync(full).isDirectory()) return walk(full);
    return /\.mdx?$/.test(entry) ? [full] : [];
  });

function frontmatter(content) {
  const match = content.match(/^---\n([\s\S]*?)\n---/);
  const fm = {};
  for (const line of match?.[1].split('\n') ?? []) {
    const i = line.indexOf(':');
    if (i < 0) continue;
    let value = line.slice(i + 1).trim();
    if (value.startsWith('"')) value = JSON.parse(value);
    fm[line.slice(0, i).trim()] = value;
  }
  return fm;
}

const pages = walk(DOCS_DIR)
  .map((file) => {
    const content = readFileSync(file, 'utf8');
    const fm = frontmatter(content);
    const slug = fm.slug ?? relative(DOCS_DIR, file).replace(/\.mdx?$/, '').replace(/\/index$/, '');
    const body = content.replace(/^---\n[\s\S]*?\n---\n*/, '').replace(/<!--[\s\S]*?-->\n*/g, '');
    return { slug, title: fm.title || slug, description: fm.description || '', body };
  })
  .sort((a, b) => a.slug.localeCompare(b.slug));

for (const page of pages) {
  const out = join(DIST_DIR, 'docs', `${page.slug}.md`);
  mkdirSync(dirname(out), { recursive: true });
  writeFileSync(out, `# ${page.title}\n\n${page.body.trim()}\n`);
}

const index = [
  '# Molty',
  '',
  `> ${SITE_DESCRIPTION} Open source, MIT licensed.`,
  '',
  '## Documentation',
  '',
  ...pages.map((p) => `- [${p.title}](${SITE_URL}/docs/${p.slug}.md)${p.description ? `: ${p.description}` : ''}`),
  '',
  '## Source',
  '',
  `- GitHub: ${REPO_URL}`,
  '- License: MIT',
  '',
];
writeFileSync(join(DIST_DIR, 'llms.txt'), index.join('\n'));

const full = ['# Molty — full documentation', '', `> ${SITE_DESCRIPTION}`, ''];
for (const p of pages) full.push('---', '', `# ${p.title}`, '', p.body.trim(), '', '');
writeFileSync(join(DIST_DIR, 'llms-full.txt'), full.join('\n'));

console.log(`Generated llms.txt (${pages.length} pages), llms-full.txt and raw docs/*.md`);
