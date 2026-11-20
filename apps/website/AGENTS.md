# website — agent guide

molty.uno: the landing page (`/`) plus the Starlight docs. Root `AGENTS.md` applies.
Standalone pnpm package, Node 22: `pnpm install && pnpm check && pnpm build` → `dist/`.
Never deploy or publish from here; nothing in this package does.

## Sources of truth

- **Site identity** (`SITE_URL`, `REPO_URL`): `src/config/site.mjs`, and
  only there. `REPO_URL` is a placeholder until the GitHub owner is decided; the check rejects
  `github.com/` anywhere else in `src/`.
- **Repo docs stay in the repo.** `src/config/docs.mjs` lists the synced files (`docs/*.md`,
  `device/creatures/*.md`, `CONTRIBUTING.md`); `scripts/sync-docs.mjs` turns them into
  `src/content/docs/repo/*.md` (gitignored, rebuilt by `dev`/`build`) with `slug` and `editUrl`
  frontmatter. Fix content in the source file, not the copy.
  Hand-written, website-only pages (introduction, flash a release, setup, what Molty shows,
  connect an agent) live in `src/content/docs/`. Addresses of pages that left the site are in
  `DOC_REDIRECTS` (`src/config/docs.mjs`).
- **Firmware for /setup is staged, never committed.** `scripts/stage-firmware.mjs` writes ESP
  Web Tools manifests (one per `device/firmware/boards/**/board.json`) and the images into the
  gitignored `public/firmware/`: `--local` from `../../device/firmware/build/<board>/`, `--release` from
  the latest GitHub release (deploy-website does this), `--none` manifests only. Same origin
  on purpose: release assets have no CORS.
- **The Molty on every screen is the firmware's own renderer, never a second one.**
  `device/firmware/web/` compiles `device/firmware/main/fluid/mole_fluid.c` and what it draws
  with (fluid, creature player, crisp dots, rings, question, scene player) to
  `device/firmware/web/built/molty.wasm` (`make web-molty`, emscripten 7.0.0; committed with its
  manifest, so this package builds with Node alone). `@molty-wasm` → that folder, `@packages` →
  `../../packages` (the cat, the astrology scenes): imported by URL, fetched when a screen nears
  the view. A change to how a Molty looks is a firmware change plus `make web-molty`; nothing
  here draws one. `scripts/tests/molty.test.ts` fails when the firmware sources, a package the
  check plays or the still move on without it.
- **One still.** `src/assets/molty/cat.webp` is a frame of that module (`pnpm molty-still`);
  the test wants it to be exactly that.
- **Diagrams are mermaid source, drawn by glyphcss at build time.** A fenced `mermaid` block in any
  doc (GitHub draws it natively) becomes a text figure through `src/integrations/markdown/glyphDiagrams.ts`
  (`@glyphcss/diagrams`, a remark plugin in `astro.config.mjs`): two renders, 78 and 44 columns,
  and CSS in `starlight.css` shows the one that fits and scales it to its box. No script, font or
  request ships for a diagram. An `accTitle:` line names it for screen readers. A source that loses
  a label or a connection fails the build; `scripts/tests/diagrams.test.ts` checks every block in the
  docs and that no hand-drawn diagram is left. Never paste rendered text into a markdown file.

## Ownership and composition

- `src/pages/` holds thin Astro routes composed from `src/layouts/` (`SiteLayout`,
  `ContentLayout`). No page `<style>` or runtime `<script>` (JSON-LD only).
- `src/components/Name/Name.tsx` is the component; `index.ts` has explicit named exports.
  Import another component through its barrel. Components are React; `.astro` lives only in
  pages, layouts and `src/integrations/{astro,starlight}/` adapters.
- One grain, one water. The page's dust is the motes a creature's world has on the device
  (`creature_dots.c`), on the device's own dot lattice at the size the page's reference screen
  shows a panel's pixel (`--m-screen`, `--m-dust-size` in `tokens.css`): loose everywhere (a
  background tile, there at first paint, no script), denser in focus round each
  `[data-dust-focus]` (a device, a heading: a second tile on the same lattice, masked by
  `PageWater` once it knows where they stand; `src/config/water.mjs` `FOCUS`). The tiles are
  pictures of the water's field at rest (`pnpm dust-tile`).
