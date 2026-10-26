# molty.UNO website

The landing page and the docs (Astro + Starlight + React islands). Agent guide: `AGENTS.md`.

```sh
pnpm install
pnpm check
pnpm build && pnpm preview
```

## The fluid: a local glyphcss checkout (temporary)

The page fluid, the Moltys and the speed reader are drawn by `@glyphcss/fluid`, which needs a
`glyphcss` with the braille-dots atlas. Neither is published to npm yet, so the site consumes
their **source** from a glyphcss checkout through Vite aliases in `astro.config.mjs`
(`@glyphcss/fluid`, `@glyphcss/fluid/reader`, `@glyphcss/fluid/worker`, `@glyphcss/core`,
`glyphcss` → `<GLYPHCSS_DIR>/packages/*/src`):

```sh
GLYPHCSS_DIR=/path/to/glyphcss pnpm build   # default: the maintainer's local checkout
```

This is a local-only setup: a build without that checkout fails. Once the packages are on npm,
replace the aliases with `@glyphcss/fluid` and `glyphcss` dependencies.
