# Molty Display v2: build report

Branch `feat/pkg-v2`, on top of `1e913af`. Phase 1 only (the display format). Nothing pushed, deployed,
flashed or published. No firmware change, no relay change, no change to the package format the device
installs.

## Result

| check | result |
|---|---|
| astrology built from the v2 skill folder | **same bytes**: sha256 `fe85da27201fd7cc5cb72c4aa35804a57d4a055cd5ce46f9d2e6639ab386b3f7`, 74344 bytes |
| `packages/index.json` | unchanged (sha256 `977e7915...2108` before and after) |
| round trip, every scenes package in the repo (astrology is the only one) | same bytes |
| display tests (new) | 16 pass, 0 fail |
| package format tests (`format.test.mjs`) | 9 pass, 0 fail, before and after |
| creature tests | 23 pass, 0 fail, before and after |
| firmware host tests for pkg and scene (`pkg_build.sh`) | pkg_test 98 passed, scene_test 267 passed, before and after |
| relay tests | 637 passed in 35 files (after only; `services/` has no diff) |
| `check-packages.mjs` | ok, output identical before and after |
| `check-skill.mjs`, `hygiene.sh` | output identical before and after: the same 6 failures that were there at `1e913af` (none in files this work touches) |
| molty-astro dry runs (13 command lines, 68350 bytes of output) | identical, `diff -r` clean |

## What was built

| piece | where |
|---|---|
| the schema, with descriptions for editors and agents | `schema/display-2.json`; `schema/display-2-screen.json` for a screen kept in its own file |
| the command | `packages/tools/molty-display.mjs`: `check`, `build`, `convert`, `preview` |
| its parts | `packages/tools/display/`: `load.mjs` (YAML with file and line), `schema.mjs` (the schema checker), `compile.mjs` (device rules, screens to layouts), `assets.mjs` (SVG and PNG to masks and images, and back), `build.mjs` (check, build, `display.md`, input schemas, cost report), `convert.mjs`, `preview.mjs` |
| the YAML parser | `packages/tools/vendor/js-yaml.mjs` (js-yaml 4.1.1, its own dist file, vendored so nothing needs installing; noted in `THIRD_PARTY_NOTICES.md`) |
| astrology as a v2 skill | `skills/molty-astro/`: `display.yaml`, `screens/*.yaml` (12), `assets/` (22 SVG, 18 PNG), `build/` (generated) |
| the dial, one-file form | `docs/examples/molty-dial/` |
| the guide | `docs/DISPLAY.md` (243 lines); one-line pointer in `docs/PACKAGES.md` |
| tests | `packages/tools/display/display.test.mjs`; `make display-test`, also in `make test` and `hygiene.sh` |

