# Web Molty report

Branch `feat/web-molty`, cut from `trial/hd-dust` at `e600d2a`. Nothing was deployed, pushed,
flashed or sent.

## Status

The website's screens now run the firmware's own drawing code. No JavaScript draws a Molty any more.

- **What runs.** `device/firmware/main/fluid/mole_fluid.c` itself, with everything it draws with
  (fluid, creature player, crisp dots, rings, question, scene player, package parser), compiled to
  WebAssembly. 24 firmware source files, unchanged.
- **Firmware files touched: none.** Nothing under `device/firmware/main` or `boards` changed. The
  web build is a new folder, `device/firmware/web/`.
- **The cat is the only creature shown**, per the scope change. The renderer still loads any
  creature package.
- **All checks pass.** See "How to run it".

### What was built

| Piece | Where | What it is |
|---|---|---|
| The web board | `device/firmware/web/web_molty.c` | Includes `mole_fluid.c` whole. Adds a panel in memory, a finger, no IMU, and the calls (`wm_*`). Draws nothing itself. |
| The chip's SDK on one thread | `device/firmware/web/web_sdk.c`, `sdk/` | A clock, a seeded random source, semaphores as counts, tasks as functions. As the host tests stub the SDK. |
| Native player | `device/firmware/web/web_main.c` | Plays a script and prints a hash of every frame. |
| Build script | `device/firmware/web/build.sh` | `host`, `wasm`, `deps`. Emscripten pinned to 7.0.0. |
| The module | `device/firmware/web/built/molty.wasm` + `molty.json` | Committed. The manifest records the toolchain, the hash of every source, and the fidelity result. |
| Build and check tool | `tools/web-molty/` | `make web-molty`, `make web-molty-test`. |
| Loader and screen | `apps/website/src/services/molty-screen/` | `core.ts` (no DOM), `screen.ts` (canvas, frames, pointer), `program.ts`. |
| Components | `MoltyScreen`, `MoltyActs`, `MoltyShowcase`, `SpeedReader` | Every round screen on the site is a `MoltyScreen`. |

### The rule for the built module

**The `.wasm` is a committed artifact, checked by tests. The website build needs no toolchain.**

Why this one:

- The site's CI and deploy run on Node alone today. They stay that way.
- A stale module cannot ship quietly. `pnpm test` (Node only) fails when any firmware source it was
  built from has changed, when the module is not the one its manifest describes, or when it no
  longer draws the checked frames. CI now runs the website job on firmware changes too.
- With the pinned Emscripten on the PATH, `make web-molty-test` rebuilds and wants the same bytes.
  It does today.

So a change to how a Molty looks is: change the firmware, run `make web-molty`, commit both.

### Fidelity check

`make web-molty` and `make web-molty-test` both do this:

- Play `device/firmware/web/fidelity.script` on the native build and on the module.
- Fixed seed, fixed inputs, fixed frame times. **2051 frames, all identical, every pixel.**
- The script: boot screen, the cat 2.0.0, a tap, three moods, eat, yarn, walk, a shake, a listen
  with a voice, a question answered by a tap, a say, the astrology chart with a tapped sign.
- The native build is `-O2` (as the device), the module `-Os`. Different optimisers, same pixels.
- The site's `pnpm test` then proves the committed module still draws those frames.

### Sizes

| What | Raw | gzip | When it loads |
|---|---|---|---|
| `molty.wasm` | 342,585 B | 161,737 B (brotli 142,246) | When the first screen mounts, after first paint |
| Loader (`services/molty-screen`, minified) | 5,719 B | 2,319 B | With the device island, which was already there |
| Cat package | 247,792 B | 108,707 B | With the module |
| Astrology package | 74,372 B | 26,579 B | Only for screens that play scenes |
| Chart sample (own chunk) | 12,706 B | 3,207 B | Only for the bake screen |
| Still image of the cat | 3,600 B | | With the page |

- The module and the cat are fetched, never part of the page's own requests. The first-load
  check now fails if either starts before the first paint. Measured on its slow network and CPU:
  first paint 444 ms, module requested at 1838 ms.
- No longer loaded: three.js from the CDN (about 600 KB), the creature rig, 29 beat files, 29
  thumbnails.
- Memory: 16 MB reserved per screen, about 6 MB touched. Five screens on the landing.

### Frame rate

