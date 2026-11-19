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