The old generator is gone. `packages/astrology/build.mjs` is now 13 lines that run the display build, so
`make packages` and `check-packages.mjs` work unchanged. The stroke rasteriser and the palette choice moved
into `assets.mjs` (the curve smoothing is `play.mjs`'s, which was the same function). The skill folder is
the only source: 40 assets rebuilt from its files are identical to the package's.

## Run each check from a clean checkout

```sh
cd <checkout of feat/pkg-v2>

# check and build both skills; the tree stays clean, the sha is the one above
node packages/tools/molty-display.mjs check skills/molty-astro
node packages/tools/molty-display.mjs check docs/examples/molty-dial
node packages/tools/molty-display.mjs build skills/molty-astro
node packages/tools/molty-display.mjs build docs/examples/molty-dial
shasum -a 256 packages/astrology/astrology.json packages/index.json
git status --short

# the tests: golden, round trip, every check rule, the limits
make display-test
node --test packages/tools/format.test.mjs
make creatures-test

# reproducibility and the index, as before
node tools/scripts/check-packages.mjs
make packages && git status --short

# firmware host tests that touch pkg and scene (needs IDF_PATH for cJSON)
device/firmware/test/host/pkg_build.sh

# relay
cd services/relay && pnpm install --frozen-lockfile && pnpm test && cd ../..

# the skill's own dry runs (compare with the same commands at 1e913af)
MOLTY_PKG_INDEX=$PWD/packages/index.json node skills/molty-astro/scripts/astro.mjs chart --date 2026-11-01 --dry-run

# the round trip by hand
node packages/tools/molty-display.mjs convert packages/astrology/astrology.json --to /tmp/astro-v2
node packages/tools/molty-display.mjs build /tmp/astro-v2 --out /tmp/astro-rt.json
cmp /tmp/astro-rt.json packages/astrology/astrology.json

# the preview (local only, 127.0.0.1); Ctrl-C stops it
node packages/tools/molty-display.mjs preview skills/molty-astro --port 4173
```

## Test output tails

```
$ make display-test
ok 1 - golden: the astrology skill folder builds today's package, byte for byte, and its committed docs
ok 2 - round trip: build(convert(v1)) is the v1 file for every scenes package in the repo
ok 3 - round trip: a v1 package with its fields in any order comes back equal, name for name and value for value
ok 4 - the schema: what the checker enforces is what editors read, and it covers every shape the compiler knows
ok 5 - check, the base skill: nothing to fix, and a problem names its file and line
ok 6 - check, layer 1 (the schema): unknown keys, misspelt shapes, wrong types, a bad color, a missing about
ok 7 - check, layer 2 (the device rules): bindings, names, text
ok 8 - check, layer 2: assets exist, are named once, and fit the device
ok 9 - check: what needs firmware is refused, with the way round it
ok 10 - check, layer 3 (the links): SKILL.md names real screens, and every screen
ok 11 - limits: 64 items a screen, 8 inputs, 32 screens, 128 items and 40 letters and 32 groups at the fullest, 256 KB
ok 12 - assets: a PNG as a symbol, a picture kept as it is or cut to 15 colors; an SVG's lines as a mask
ok 13 - the molty-astro skill sends what its screens declare: every dry-run command fits build/inputs
ok 14 - the dial example: one file, checks clean, builds what is committed, plays in the reference player
ok 15 - docs/DISPLAY.md: its first screen builds as written, and the call it shows is the one the build writes
ok 16 - preview: a local page that draws a screen with the reference player, a tapped zone lit
# tests 16   # pass 16   # fail 0

$ node --test packages/tools/format.test.mjs      # tests 9   pass 9   fail 0   (same before)
$ make creatures-test                             # tests 23  pass 23  fail 0   (same before)
$ device/firmware/test/host/pkg_build.sh
pkg_test: 98 passed, 0 failed
  real package: 12 layouts, 20 commands, 74344 B parsed into 126768 B, deepest queue 6
scene_test: 267 passed, 0 failed                  (same counts before)
$ cd services/relay && pnpm test                  # Test Files 35 passed (35), Tests 637 passed (637)
$ node tools/scripts/check-packages.mjs
check-packages: ok (astrology 74344 B, cat 247792 B, duck 31234 B, octopus 33754 B)
$ node packages/tools/molty-display.mjs build skills/molty-astro
assets    symbols 32, 17.1 KB; maps 8, 44.3 KB
screens   12, 10.6 KB; fullest: chart, 26 items written, 124 of 128 on screen with 10 bodies, 12 signs, 11 aspects
package   packages/astrology/astrology.json  72.6 KB of 256.0 KB  ok
docs      skills/molty-astro/build/display.md, skills/molty-astro/build/inputs/*.schema.json
```

`check-skill.mjs` fails the same way before and after (6 problems, all older than this branch):
`skills/molty/SKILL.md` is 7038 chars, three `api.md` facts, and two files under `packages/cat/blender/`
over 256 KiB.

The preview was loaded in headless Chrome and the chart screen looked at, in the braille view and in high
definition: the wheel, twelve signs, ten bodies and three aspect lines, drawn from the v2 skill's example.

## Deviations from the proposal

Each one is the smallest change that kept the oracle green, or a limit of phase 1.

| # | deviation | why |
|---|---|---|
| 1 | `sent_as` on an input or a list field: the name the agent sends when the drawing uses a clearer one | "real names" and "same bytes, same commands" cannot both hold without it: the package and `astro.mjs` say `g`, `a`, `say`; the drawing says `symbol`, `angle`, `reading` |
| 2 | a screen name may contain `-` | today's layouts are `sign-closeup`, `solar-eclipse`, `lunar-eclipse`, and the name is what the agent sends |
| 3 | a place may be `{ y: 0.08 }` alone (the other is 0) | today's items carry `y` without `x`; writing `x: 0` would add a field |
| 4 | fields come out in the order written. A `for` block puts the repeat marker last; written with `draw` above `for` it goes first | today's file has both orders. The 5 event screens (transit, solar-eclipse, occultation, appulse, lunar-eclipse) use the second form, with a one-line comment. It changes nothing on the device |
| 5 | assets keep today's names (`g-sun`, `sun`, `rings-p3`), not the proposal's (`sun`, `sun-map`) | the names are in the package and in what the agent sends |
| 6 | asset groups: `files` takes `{a,b,c}` lists (order kept) besides `*`; `smooth: true` draws SVG polylines as curves through their points; `stroke` is a fraction of half the drawing's width | the package's asset order is the zodiac's, not the alphabet's; the signs were authored as a few points with a smooth curve through them, so the SVGs hold those points (18 KB in all) |
| 7 | SVG support is `line`, `polyline`, `polygon` (no fills, no paths, no transforms); pictures and maps come from PNG only; a PNG is used at its own size (`pixels` on a PNG is checked, not applied) | the brief said reuse today's rasteriser, which draws stroked polylines; a second rasteriser or a resampler would have no oracle |
| 8 | the planet maps and ring masks are checked in as PNGs that already hold their 15 colors; the build keeps such a palette as it is. The 15-color choice (today's k-means) runs only for a PNG with more colors | the maps were generated from float noise; only the stored result reproduces the bytes. The noise and ring generators are deleted (they are in git history) |
| 9 | `default` is refused, not "documented only" as the proposal's table had it; `type: path` is refused with `move` | the brief asked for a refusal; a default the device ignores would be a false promise |
| 10 | a list takes `max` (most elements), a text input takes `max` (most letters), so the fullest screen can be counted. Groups are counted from `group`, not from `highlights` | the proposal's 128, 40 and 32 budgets need a bound per list; the chart is 124 of 128 with 10 bodies, 12 signs, 11 aspects |
| 11 | a list's fields need `about` too | the generated docs table has a row per field |
| 12 | x and y inputs are typed `number` with `min` and `max`, not `distance` | the proposal's example typed them `distance`, which is 0 to 2 and would refuse a negative x |
| 13 | the dial lives in `docs/examples/molty-dial/`, not `skills/molty-dial/` | `skills/` ships to every plugin user and `packages/index.json` feeds the live catalog; an example there is a product change. Moving it is `git mv` plus one index entry |
| 14 | `SKILL.md` keeps its one-line `description` (the proposal showed a folded one) | `check-skill.mjs` requires it |
| 15 | `references/layouts.md` became the generated `build/display.md` plus a short hand-kept `references/commands.md` | the install and event-loop instructions for agents without a shell are not screen data. That page still describes the REST calls with a bearer token placeholder, as it did before; it is the one place that does |
| 16 | the schema is checked by a small checker (`schema.mjs`) for exactly the keywords the schema uses (a test holds the schema to that list), with `errorMessage` for plain-word errors | a full JSON Schema library would be a large dependency with poor messages |
| 17 | the schema's shapes are `#/$defs/circle`, not `#/$defs/shape/circle`; split screens point at `display-2-screen.json` | valid JSON Schema nesting; editors need a file to point a screen at |
| 18 | package version stays `1.5.0` (the proposal's example said 2.0.0) | same bytes |
| 19 | the cost report counts 1 KB as 1024 bytes (72.6 KB), the proposal used 1000 (74.3 KB) | the limit is 256 x 1024 |
| 20 | `build/` of a skill is committed (`.gitignore` re-includes `skills/*/build/` and `docs/examples/*/build/`) | `SKILL.md` links `build/display.md`, and skills are installed from the repo |
| 21 | the sample call in `display.md` cuts lists to two elements | the chart's full example is 4 KB |

Edits to files that existed, all small: `play.mjs` exports two helpers (`chunk`, `smooth`);
`index.mjs` finds a package's skill by its `display.yaml` (so `molty.json` could fold in; `PACKAGES.md`
still says "those whose `molty.json` names it"); `check-skill.mjs` allows the SVG namespace host in
`.svg` files (one line); `hygiene.sh` and the `Makefile` run the new tests; `sky.mjs` and `astro.mjs`
had comments pointing at the old generator (comments only, output identical).

After Juan's later notes, `docs/DISPLAY.md` names one format only: no older format, no conversion, no
compiled form, no link to `PACKAGES.md`. `convert` still exists and is tested; it is described here and in
the tool's usage line, not in the guide.

## Not verified

- Nothing was run on a device, and the firmware was not rebuilt (it has no diff). The proof is byte
  identity of the package plus the firmware's host tests.
- The firmware host tests other than `pkg_build.sh` were not run (no file they build changed).
- The relay tests have no "before" run; `services/` has no diff.
- Editor completion from the schema was not tried in an editor (the schema is plain draft 2020-12).
- In the preview, clicking a tap zone in a real browser was not exercised: the page loaded headless and
  was looked at, and the test fetches a tapped frame from the server, but the click handler itself ran
  only by reading.
- CI was not run.
- The 15-color choice for PNGs with more than 15 colors is tested for shape and determinism, not against
  an oracle (astrology no longer goes through it).

## Where a sector zone and an "opens" tap plug in

The other branch adds two things to the package format: a tap target shaped as a ring sector, and a tap
that opens another layout with params. `tap_zone` was kept as a table row plus named cases so each is a
small addition:

| place | change |
|---|---|
| `schema/display-2.json`, `$defs.tap_zone.properties` | add `sector: { from, to, inner, outer }` beside `at` and `along`; add `opens: { screen, with }`. `required` is `["radius", "reads"]` today: turn it into an `anyOf` when a sector needs no `radius` or an opening tap needs no `reads` |
| `packages/tools/display/compile.mjs`, `SHAPES.tap_zone` and `shape()` | a plain field is one more row in the table; a structured one is one more `else if` in `shape()`, next to `along` (a comment marks the spot). `opens.screen` is checked against `skill.screens`; its `with` values go through `value()` like any binding |
| `packages/tools/display/convert.mjs`, `shapeOf()` | the reverse case, next to the one for a zone's segment |
| `packages/tools/display/preview.mjs`, the page's `far()` | the hit test for a sector; an opening tap can switch the screen picker |
| `docs/DISPLAY.md`, the Taps table | two rows |
| `display.test.mjs`, the schema test | already fails if the schema and the compiler's table disagree, so neither side can be added alone |

The device-side rules need nothing here: once `format.mjs` knows the new fields, `check` picks them up
through the package's own check, which already runs last on every build.

# Phase 2: Moltys

Same branch, after phase 1. No firmware change, no change to what the device installs, duck and octopus untouched.
Nothing pushed, deployed, flashed or published.

## Result

| check | result |
|---|---|
| the cat built from its Molty folder (`molty.json` + `cat.glb`) | **same bytes**: sha256 `96189940b42cc4fb53297b2205f8a924f97796823f6c8235eb88d0227473170e`, 247792 bytes |
| the same build with Blender off the PATH (PATH holding only node) | same sha256; this is the golden test, and it also checks that `blender` cannot be found there |
| `cat.blend` through the Blender exporter | writes the committed `cat.glb` byte for byte (sha256 `244ee9df...bdf04`), twice in a row, 57 s |
| `packages/index.json`, `duck.json`, `octopus.json`, `astrology.json` | unchanged from `1e913af` |
| Molty tests (new, `make molty-test`) | 14 pass, 0 fail (the exporter one runs only where Blender 5.3 is; it ran here) |
| display tests, package format tests, creature tests | 16, 9, 23 pass, as before |
| firmware host tests for creatures and packages | clip 48, creature 532, pkg 98, scene 267 passed, as before |
| relay tests | 637 passed (no diff in `services/`) |
| `check-packages.mjs`, molty-astro dry runs | identical output |
| `check-skill.mjs`, `hygiene.sh` | **changed, for the better**: 4 old failures left of 6. The two that named files under `packages/cat/blender/` are gone with that folder |

## The facts first

What was in `cat.blend` and what the scripts added, before this work:

- `cat.blend` held the rig (51 bones, 43 of them exported), the render model (49 metaballs, 16 meshes), the props,
  and 49 actions baked for the render sheets. Those actions were not what the device got: they had another frame
  count and were not read by the exporter.
- The exporter (`export_molty.py`) took **nothing animated from the file**. Each clip was a Python function of time
  (`cat_clips.py`, 1351 lines) that posed the rig through `cat_poses.py` on every frame; the drawn shapes were
  Python data in `cat_model.py`; the route map, the views and the paws' contacts were Python constants. A full
  export took about 70 minutes on this machine, nearly all of it Python posing.
- Is that export bit-stable? One full re-run of the same posing on Blender 5.3.0 gave samples that build to
  today's `cat.json` byte for byte, so yes for that run. A second run was not completed (it was stopped to free the
  machine), so "stable across runs" rests on one reproduction plus the exporter's own two identical runs.

What was done about it: the scripts were run once more inside Blender and what posed the rig on every frame was
keyed into `cat.blend` as 45 actions (with their length, loop flag, view, follow and stride), the 90 drawn shapes
were added as Empties on their bones, and the feet were marked. `cat.blend` now holds everything; the scripts are not
needed. The exporter reading those actions gives the same samples as the scripts did, bit for bit (checked track by
track for all 5378 tracks), in under a minute.

## What a Molty is now

The format (tool-neutral, what the tools read):

```
moltys/cat/
  molty.json    55 lines: name, about, look, 12 colors, 5 poses, 7 moods and 18 gestures in words, feet, center
  cat.glb       the body file: 43 bones, 90 shapes, 45 actions sampled once a frame (5.2 MB)
```

Our source for it, and the one script that turns it into the body file:

```
moltys/cat/cat.blend                  the Blender file (4.9 MB)
tools/blender/molty_export.py         Blender to .glb, for any Molty made our way (280 lines). Needs Blender 5.3
```

Before, the cat was 15 files and about 3,450 lines of its own code: `packages/cat/build.mjs` (89 lines) and
`packages/cat/blender/` with `cat_model.py`, `cat_poses.py`, `cat_clips.py`, `export_molty.py`, `anim_render.py`,
`render_sheet.py`, `overlay_ref.py`, `preview_data.mjs`, `animations.html`, `model-sheet.html`, `cat-export.json`,
`cat.blend`, `.gitignore`. All of `packages/cat/blender/` is removed. `packages/cat/build.mjs` is 13 lines that run the
Molty build, so `make packages` and `check-packages.mjs` work unchanged; `cat.json` and `description.txt` beside it are
build outputs.

The shared tools: `packages/tools/molty-creature.mjs` (`check`, `build`, `preview`) over `packages/tools/creature/`
(`body.mjs` reads a body file or a hand-written body, `bake.mjs` turns actions into the device's clips, `build.mjs`
does the names and the package, `preview.mjs` the page), and `schema/molty-2.json`. None of them imports the
exporter or runs any program; a test holds that.

Names chosen: the file is `molty.json` because it is the Molty (the skill sidecar that had that name went away in
phase 1). The command is `molty-creature`, beside `molty-display`.

## The asset form: glTF, with a hand-written twin for small ones

| | (a) a glTF file (`.glb` / `.gltf`) | (b) a JSON of our own |
|---|---|---|
| who can make it | every 3D tool exports it; libraries exist in every language | anyone with a text editor; no tool exports it |
| skeleton and actions | native: nodes and animations, the same layout the device's clips already use (y up, facing +z, a channel a bone) | easy to define |
| shapes | not in the standard: carried in each node's `extras` (5 keys, documented) | first class |
| the cat's 2934 baked frames, 5378 tracks | 4.15 MB of floats, 5.2 MB with its index | about 11.5 MB as text |
| written by hand | no (binary) | yes |
| code to read it | 60 lines, no dependency | 40 lines |

Recommendation, and what is built: **(a) is the body file**. It passes the test "could someone without Blender make
it": any tool that saves glTF does, and the page documents the few `extras`. (b) cannot carry a baked body at a
sane size, but it is the only form a person can type, so the same model can be written **inside `molty.json`**
(bones, shapes, keys in degrees and seconds) for a small Molty. Both are read into one plain form by `body.mjs`;
everything after that is shared. Sampling, key reduction and packing happen in the build, for both.

What the device package needs that neither form can change: see deviations 1 and 2.

## The proofs of the surface

- **A second Molty by editing only JSON**: the cat's folder copied, `molty.json` changed (renamed Ember, four
  colors, `wiggle` now plays `stretch`, the `inspect` mapping dropped). With the mapping dropped, `check` says
  `molty.json:55  gesture inspect: there is no action named inspect. Name an action inspect, or say which one it
  plays`; with a gesture named after an existing action it is wired by the name alone. It builds, its clips equal
  the cat's, it draws in its own colors. The test makes it each run; it was previewed and looked at (a ginger cat
  mid-stretch). It is not committed: it would copy the 5 MB body.
