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
