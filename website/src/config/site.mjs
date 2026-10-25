// The ONE place for site-wide identity. Plain .mjs so astro.config.mjs, the Node scripts
// and the React components all import the same values.
//
// REPO_URL is a placeholder: the public GitHub owner/name is not decided yet. Change it
// here and every link (header, footer, docs "source" notes, llms.txt) follows.
export const SITE_URL = 'https://molty.uno';
export const REPO_URL = 'https://github.com/apresmoi/molty.uno';
export const REPO_BRANCH = 'main';
export const SITE_NAME = 'Molty';
export const SITE_TAGLINE = 'A calm creature of characters for your desk. Your agent is its brain.';
export const SITE_DESCRIPTION =
  'Molty is an open-source desk device: a round screen of flowing braille characters where small creatures emerge, dance and read. Pair any AI agent (Claude, Grok, OpenClaw) as its brain with one message.';
/** three.js for the live fluid stage, from the CDN (same pin as the console). */
export const THREE_CDN = 'https://cdn.jsdelivr.net/npm/three@0.170.0/build/three.module.js';
export const repoFile = (path) => `${REPO_URL}/blob/${REPO_BRANCH}/${path}`;