- **A Molty written by hand, no 3D tool**: `docs/examples/molty-pip/molty.json`, 4 bones, 7 shapes, 2 actions
  (`idle`, `hop`), 55 lines. Builds to 2.2 KB, plays in the device's reference player, previewed and looked at.

## Run each check from a clean checkout

```sh
node packages/tools/molty-creature.mjs check moltys/cat
node packages/tools/molty-creature.mjs build moltys/cat && shasum -a 256 packages/cat/cat.json && git status --short
env -i PATH="$(dirname "$(which node)")" node packages/tools/molty-creature.mjs build moltys/cat   # no Blender in reach
node packages/tools/molty-creature.mjs build docs/examples/molty-pip
make molty-test                         # the exporter test runs a few actions when Blender 5.3 is on the PATH
MOLTY_EXPORT_ALL=1 make molty-test      # the whole cat.glb again from cat.blend, byte for byte (about a minute)
make display-test creatures-test && node --test packages/tools/format.test.mjs
node tools/scripts/check-packages.mjs && make packages && git status --short
device/firmware/test/host/clip_build.sh && device/firmware/test/host/creature_build.sh && device/firmware/test/host/pkg_build.sh
blender -b moltys/cat/cat.blend -P tools/blender/molty_export.py -- --out /tmp/cat.glb && cmp /tmp/cat.glb moltys/cat/cat.glb
node packages/tools/molty-creature.mjs preview moltys/cat --port 4174     # local only; Ctrl-C stops it
```

