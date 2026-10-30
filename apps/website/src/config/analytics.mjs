// Analytics is configured but EMPTY: no provider is chosen for molty.uno yet. When one is,
// fill these in; Analytics.astro and astro.config.mjs only emit them in production builds.
export const ANALYTICS_SRC = '';
export const ANALYTICS_INIT = '';
export const analyticsHead = ANALYTICS_SRC
  ? [
      { tag: 'script', attrs: { async: true, src: ANALYTICS_SRC } },
      { tag: 'script', content: ANALYTICS_INIT },
    ]
  : [];
