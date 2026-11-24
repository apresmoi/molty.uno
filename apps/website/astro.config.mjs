// @ts-check
import { fileURLToPath } from 'node:url';
import { defineConfig } from 'astro/config';
import starlight from '@astrojs/starlight';
import sitemap from '@astrojs/sitemap';
import react from '@astrojs/react';
import { existsSync } from 'node:fs';
import { analyticsHead } from './src/config/analytics.mjs';
import { SYNCED_DOCS, DOC_REDIRECTS } from './src/config/docs.mjs';
import { remarkGlyphDiagrams } from './src/integrations/markdown/glyphDiagrams.ts';
import { islandPreload } from './src/integrations/astro/islandPreload.ts';
import { SITE_URL, SITE_NAME, SITE_DESCRIPTION, REPO_URL, REPO_BRANCH } from './src/config/site.mjs';

/** @param {string} path */
const repoPath = (path) => fileURLToPath(new URL(path, import.meta.url));

/** An optional synced doc (e.g. docs/PORTING.md, landing on another branch) is listed only
 *  once its source exists, so the sidebar never points at a page that was not built. */
/** @param {{ source: string }} doc */
const syncedExists = (doc) => existsSync(repoPath(`../../${doc.source}`));

/** The molty-astro skill's sky (skills/molty-astro/scripts/sky.mjs) in the browser, for the
 *  simulator's scenes: it reads its vendored astronomy-engine (CommonJS) through node:module's
 *  createRequire. For that one importer only, node:module is a module that hands it the same file,
 *  run once with an exports object. Nothing of the skill is copied. */
const SKY = repoPath('../../skills/molty-astro/scripts/sky.mjs');
const SKY_REQUIRE = '\0molty-sky-require';
/** @type {{ name: string, enforce: 'pre', resolveId: (id: string, importer?: string) => string | null, load: (id: string) => string | null }} */
const skyInBrowser = {
  name: 'molty-sky-in-browser',
  enforce: 'pre',
  resolveId: (id, importer) => (id === 'node:module' && importer?.split('?')[0] === SKY ? SKY_REQUIRE : null),
  load: (id) =>
    id === SKY_REQUIRE
      ? `import src from ${JSON.stringify(repoPath('../../skills/molty-astro/scripts/vendor/astronomy.min.js') + '?raw')};
const exports = {};
new Function('exports', 'module', src)(exports, { exports });
export const createRequire = () => () => exports;`
      : null,
};

export default defineConfig({
  site: SITE_URL,
  redirects: DOC_REDIRECTS,
  // ```mermaid blocks in the docs are drawn at build time by @glyphcss/diagrams (no script ships for them)
  markdown: { remarkPlugins: [remarkGlyphDiagrams] },
  devToolbar: { enabled: false },
  vite: {
    resolve: {
      dedupe: ['react', 'react-dom'],
      // The firmware's renderer (built by `make web-molty`) and the packages it plays, from the
      // repo: imported, never copied.
      alias: [
        { find: '@molty-wasm', replacement: repoPath('../../device/firmware/web/built') },
        { find: '@packages', replacement: repoPath('../../packages') },
        { find: '@skills', replacement: repoPath('../../skills') },
      ],
    },
    plugins: [skyInBrowser],
    server: { fs: { allow: ['../..'] } },
    // The module, the packages and the still ship as real files (cacheable), not data URIs.
    build: { assetsInlineLimit: 0 },
  },
  integrations: [
    react(),
    // the islands that hydrate at once: their modules preloaded from the page's head
    islandPreload(),
    // the demo (/demo/), the device simulator: not listed yet
    sitemap({ filter: (page) => !page.includes('/demo/') }),
    starlight({
      title: SITE_NAME,
      description: SITE_DESCRIPTION,
      favicon: '/favicon.svg',
      head: [
        ...(process.env.NODE_ENV === 'production' ? analyticsHead : []),
        { tag: 'meta', attrs: { property: 'og:image', content: `${SITE_URL}/og.png` } },
        { tag: 'meta', attrs: { name: 'twitter:image', content: `${SITE_URL}/og.png` } },
        { tag: 'meta', attrs: { name: 'twitter:card', content: 'summary_large_image' } },
      ],
      components: {
        Header: './src/integrations/starlight/Header.astro',
        SiteTitle: './src/integrations/starlight/SiteTitle.astro',
      },
      social: [{ icon: 'github', label: 'GitHub', href: REPO_URL }],
      editLink: { baseUrl: `${REPO_URL}/edit/${REPO_BRANCH}/apps/website/` },
      customCss: ['./src/integrations/starlight/starlight.css'],
      // Same order as the landing: fluid · moltys · bridge · boards.
      sidebar: [
        {
          label: 'Start',
          items: [
            { label: 'Introduction', slug: 'getting-started/introduction' },
            { label: 'Flash a release', slug: 'getting-started/flash-a-release' },
            { label: 'Set up your Molty', slug: 'getting-started/setup' },
            { label: 'What Molty shows', slug: 'getting-started/on-screen' },
            { label: 'Porting to your board', slug: 'getting-started/porting' },
          ],
        },
        {
          label: 'Fluid',
          items: [{ label: 'How it renders', slug: 'architecture' }],
        },
        {
          label: 'Moltys',
          items: [
            { label: 'The Molty format', slug: 'moltys/molty-format' },
            { label: 'Draw on Molty', slug: 'moltys/draw' },
            { label: 'Packages and scenes', slug: 'moltys/packages' },
            { label: 'Creature packages', slug: 'moltys/creature-packages' },
          ],
        },
        {
          label: 'Bridge',
          items: [
            { label: 'Connect an agent', slug: 'agents/connect' },
            { label: 'Protocol: pairing, REST, MCP', slug: 'agents/protocol' },
            { label: 'CLI and local MCP', slug: 'agents/cli' },
          ],
        },
        {
          label: 'Boards',
          items: [
            { label: 'Build and flash', slug: 'getting-started/hardware' },
            ...SYNCED_DOCS.filter((d) => d.optional && syncedExists(d)).map((d) => ({ label: d.title, slug: d.slug })),
          ],
        },
        {
          label: 'Project',
          items: [{ label: 'Contributing', slug: 'contributing' }],
        },
      ],
    }),
  ],
});