## Test output tails

```
$ make molty-test
ok 1 - golden: the cat's folder builds today's cat.json byte for byte, with no Blender on the PATH
ok 2 - the cat: what its names wire up, with nothing said about it in molty.json
ok 3 - the exporter (only where Blender 5.3 is): cat.blend still gives the committed cat.glb
ok 4 - the exporter says which Blender it needs
ok 5 - a second Molty from the cat, by editing only its JSON: renamed, recoloured, a gesture remapped, a mapping dropped
ok 6 - a Molty written by hand, with no 3D tool: it builds what is committed, moves and hops
ok 7 - docs/CREATURE.md: its first Molty builds as written
ok 8 - the schema: what the checker enforces is what editors read
ok 9 - check: a problem names molty.json, the line and the fix
ok 10 - conventions: a way between poses is found by name, or because one free action goes that way
ok 11 - limits: 80 bones, 128 shapes, 64 actions, 8 poses, 32 gestures, names of 24 letters, 256 KB
ok 12 - the rounding is the first exporter's: six digits and halves to even, as Python does them
ok 13 - preview: a local page that plays the built Molty with the device's own player
ok 14 - the Molty tools never run a 3D tool
# tests 14   # pass 14   # fail 0   # skipped 0

$ MOLTY_EXPORT_ALL=1 node --test --test-name-pattern="the exporter" packages/tools/creature/creature.test.mjs
ok 1 - the exporter (only where Blender 5.3 is): cat.blend still gives the committed cat.glb     (60.6 s)

$ node packages/tools/molty-creature.mjs build moltys/cat
body      43 bones, 90 shapes, 12 colors
actions   45, 2934 frames, 224.7 KB; 7 moods, 18 gestures, 5 poses, 16 ways between them
package   packages/cat/cat.json  242.0 KB of 256.0 KB  ok

display 16 pass; format 9 pass; creatures 23 pass
clip_build: 48 passed, 0 failed     creature_build: 532 passed, 0 failed
pkg_test: 98 passed, 0 failed       scene_test: 267 passed, 0 failed
relay: Test Files 35 passed (35), Tests 637 passed (637)
check-packages: ok (astrology 74344 B, cat 247792 B, duck 31234 B, octopus 33754 B)
```

