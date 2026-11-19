# Docs update report

Branch `trial/hd-dust`, checked against commit `1e913af`. Nothing committed, pushed or deployed.

## Status

- Docs, skills and website now describe what the code on this branch does.
- `pnpm check`, `pnpm test` (31 pass) and `pnpm build` (21 pages) pass in `apps/website`.
- 10 built pages opened headless at 1280 px and 390 px: no errors, no overflow, 0 dead internal links.
- Facts about `device/` and `packages/` were read from the commit, not from the other agent's edits in flight.

## Details

**Files changed**

| File | What |
|---|---|
| `docs/PACKAGES.md` | "At a glance" (format v1, limits, what ships, one example), install ring, store rules, astro command table |
| `docs/CREATURES.md` | what ships, cat 2.0.0 clips, call to the front, rings over the cat, corrected limits |
| `docs/AGENT-LINK.md` | hello caps, pairing screen, command rows, recording ring table, silence rule, tap `spot`, MCP tools |
| `docs/ARCHITECTURE.md` | command table, components, firmware layout, "What is on the screen", frame rate |
| `docs/HARDWARE.md` | 1.75C status, build and flash lines for it |
| `skills/molty-astro/SKILL.md`, `references/layouts.md` | install question wording, tap readings |
| `skills/molty/references/api.md`, `creature.md` | install wording, the cat's own gestures, the cat comes when called |
| `apps/website/src/config/docs.mjs`, `astro.config.mjs` | two synced docs, three sidebar entries, edit link path |
| `apps/website/src/content/docs/getting-started/on-screen.md` | new page |
| `.../introduction.md`, `setup.md`, `agents/connect.md` | brought up to date |
| `apps/website/src/features/home/content.ts`, `components/HomePage/HomePage.tsx` | boards list, two links |

**Website changes to accept or reject**

1. New docs page "What Molty shows" (Start group): the screen and the hands, in tables.
2. Sidebar: added "What Molty shows", "Creature packages", "Packages and scenes".
3. Home, bake: added the 1.75C row; the 1.28 now reads "experimental" (was "supported").
4. Home, talk: new link "what molty shows".
5. Home, moltys: new link "packages & scenes" (desktop only).
6. Introduction: Moltys now "a duck, a cat and an octopus as data packages"; the bridge line names ask and scenes; boards table has the 1.75C.
7. Setup: step 2 lists creature packages (was "29 Moltys"); added the pairing line and step 4, voice.
8. Connect: pairing screen text, a fuller command table, a "Skills" section naming `molty-astro`.
9. Edit links of hand-written pages now point at `apps/website/` (was `website/`).

**Doc and code disagreements found (code won, all fixed unless marked)**

| # | Doc said | Code says |
|---|---|---|
| 1 | CREATURES: cat is a 31.4 KB channel creature | rigged, 242 KB, no channels |
| 2 | CREATURES: 1413 LINEAR, 413 STEP, 1 CUBICSPLINE | 1380, 447, 0 |
| 3 | CREATURES: duck 30.1 KB, octopus 32.8 KB, older counts | 30.5 and 33.0 KB; counts measured again |
| 4 | AGENT-LINK: the ring breathes with the voice | a fixed circle over duck and octopus; voice ripples only over the cat |
| 5 | AGENT-LINK: pairing shows a held code | a question screen with accept and reject arcs |
| 6 | AGENT-LINK: hello example with 16 caps | 29 caps |
| 7 | AGENT-LINK: no `progress`, `gestures`, `pkg.*`, `scene.*`, `creature.*` rows; 6 MCP tools missing | all exist |
| 8 | AGENT-LINK: `tap {x, y}` | also `spot` on a scene's tap spot |
| 9 | PACKAGES: every astro scene under 1 KB | the chart sends 4.2 KB (5.8 KB with `--all`) |
| 10 | molty-astro: install buttons "Astrolog" and "no" | "INSTALL ASTROLOGY?", `install` and `not now` |
| 11 | HARDWARE: 1.75C "untested on hardware" | the changelog and commits show it running |
| 12 | ARCHITECTURE: ten commands, no packages, two boards | stale throughout |
| 13 | Website: 1.28 "supported"; setup has 3 steps and 29 Moltys | `board.json` says experimental; 4 steps, creature packages |
| 14 | NOT FIXED (not my path): `pkg_cmd.h` and `mole_fluid.h` comments | still describe the old install buttons and the old ring |
| 15 | NOT FIXED: relay takes a 512 byte package URL | firmware refuses over 384 |
| 16 | NOT FIXED: docs name `ask-options.html`, `ring-options.html`, `chart-options.html`, `PORT-SPEC.md` | those files are untracked, not committed |
| 17 | NOT FIXED: `CHANGELOG.md` Unreleased | has none of this work |

