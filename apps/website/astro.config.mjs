// @ts-check
import { fileURLToPath } from 'node:url';
import { defineConfig } from 'astro/config';
import starlight from '@astrojs/starlight';
import sitemap from '@astrojs/sitemap';
import react from '@astrojs/react';
import { existsSync } from 'node:fs';
import { analyticsHead } from './src/config/analytics.mjs';
import { SYNCED_DOCS } from './src/config/docs.mjs';
import { SITE_URL, SITE_NAME, SITE_DESCRIPTION, REPO_URL, REPO_BRANCH } from './src/config/site.mjs';

/** @param {string} path */
const repoPath = (path) => fileURLToPath(new URL(path, import.meta.url));

/** An optional synced doc (e.g. docs/PORTING.md, landing on another branch) is listed only
 *  once its source exists, so the sidebar never points at a page that was not built. */
/** @param {{ source: string }} doc */
const syncedExists = (doc) => existsSync(repoPath(`../../${doc.source}`));

export default defineConfig({
  site: SITE_URL,
  devToolbar: { enabled: false },
  vite: {
    resolve: {
      dedupe: ['react', 'react-dom'],
      // The firmware's renderer (built by `make web-molty`) and the packages it plays, from the
      // repo: imported, never copied.
      alias: [
        { find: '@molty-wasm', replacement: repoPath('../../device/firmware/web/built') },
        { find: '@packages', replacement: repoPath('../../packages') },
      ],
    },
    server: { fs: { allow: ['../..'] } },
    // the fluid worker lazy-loads glyphcss's atlas payloads: workers must be ES modules
    worker: { format: 'es' },
    // The module, the packages and the still ship as real files (cacheable), not data URIs.
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
            { label: 'Moltys and the viewers', slug: 'moltys/overview' },
            { label: 'Make your own', slug: 'moltys/making-your-own' },
            { label: 'The .molty format', slug: 'moltys/format' },
            { label: 'Creature packages', slug: 'moltys/creature-packages' },
            { label: 'Packages and scenes', slug: 'moltys/packages' },
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