## Deviations, and limits you should know

| # | what | why |
|---|---|---|
| 1 | a Molty can only be made of the parts the device knows how to draw: outlined `torso head tail legF.L legF.R hindL hindR bowl yarn`, features `eye nose maw tongue earIn whisker chest milk yarnline`, painted from fixed color names (`body`, `iris`, `shine`, `nose`, ...) | the device's drawing code names them (`creature_dots.c`, `clip-dots.js`), and firmware does not change. `check` enforces it with the list. "Any body" is true for the skeleton and the actions, not yet for the look |
| 2 | how exactly a bone's keys are kept depends on its name (`tail`, `ear`, `eye`, `mouth`, `tongue`, `neck`, `head` looser and in one byte; `pelvis`, `spine`, legs tighter) | the cat's first exporter did this by name, and the bytes depend on it. Other names get the default. The player also has a few names of its own (`listening`, `walk`) |
| 3 | two body forms are read (a glTF file, or the body inside `molty.json`), though the brief asked to recommend one | a hand-written Molty with no tool was required, and a binary file cannot be typed |
| 4 | which pose an action starts and ends in is **worked out** (nearest resting pose), unless its name is `<a>_to_<b>` or it says `from` / `to`. For the cat all 45 come out as the scripts had them | the brief: transitions are worked out unless named. A way between poses is `<a>_to_<b>`, or the one action going that way that no mood, gesture or mood-entry plays; two candidates is an error that says which name to give |
| 5 | `molty.json` lists `feet` (which bones, in order) while the point on the bone comes from the body file | the order is in the package, and a bone's length is not in glTF |
| 6 | in `cat.blend` the drawn shapes are Empties with custom properties (exact numbers), added beside the old render model (metaballs and meshes), which is still in the file and is **not** what gets exported | the device draws analytic shapes, not meshes. The render model now can drift from the shapes; removing it or regenerating it from the Empties is a follow-up |
| 7 | the cat's actions in `cat.blend` are one key a frame on every bone that moves (what the scripts posed), not hand-shaped curves | they come from procedural clips. Editable in Blender, but not as pleasant as authored curves |
| 8 | `cat.glb` is 5.2 MB and `cat.blend` 4.9 MB in git (before: one 5.5 MB `.blend` and a 335 KB JSON) | the body file holds every frame as it was sampled, with nothing rounded, which is what makes the bytes reproduce |
| 9 | the exporter test exports 4 actions (12 s) when Blender 5.3 is present and compares them key for key; the whole file byte for byte only with `MOLTY_EXPORT_ALL=1` (60 s) | a minute in every test run seemed too much; both were run and pass |
| 10 | small edits to shared files: `tools/creatures/kit.mjs` gained `packageText` (the builders for duck and octopus are untouched and rebuild the same bytes); `clip-dots.js` exports its two part lists; `check-skill.mjs` treats `.glb` and `.blend` as binary assets like `.png`; `display/load.mjs` reports the right line for top-level keys of a JSON file; `docs/CREATURES.md` points at the cat's new folder; `hygiene.sh` and the `Makefile` run the new tests | each is needed by the above |
| 11 | `docs/CREATURE.md` is 152 lines (asked: under about 150) | |

