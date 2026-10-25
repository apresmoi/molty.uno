// @ts-check
import { fileURLToPath } from 'node:url';
import { defineConfig } from 'astro/config';
import starlight from '@astrojs/starlight';
import sitemap from '@astrojs/sitemap';
import react from '@astrojs/react';
import { analyticsHead } from './src/config/analytics.mjs';
import { SITE_URL, SITE_NAME, SITE_DESCRIPTION, REPO_URL, REPO_BRANCH, THREE_CDN } from './src/config/site.mjs';

/** @param {string} path */
const repoPath = (path) => fileURLToPath(new URL(path, import.meta.url));

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
      alias: {
        '@creatures': repoPath('../creatures/js'),
        '@molty-files': repoPath('../creatures/molty'),
        '@molty-catalog': repoPath('../console/src/catalog'),
      },
    },
    server: { fs: { allow: ['..'] } },
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
        ThemeSelect: './src/integrations/starlight/ThemeSelect.astro',
        ThemeProvider: './src/integrations/starlight/ThemeProvider.astro',
        SiteTitle: './src/integrations/starlight/SiteTitle.astro',
      },
      social: [{ icon: 'github', label: 'GitHub', href: REPO_URL }],
      editLink: { baseUrl: `${REPO_URL}/edit/${REPO_BRANCH}/website/` },
      customCss: ['./src/integrations/starlight/starlight.css'],
      sidebar: [
        {
          label: 'Getting started',
          items: [
            { label: 'Introduction', slug: 'getting-started/introduction' },
            { label: 'Hardware, build and flash', slug: 'getting-started/hardware' },
            { label: 'Flash a release', slug: 'getting-started/flash-a-release' },
            { label: 'Set up your Molty', slug: 'getting-started/setup' },
          ],
        },
        {
          label: 'Agents',
          items: [
            { label: 'Connect an agent', slug: 'agents/connect' },
            { label: 'Protocol: pairing, REST, MCP', slug: 'agents/protocol' },
            { label: 'CLI and local MCP', slug: 'agents/cli' },
          ],
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
          label: 'Project',
          items: [
            { label: 'Architecture', slug: 'architecture' },
            { label: 'Contributing', slug: 'contributing' },
          ],
        },
      ],
    }),
  ],
});
