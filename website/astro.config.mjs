// @ts-check
import { fileURLToPath } from 'node:url';
import { defineConfig } from 'astro/config';
import starlight from '@astrojs/starlight';
import sitemap from '@astrojs/sitemap';
import react from '@astrojs/react';
import { existsSync } from 'node:fs';
import { resolve as resolvePath } from 'node:path';
import { analyticsHead } from './src/config/analytics.mjs';
import { SYNCED_DOCS } from './src/config/docs.mjs';
import { SITE_URL, SITE_NAME, SITE_DESCRIPTION, REPO_URL, REPO_BRANCH, THREE_CDN } from './src/config/site.mjs';

/** @param {string} path */
const repoPath = (path) => fileURLToPath(new URL(path, import.meta.url));

/** An optional synced doc (e.g. docs/PORTING.md, landing on another branch) is listed only
 *  once its source exists, so the sidebar never points at a page that was not built. */
/** @param {{ source: string }} doc */
const syncedExists = (doc) => existsSync(repoPath(`../${doc.source}`));

/** LOCAL-ONLY until @glyphcss/fluid (and the glyphcss with the braille-dots atlas) is on npm:
 *  the fluid packages are consumed as source from a glyphcss checkout, as glyphcss's own
 *  website does. GLYPHCSS_DIR overrides the checkout (see README.md). */
const GLYPHCSS_DIR = resolvePath(process.env.GLYPHCSS_DIR ?? '/Users/apresmoi/Documents/molty.uno/glyphcss-fluid');
/** @param {string} path */
const glyphcssPath = (path) => resolvePath(GLYPHCSS_DIR, 'packages', path);
const glyphcssAliases = [
  { find: /^@glyphcss\/core$/, replacement: glyphcssPath('core/src/index.ts') },
  { find: /^@glyphcss\/fluid\/reader$/, replacement: glyphcssPath('fluid/src/reader/index.ts') },
  // for `@glyphcss/fluid/worker?worker`: the package's default `new URL("./worker.js")` only
  // exists next to its built dist, so the site hands it a Vite-bundled worker instead
  { find: /^@glyphcss\/fluid\/worker(?=$|\?)/, replacement: glyphcssPath('fluid/src/runtime/worker.ts') },
  { find: /^@glyphcss\/fluid$/, replacement: glyphcssPath('fluid/src/index.ts') },
  { find: /^glyphcss$/, replacement: glyphcssPath('glyphcss/src/index.ts') },
];

/** The creature engine imports bare `three`; like console/vite.config.ts, it comes from the
 *  CDN at runtime instead of being bundled (~600 KB). */
const threeFromCdn = () => ({
  name: 'molty-three-cdn',
  enforce: /** @type {const} */ ('pre'),
  /** @param {string} id */
  resolveId: (id) => (id === 'three' ? { id: THREE_CDN, external: true } : null),
});

export default defineConfig({
  site: SITE_URL,
  devToolbar: { enabled: false },
  vite: {
    plugins: [threeFromCdn()],
    resolve: {
      dedupe: ['react', 'react-dom'],
      // The REAL engine and assets from the repo, imported, never copied (as console/ does).
      alias: [
        { find: '@creatures', replacement: repoPath('../creatures/js') },
        { find: '@molty-files', replacement: repoPath('../creatures/molty') },
        { find: '@molty-catalog', replacement: repoPath('../console/src/catalog') },
        ...glyphcssAliases,
      ],
    },
    server: { fs: { allow: ['..', GLYPHCSS_DIR] } },
    // the fluid worker lazy-loads glyphcss's atlas payloads: workers must be ES modules
    worker: { format: 'es' },
    // Thumbnails and .molty files ship as real files (downloadable, cacheable), not data URIs.
    build: { assetsInlineLimit: 0 },
  },
  integrations: [
    react(),
    sitemap(),
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
      editLink: { baseUrl: `${REPO_URL}/edit/${REPO_BRANCH}/website/` },
      customCss: ['./src/integrations/starlight/starlight.css'],
      // Same order as the landing: fluid · moltys · bridge · boards.
      sidebar: [
        {
          label: 'Start',
          items: [
            { label: 'Introduction', slug: 'getting-started/introduction' },
            { label: 'Flash a release', slug: 'getting-started/flash-a-release' },
            { label: 'Set up your Molty', slug: 'getting-started/setup' },
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
            { label: 'Moltys and the viewers', slug: 'moltys/overview' },
            { label: 'Make your own', slug: 'moltys/making-your-own' },
            { label: 'The .molty format', slug: 'moltys/format' },
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