Measured in headless Chromium (Playwright 1.61.1) by `pnpm molty`, which counts every
`putImageData` on a screen's canvas for 10 seconds:

| Case | Frames a second | Longest gap | Main thread a frame |
|---|---|---|---|
| Hero in view, 1280 px | **60.0** (600 drawn) | 18.5 ms | 1.56 ms, page fluid included |
| Same, CPU slowed 4 times | **60.0** (600 drawn) | 27.2 ms | 4.29 ms |

- One firmware frame costs about 0.43 ms under Node.
- Out of view, or tab hidden: no frames. Checked.
- Reduced motion: one settled still per change, no loop. Checked.
- The check fails under 30.

## Visible page changes

Each one is yours to accept or reject.

### Landing

1. **Hero.** The device now powers on like the real one: the boot screen ("molty.uno" in dots,
   about 3 s), then the cat. Before: duck, octopus and bunny took turns, and ran in from the right.
2. **Hero, taps.** A tap ripples the water and the cat comes to the front. After 5 minutes
   untouched it dozes off, as on the device.
3. **No Molty in the page's braille fluid any more.** The fluid still draws the background and
   the headings.
4. **Talk.** The screen is the device's reader, then the cat comes back and the question stands on
   the rim as arcs (yes, no, idk). Before: three pie sectors. While it listens, `[ yes ] [ no ]
   [ idk ]` also show as buttons under the status line, for the keyboard.
5. **Moltys, the copy.** New text: "The first Molty is a cat. This is the device's own drawing
   code, running here. Tap the glass and it comes to you. More creatures are coming." Phone: a
   shorter version.
6. **Moltys, the picker.** The 29 names are gone. In their place: moods (7), its own (eat, yarn,
   walk, shake), on the glass (listen, ask), and one line of the cat's own words for what plays.
7. **Moltys, the links.** Now "everything it does", "creature packages", "packages & scenes".
   Gone: "all moltys & actions", "make your own", "the .molty format", and the `.molty` download.
8. **Moltys, the heading colour.** The cat's lavender (`#a99be8`). Before: green, tinted by the
   picked Molty.
9. **Bridge.** The device shows the pairing question as the device asks it: "pair with claude?",
   "check the code matches", `4821`, accept or reject. Accept makes the cat happy. Before: the
   octopus.
10. **Bake.** The cat, then the astrology birth chart for 40 s, and round again. Planets, signs
    and lines are tappable and read out. Before: the duck. This is the scene from goal 3. It
    needed no new section.
11. **Phones, moltys section.** The lists wrap instead of scrolling sideways. The device is
    smaller there, because the lists take room.

### Showcase

12. **The cat only.** Groups: moods, its own, gestures (14), on the glass, scenes (chart, moon,
    saturn). "random" and "autoplay" stay. The 29 names and the download are gone.
13. **Copy.** "The cat, the first Molty, and everything it can do. Tap an action: the device's own
    code plays it right here. Tap the glass too." Then "More creatures are coming."
14. **Links and URL.** Footer links point to creature packages and packages & scenes. A link is
    now `?a=<action>` (was `?m=<molty>&a=<action>`). The page description changed.

### Outside the site