**Relay not deployed (documented from source, not live)**

- `ask` `title`, the 14 / 10 / 8 label limits, and `firmware_too_old` for labels over 8.
- They are in `docs/AGENT-LINK.md`, `skills/molty/SKILL.md` and `references/api.md` already. I left them and added no "live" claim.
- Same doubt for the silent press rule (`voiced.ts`): in source, deploy state unknown.

**Could not verify**

- Anything on a device: frame rates, the 30 fps floor, the 1.75C status, how the ring, questions and scenes look. I read code and comments only.
- What the deployed relay accepts.
- The cat's cost numbers in CREATURES section 12 ("to be confirmed on the device"): left as they were.
- `tools/scripts/check-skill.mjs` fails with the same 6 errors before and after my edits (old ask wording pinned, SKILL.md 38 characters over its limit, two Blender files over the plugin size limit).
- 108 em dashes remain in `docs/AGENT-LINK.md`, more in `skills/molty`. Some are pinned by `check-skill.mjs`. I removed them only in files I rewrote.

## Next steps

Add when the unshipped features land:

| Feature | Where |
|---|---|
| Whole-wedge sign taps | `PACKAGES.md` spots, `layouts.md` signs, "What Molty shows" |
| A sign tap opens its scene on the device | `molty-astro/SKILL.md` "Tap to read", `layouts.md` close-up loop |
| Recording ring over a scene | `AGENT-LINK.md` ring table, "What Molty shows" |
| The agent learns the scene on screen | `skills/molty/references/events.md`, `AGENT-LINK.md` status |
| Astrology 1.6.0 (in flight) | version and size in `PACKAGES.md` "At a glance" and its example |
| Relay deploy | a `skills/molty/CHANGES.md` entry and a skill version bump |

Decisions for Juan:

1. Keep the new "What Molty shows" page and the two home links?
2. Is the 1.75C "experimental" or "supported"? Same question for the 1.28.
3. The site still presents Moltys as ~1 KB `.molty` files with a 26 creature catalog. The device ships three package creatures. Reposition?
4. Strip the remaining em dashes (needs the `check-skill.mjs` pins changed)?
5. Fix the 6 `check-skill.mjs` failures?

## Second pass: docs rewritten for readers

Juan rejected the docs site: it served raw engineering files. Now a visitor reads five short
hand-written pages. The engineering files are unchanged in purpose and sit in one folded group.

**Sidebar**

1. What Molty is: `/getting-started/introduction/`
2. Set it up: `/getting-started/setup/`
3. Connect your agent: `/agents/connect/`
4. What Molty shows: `/getting-started/on-screen/`
5. Packages: `/moltys/packages/`
6. Reference (folded): Flash a release by hand, Build and flash from source, Port to your board,
   Agent protocol, Command line and local MCP, How it is built, Package format, Creature package
   format, Legacy: the .molty kit, Legacy: the .molty viewers, Legacy: the .molty file, Contributing

**Reader pages**

| Page | Lines | Tables |
|---|---|---|
| What Molty is | 44 | 1 |
| Set it up | 52 | 1 |
| Connect your agent | 61 | 0 |
| What Molty shows | 54 | 1 |
| Packages | 50 | 1 |

**How it is wired**

- Every reference page opens with a note: "Reference for contributors", or "Legacy" on the three .molty pages.
- The package and creature formats moved to `/reference/...`. `/moltys/packages/` is now the friendly page.
- Old addresses redirect, so the landing page's links still work: `/moltys/overview/` and
  `/moltys/making-your-own/` go to Packages, `/moltys/format/` and `/moltys/creature-packages/` to their reference pages.