## What was removed, and one thing worth keeping somewhere

Removed from the tree (all in history; commit `ba3d43e` holds them together with the two one-off scripts that did
the baking, `record.py` and `bake_blend.py`): `cat_model.py`, `cat_poses.py`, `cat_clips.py`, `export_molty.py`,
`anim_render.py`, `render_sheet.py`, `overlay_ref.py`, `preview_data.mjs`, `animations.html`, `model-sheet.html`,
`cat-export.json`, the old `build.mjs` body.

Still useful as authoring aids, if you want them back: `cat_clips.py` with `cat_poses.py` is where the cat's
motion was really written (gaits, paw planting, the transitions), and `cat_model.py` rebuilds the model from
numbers. To change a clip the way it was made, one edits those and bakes again. Proposal: restore the three, with
`record.py` and `bake_blend.py`, under `tools/blender/cat-authoring/`, clearly ours and outside what a Molty is.
They are not in the tree now, as asked.

## Not verified

- Nothing ran on a device, and the firmware was not rebuilt (its only diff is none; `device/` changed in one JS
  reference file, by two `export` words).
- The exporter on any other Blender version, machine or OS. It refuses a version other than 5.3 by a check at its
  top; that refusal was read and is pinned by a test of the source, not run on another Blender. Whether another
  machine's Blender 5.3 solves the IK to the same bits is unknown; the committed `cat.glb` makes the build
  independent of it.
- Changing the cat in Blender by hand (moving a shape Empty, keying a new action) and exporting was not exercised;
  the exporter was only run on the baked file.
- A glTF from another 3D tool was not tried. The reader takes float keys with straight or held interpolation and
  refuses the rest by name.
- The preview pages were loaded headless and looked at (the cat, Ember mid-stretch, Pip mid-hop); their buttons were
  not clicked in a real browser.
- CI was not run.