15. `README.md`: one sentence (the showcase link no longer says "see them all") and two build lines.
16. `device/creatures/README.md`: two lines, now true.
17. `THIRD_PARTY_NOTICES.md`: a section for the module (cJSON, Emscripten's libc).

Not changed: navigation, page layout, hero copy, bridge and bake copy, the boards list, the footer,
`public/og.png`, the README hero image.

### Removed from the site

- `src/features/moltys/catalog.ts` (the 29-creature catalog).
- `src/components/MoltyPicker/` (replaced by `MoltyActs`).
- `src/services/molty-stage/`: `moltyMesh.ts`, `shiftTarget.ts`, `actions.ts`, `speedReader.ts`,
  and `beats/` (33 files). It keeps only the page fluid's headings.
- `scripts/thumbs.mjs`.
- Config: the `@creatures`, `@molty-files`, `@molty-catalog` aliases, the three.js CDN plugin and
  pin, their type declarations.
- Small leftovers: the screen and still filter tokens, the yes / no / idk colour tokens, the
  "quick" reveal, `Device`'s `replay`.

Left alone: everything under `device/creatures/`. Its `catalog/` folder now has no reader.

## How to run it

From a clean checkout of this branch. Needs Node 22, pnpm, a C compiler, and ESP-IDF v5.5 for its
cJSON (as the host tests do). Emscripten only to rebuild the module.

```sh
# 1. check the committed module against the firmware sources (no emscripten needed)
make web-molty-test

# 2. the website
cd apps/website
pnpm install
pnpm check
pnpm test
pnpm build
pnpm preview            # leave it running; default port 4330

# 3. in another terminal, from apps/website: look and measure
pnpm first-load
pnpm screens
pnpm molty

# 4. only after changing the firmware's drawing code: rebuild the module
brew install emscripten # 7.0.0, pinned in device/firmware/web/build.sh
make web-molty
cd apps/website && pnpm molty-still   # only if the cat's resting look changed
```

What I ran, all green: `make web-molty`, `make web-molty-test`, `make firmware-test`,
`make firmware BOARD=waveshare/esp32-s3-touch-amoled-1.75c` (compile only, 2,023,520 B image),
`pnpm check`, `pnpm test` (36 tests), `pnpm build`, `pnpm first-load`, `pnpm screens`, `pnpm molty`.

Pictures, in `apps/website/e2e-out/` (not committed):

- `molty-hero-1280-dark.png`, `molty-hero-1280-light.png`, `molty-hero-390-dark.png`,
  `molty-hero-390-light.png`
- `molty-moltys-<width>-<theme>.png` (the same four for the moltys section)
- `molty-tap-sheet.png`, `molty-tap.webp`, `molty-tap-00.png` to `-31.png`: the cat walks off,
  a tap calls it back
- `landing-1440-ask.png`, `showcase-1440-chart.png`, `showcase-390.png`, `first-load-*.png`
- `og-candidate.png`: what `pnpm og` would make now

## Next steps

### Things you should know

- **I had to repair Python after the install.** `brew install emscripten` pulled in Python 3.15
  and Homebrew pointed `python3` at it. That broke `make firmware` for everyone on this machine
  (ESP-IDF looks for a Python 3.14 environment). I pointed `python3` back at 3.14 with
  `brew unlink python@3.14 && brew link --overwrite python@3.14`. Emscripten keeps its own 3.15.
  The firmware build passed after that. Anyone installing Emscripten will hit the same thing.
- **60 frames a second is faster than the device.** The device paces itself at 33. Almost
  everything in the firmware runs on time, not on frames, so it looks the same and smoother. One
  thing I saw that runs per frame: the water settling after a listen ends. To match the device
  exactly, set `FPS` to 33 in `services/molty-screen/screen.ts`.
- **The web repaints the whole panel every frame.** The device sends only changed cells. Same
  drawing code, its own full-repaint path. The one place they can differ: a scene on fine dust
  under load, where the device puts some cells off to a later frame.
- **A firmware quirk, not mine.** A question asked within about a second of a listen ending forms
  late (about 3 s instead of 1.6 s), because the water is still being calmed. Seen in both builds.

### Not verified

- **Against a real device.** The check is module against native build. The device adds its own
  maths library, compiler and the two differences above.
- **Safari and Firefox.** Only headless Chromium was run.
- **Real phones.** The 4 times slower CPU is a stand-in.
- **Plugin loaders and the `.wasm`.** `tools/scripts/check-skill.mjs` limits tracked text files to
  256 KiB. I listed `.wasm` with its binaries (`.bin`, images). The module is 335 KiB. If a plugin
  loader does refuse it, the module must move out of the plugin tree.
- `make hygiene` still fails on this branch for 6 reasons that were there before (SKILL.md
  length, api.md, two files under `packages/cat/blender/`). None are from this work.

### Not done

- **`public/og.png` and the README hero still show the old creatures.** `pnpm og` works with the
  new page (see `og-candidate.png`). I did not replace the public image without your say.
- **`pnpm readme` and `pnpm og-options` no longer run.** They drive the old slots. Port them or
  drop them.
- **`device/creatures/catalog/`** has no reader now. Delete it if you agree.
- **Duck and octopus** are not shown anywhere, per the scope change. To show one later:
  `MoltyScreen` takes its creature from `features/molty/content.ts`.

### Open question

Do you want the site at the device's 33 frames a second (exact) or at 60 (smoother)? It is 60 now.