- Not touched: `src/components`, `src/features`, `src/pages`. The landing page still says "make your own" and
  "the .molty format"; those links now land on Packages and on the legacy page.

**Left out, because not true or not shipped**

- How to write a package or a creature: one closing line only ("on its way"), no format.
- "The question shows at the top": an agent's question text on screen needs the relay that is not deployed. The page says only that the answers appear.
- "A scene stays a minute": false for the example, which ends after 4 seconds. The page says the creature comes back when the scene ends.
- "The 1.75C is the sharpest": false, it has fewer pixels per inch than the 1.46. Now "the largest". Also fixed in `HARDWARE.md`.
- Forgetting an agent from the console: I found no such button, so it is not mentioned.
- The Codex install lines, self-hosting a relay, the skills table, frame rates: cut for length; they are in Reference.

**Reference lines fixed because newer commits changed the code**

- `PACKAGES.md`: astrology is 1.6.0; the install line over the cat is green.
- `AGENT-LINK.md`: the progress line over the cat is green.

**Checks**: `pnpm check`, `pnpm test` (31 pass) and `pnpm build` (22 pages) pass. 1148 internal links, 0 dead.
All five reader pages viewed headless at 1280 and 390 px.

## Third pass: diagrams drawn by glyphcss

Every hand-drawn diagram in the docs is now mermaid source, and the website draws it with
`@glyphcss/diagrams`.

**How to author a diagram (three lines)**

1. Write a fenced `mermaid` block in the markdown file. Start it with `accTitle: what it shows`.
2. Pick the family: `sequenceDiagram` for exchanges between parties, `flowchart LR` for a short pipeline, `flowchart TD` for anything that fans in or out. Short plain labels; `<br>` breaks a long one.
3. Run `pnpm test` in `apps/website`. It draws every block at both widths and fails on one that loses a label, cuts a word or does not fit.

**How the site renders them**

- At build time. A markdown plugin (`apps/website/src/integrations/markdown/glyphDiagrams.ts`) turns each block into a text figure with `@glyphcss/diagrams`.
- Why build time: no script, font or request ships for a diagram. Script bytes in the build are unchanged (1,986,883 before and after). Each figure is about 5 KB of HTML.
- Two renders per diagram, 78 and 44 columns. CSS shows the one that fits its box and scales the type to it; under 9.5 px it scrolls inside its own box. The page never scrolls sideways.
- Lines are dim, arrowheads green, words in the text colour, all from the site's tokens, so light and dark both work.
- GitHub draws the same blocks natively.
- The package: `glyphcss` 0.3.2 does not contain diagrams. They are a separate published package, `@glyphcss/diagrams`. I added it pinned at 0.3.2, the same release as the site's `glyphcss` and `@glyphcss/fluid`. 0.3.4 is out and drew these the same.

**Converted (14)**