- The page's water is the firmware's: `device/firmware/web/web_field.c` runs `fluid_sim.c`
  (the wave alone) and `fluid_draw.c` (crisp dots) on a grid as wide as the window, from one
  more instance of the module (`services/molty-screen/field.ts`). `src/services/page-water/`
  is its page side: one canvas fixed to the viewport, drawn only while the water moves and only
  in the cells it moves (a cell at rest is clear: the tile shows), stood again as the page
  scrolls; every tap is a bump in the field. A screen takes the field's water in its rim
  (`screen.ts` `meetPage`, `web_molty.c` `wm_couple`) and gives its own water the page's speed
  in on-screen pixels; a heading's window takes all of its water from the field. One clock
  (`services/molty-screen/clock.ts`): screens first, the field last. Page-side, and only this:
  where the dust is and that a crest brings the denser motes (`web_field.c`), the canvas, the
  taps, `CARRY` (the page's water keeps its waves while a front is out; else it is the
  device's) and `HEADING_GAIN` (`config/water.mjs`). `scripts/tests/water.test.ts` pins the
  numbers to the firmware's sources, wants a front to cross a rim within a few pixels, and
  every heading to loosen under a wave and sharpen back.
- Headings are words the firmware draws: `src/config/words.mjs` holds the letters as lines and
  arcs, `pnpm words` writes them as a scenes package (`src/assets/molty/words.json`), `MoltyWord`
  plays a word's layout on a round window with as many pixels as the page's dust has across it
  (a scene on the braille haze: thin lines in the page's own grain, which a wave loosens and calm
  water sharpens back), `WordHeading`
  sizes the heading to it. A word runs while near the viewport and is put away when far.
- `src/services/molty-screen/` is the only owner of the firmware's renderer. `core.ts` is the
  module's loader and calls (no DOM: the fidelity check drives it under Node), `screen.ts` runs
  one instance on a canvas (the page's frames, at most 60 a second, only in view and with the
  tab shown; a heading's window every other one; the pointer is the finger; reduced motion: one settled still per change),
  `program.ts` is what a screen does by itself. Its calls are the device's own:
  `state` is `POST /api/creature/state`'s body, `ask` the link's question, `scene` scene.play.
  `MoltyScreen` (canvas + still + `Device`) mounts it; `features/molty/` holds the cat, the lists
  read from its package and the side screens' programs; `hooks/useMoltyStage` plays actions on
  one. Each screen is its own instance (~6 MB). The hero's cat walks in from beyond the window's
  edge (`enter`): the same renderer twice for those seconds, the second with its camera on the
  cat, its picture of the cat alone carried across the page on a clear canvas.
- **The simulator, `/sim/`** (not linked, not in the sitemap, `noindex`): the whole device for
  judging a change before it is flashed. `components/SimPage` (controls grouped as the device is
  used, the event log, "copy this state as text"), `services/molty-screen/sim.ts` (`MoltySim`: one
  instance, the pointer as the finger, two speeds; `DeviceCalls`: the module's simulator calls),
  `features/sim/` (the menus' data; the astrology params come from the skill's own
  `skills/molty-astro/scripts/sky.mjs`, which `astro.config.mjs` runs in the browser). The
  module's side is `device/firmware/web/web_device.c`: device mode (the drawer's locals kept, so
  only the windows the device sends are sent; each frame costed with the panel's measured
  constants, its table at the top; the firmware's clock run at `DEV_CPU_X` times the wall clock so
  time-budgeted work spreads as on the device), the senses (sense.c's loop on gesture.c, every
  agent event from link_protocol.c's frames) and the lab (`LAB`: one line a firmware experiment
  switch; `features/sim/controls.ts` `LAB_LABELS`: one line for its words). Its code loads only on
  `/sim/`. `pnpm sim` drives it through Juan's script in both speeds (`e2e-out/sim/`).
- The page must read without the engine: a heading keeps its DOM text (it steps aside only once
  its word draws, `[data-word="live"]`), a Molty screen shows its still.
- A device is one thing: nothing of its screen shows until its frame stands in place
  (`[data-molty-screen][data-screen]`: `off` while the device is away or turning, then `dark`,
  then `live`; `still` without the renderer or with reduced motion). `pnpm in-place` samples
  every frame for it.
- The first paint is the intro's start state: a page with the water (`fluid` on its layout) gets
  `data-fluid` / `data-fluid-moltys="pending"` from an inline script in `Head.astro` before
  paint, and CSS keeps the headings' text, the stills and arriving devices unseen until
  `PageWater` (`data-fluid`: `live`) and the first screen (`data-fluid-moltys`: `live`, or `off`
  when the module cannot load) move them on. Never show something settled that the intro then
  hides. No JS: never set, all shows at rest; no script taking over: `static` after 8 s.
  Reduced motion: shown at once, stills only, no waves, no entrance.
- `src/features/<feature>/` holds copy and data (`home/content.ts`, `molty/actions.ts`).
  `src/hooks/` holds reusable React hooks. Features, services, hooks and config never import
  components.

## Styling

- `src/styles/tokens.css` is the only place for colours, fonts, sizes: black glass, fine dust,
  monospace. The only UI colour is the wordmark green; every other colour comes from a Molty's
  palette. Dark and light: on paper the dust is ink (its own tile) and the words'
  windows are inverted with their hue kept; a device's screen is its own canvas, a dark panel in
  both themes, so a Molty looks the same in both. Light text tokens hold AA (`scripts/tests/light-theme`).
  `ThemeToggle` shares Starlight's `starlight-theme` key.
- Components own CSS Modules. `global.css` imports tokens, base and layout only. Starlight's
  chrome is mapped in `src/integrations/starlight/starlight.css`.
- Static presentation belongs in CSS; inline styles only for runtime values.
- Layouts must not overflow horizontally at 390px; wide `<pre>` blocks scroll inside themselves.

## Verification

- `pnpm check` — architecture rules (`scripts/checks/architecture.mjs`: owners, barrels,
  layers, the renderer / packages import boundaries, single repo URL, colour/font
  tokens, cycles) + prettier (`pnpm format` fixes). No baselines or exemptions.
- `pnpm test` includes `molty.test.ts`: the committed module is the one its manifest
  describes, its sources have not changed since, it still draws the frames `make web-molty`
  checked against the native build, the still is one of its frames.
- `pnpm build && pnpm preview`, then `pnpm screens` (`e2e/screens.mjs`): screenshots to
  `e2e-out/` (gitignored), fails on page errors, 390px overflow, a page with no dust, a title
  the renderer does not draw, a tap that sends no wave, a Molty screen that does not draw or move, a stage whose buttons
  do not reach the cat, or a talk screen that does not ask and take an answer. Look at the
  screenshots.
- `pnpm molty` (`e2e/molty.mjs`, preview running): the hero and the moltys section at 1280 and
  390 px, dark and light; the cat's entrance; the frames a screen draws a second at 1280 and
  390 px with a wave out on the page, as is and with the CPU slowed 4x (fails under 30); nothing
  drawn out of view; stills only with reduced motion.
- `pnpm water` (`e2e/water.mjs`, preview running; the display's frames run by hand): a tap on
  the page moves its dust and the water in the hero's glass answers when the front gets to its
  rim; every heading of the landing, the showcase and the build page loosens as a front arrives
  and sharpens back; the title answers taps from six places; on the showcase a change of picture
  is a wave through the dust, from the middle and from the finger. Strips to
  `e2e-out/water-*.png`.
- `pnpm in-place` (`e2e/in-place.mjs`, preview running): on a cold cache, slow and fast, at 1280
  and 390 px, every frame of the load, a scroll through every section and a theme switch: a
  screen shows only inside its device at rest, never moves, never goes away. And the cat walking
  in is one picture on both sides of the rim in every frame (no step, no second pose).
- `pnpm sim` (`e2e/sim.mjs`, preview running): `/sim/` in both speeds: boot, a tap, a mood, a
  gesture, a listen, a say, questions, the install, the chart and its taps (a planet, a sign
  opening, back), an aspect, back to the cat; each step's event must come, the glass must change,
  and in device speed the panel must be what the firmware shows (`wm_glass_off`). Pictures to
  `e2e-out/sim/`.
- `pnpm fullpage` (`e2e/fullpage.mjs`): the whole landing, 1280 and 390 px, dark and light.
- `pnpm first-load` (`e2e/first-load.mjs`, preview running): samples every frame of the landing's
  first load on a slow network + CPU and fails if the hero is shown and then hidden, the device
  is not monotonic, CLS > 0.01, the renderer or the cat load before the first paint, or reduced
  motion / no JS / bundles that never load leave it hidden. Frames to `e2e-out/first-load-*.png`.
- `pnpm theme-label` (`e2e/theme-label.mjs`, preview running): the theme button's label is
  CSS-driven off `<html data-theme>` (set before paint); fails if any frame before a click shows
  another label (the old `[ theme ]` → `[ dark ]` flash) or a click does not flip it.
- `pnpm og` regenerates `public/og.png` from the live landing (needs the preview running).
- `pnpm readme` (`docs/assets/molty-hero*`) and `pnpm og-options` (`scripts/og/`) were written
  for the Moltys that emerged from the page fluid and drive that old slot protocol: they do not
  run against the screens yet. Port or drop them before the next README / OG refresh.
- `dist/llms.txt`, `llms-full.txt` and raw `docs/*.md` come from `scripts/generate-llms-txt.mjs`.