| File | Count | Diagrams |
|---|---|---|
| `docs/AGENT-LINK.md` | 8 | relay hub (flow), filling the silence (flow), shake to talk (sequence), while the human speaks (sequence), reflex (flow), event path (flow), firmware update (sequence), voice box (sequence) |
| `docs/ARCHITECTURE.md` | 1 | two ways an agent reaches Molty (flow) |
| `docs/PACKAGES.md` | 1 | install and play (sequence, Juan's source) |
| `docs/CREATURES.md` | 1 | creature pipeline (flow, Juan's source) |
| `README.md` | 1 | Molty, relay, agent (flow) |
| `device/creatures/MOLTY-FORMAT.md` | 1 | .molty pipeline (flow) |
| `skills/molty/references/self-hosting.md` | 1 | your own relay (flow) |

Not converted, because they are not diagrams: the directory listings in `ARCHITECTURE.md`, `PORTING.md` and
`device/creatures/README.md`, and the ledger sample in `skills/molty/references/etiquette.md`.

The finder, run from the repo root, prints nothing:

```sh
grep -nE '[─│┌┐└┘├┤┬┴┼▶▼◀▲╌╎]|\+--|-{3,}>|<-{3,}' docs/*.md README.md CONTRIBUTING.md \
  device/creatures/README.md device/creatures/MOLTY-FORMAT.md device/creatures/js/cute/README.md \
  apps/website/src/content/docs/getting-started/*.md apps/website/src/content/docs/agents/*.md \
  apps/website/src/content/docs/moltys/*.md $(find skills -name '*.md') \
  | grep -v '^docs/DOCS-UPDATE-REPORT.md' | grep -vE '^[^:]+:[0-9]+:[│ ]*([├└]── |│ {2,})'
```

The last filter drops directory listings. The same check runs in `scripts/tests/diagrams.test.ts`.

**Where the detail went.** Paths, endpoints, field names and limits that lived inside the drawings are now a line
or a small table right under each diagram. Nothing was dropped.

**Corrected against the code**

- `ARCHITECTURE.md`: the console's setup has four steps (Wi-Fi, Molty, agent, voice), not three.
- `CREATURES.md`: the package limit is 40 KB, or 256 KB for a rigged creature; the drawing said 40 KB only.
- `AGENT-LINK.md`, firmware update: the drawing now shows that Molty itself downloads the release.

**Changed shape, same content**

- Architecture: the "home network" frame is a bullet under the diagram; a frame made the drawing 212 columns wide.
- README: "your desk", "the cloud" and "anywhere" are in the box labels instead of three frames.
- Relay hub, voice box, updates: path names moved from the arrows to the text below.

**To know**

- `device/creatures/MOLTY-FORMAT.md` is under `device/`, which I was told not to touch. The job named it, so I changed that one diagram only.
- Not verified: how GitHub draws these blocks (no push). `accTitle:` is standard Mermaid.
- `tools/scripts/check-skill.mjs` now also flags `skills/molty-astro/SKILL.md` at 7131 characters. That came with commit `081a532`, not from this work.
- Commits `081a532` and `30629d0` shipped things the reader pages left out as unshipped (a tap that opens a scene, the ring over a scene, the scene told to agents). The reader pages were left as they are, as asked.

## Fourth pass: the original division back, without the old kit

The second pass's five pages plus a Reference group are gone. The docs are divided as they were, and the glyphcss
diagrams stay.

**Sidebar**

- Start: Introduction, Flash a release, Set up your Molty, What Molty shows, Porting to your board
- Fluid: How it renders
- Moltys: Packages and scenes, Creature packages
- Bridge: Connect an agent, Protocol: pairing, REST, MCP, CLI and local MCP
- Boards: Build and flash
- Project: Contributing (GitHub is the icon in the header)

**Removed from the site:** "Moltys and the viewers", "Make your own" and "The .molty format". Their addresses redirect
to Packages and scenes. The source files under `device/` are untouched and no longer published.

**Light fixes**

- Packages and scenes opens with the short friendly text (what a package is, what ships, one tap to install, how a scene is shown). Its one raw curl with a bearer token is gone: the skill command comes first, then the small call, and the plain request sits in "Writing your own client" at the end.
- Packages and scenes, Creature packages: file names and repo paths are out of the prose. Each page ends with one "Where this lives" list.
- Old-kit mentions trimmed from How it renders and the protocol page (five phrases each). The truth fixes from the earlier passes are kept.
- Introduction, Set up your Molty, Connect an agent and What Molty shows keep their short text under their original titles.

**Grep over the 13 published doc pages (rendered text)**

| Looked for | Found |
|---|---|
| legacy, old kit, cute kit | none |
| v1 outside a `/v1/` address | none |
| `.molty` | 2, both on Contributing |
| raw curl blocks | before: 1 (Packages and scenes). After: 0 |

**Needs a product change, not a docs change**

- Contributing still tells contributors to re-export `.molty` files, because the repo still ships that kit and its `make creatures` target.
- The landing page (not my path) still says "a ~1 KB .molty file" and links "make your own" and "the .molty format"; those links now land on Packages and scenes.
- There is no helper in the molty skill or the CLI for packages and scenes. The only simple way is the `molty-astro` skill; anything else is a raw request.
- A package example still shows `"v": "molty-pkg/1"`, because that is the real field in the file.

**Checks**: `pnpm check`, `pnpm test` (48 pass), `pnpm build` (18 pages) and first-load pass. 929 internal links, 0 dead.
Moltys pages viewed headless at 1280 and 390 px.
