# Packages

A package teaches a Molty to draw something new: a creature, or a family of scenes such as an astrology wheel.
It is **data only**, never code. The firmware knows how to play packages, not what any of them are about:
it has no idea what a planet, a zodiac sign or a cat is.

```
repo packages/<id>/      built package files live here (source of truth for now)
          │  the agent's skill says "install <id>"            (one tiny command through the relay)
          ▼
device    asks the owner with one tap, downloads the file straight from the repo, checks its SHA-256,
          keeps it in flash (under /spiffs/pkg/)
          │  the agent says "play <layout> of <id> with these few params"   (a few hundred bytes)
          ▼
screen    the device draws the layout from the package's own shapes and images
```

## 1. The package file

One JSON file, `packages/<id>/<id>.json`, at most **256 KB**. Built from sources by `packages/<id>/build.mjs`.
`packages/index.json` lists every package: `{ "<id>": { "name", "version", "kind", "description", "layouts"?, "skills"?, "path", "size", "sha256" } }`.
`description` is one line from `packages/<id>/description.txt` (what it adds, for an agent to pitch it to its human); a scenes
package lists its `layouts` and the `skills` that play it (those whose `molty.json` names it). `packages/tools/index.mjs` builds an entry.
`tools/scripts/check-packages.mjs` validates every package and the index (size, hash, schema, every reference resolves).

```jsonc
{
  "v": "molty-pkg/1",
  "id": "astrology",              // [a-z0-9-]{1,32}; the file name and the install id
  "name": "Astrology",            // shown in the install prompt and the console
  "version": "1.0.0",
  "kind": "scenes",               // "scenes" or "creature"
  "assets": { "<name>": Asset },  // images and glyphs, drawn once, used by name
  "layouts": { "<name>": Layout },// scenes (kind "scenes")
  "creature": { ... }             // kind "creature": the creature team's format (section 5)
}
```

### Assets

```jsonc
{ "kind": "mask",  "w": 16, "h": 16, "bits": "<base64, 1 bit per pixel, rows top to bottom, MSB first>" }
{ "kind": "image", "w": 48, "h": 48, "px": "<base64, 4 bits per pixel, index into palette>", "palette": ["#rrggbb", ... up to 16] }
```
A mask takes the colour of the item that draws it. An image keeps its palette. Index 0 of an image is transparent.

### Layouts

A layout is a timeline of items, the same model as the lab's scenes (`molty-creature-lab/astronomy`, `plot.mjs`):

```jsonc
{ "dur": 6,                                   // seconds; 0 = until replaced or its ttl ends
  "params": { "bodies": "list", "title": "text", "lit": "number" },   // what the agent may send, with types
  "items": [ Item, ... ] }
```

Every item has `k` (kind), optional `on`/`off` (seconds into the layout, for builds and reveals), `color` (#rrggbb),
and geometry in **glass units**: the round window has radius 1, `x` right, `y` up, angles in degrees, 0 = right, counter-clockwise.

| k | fields | draws |
|---|---|---|
| `ring` | `r`, `w` | a circle outline |
| `arc` | `r`, `w`, `a0`, `a1` | part of a circle |
| `ticks` | `r`, `len`, `n`, `a0` | n evenly spaced radial marks |
| `line` | `pts: [[x,y],...]` | a polyline |
| `curve` | `pts` | a smooth curve through the points |
| `disc` | `x`, `y`, `r` | a filled circle |
| `ball` | `x`, `y`, `r`, `img`, `lit` (0..1 lit fraction), `side` (-1/1 which side is lit), `spin` | a shaded sphere wearing an image asset |
| `glyph` | `x`, `y`, `size`, `asset`, `rot` | a mask or image asset |
| `text` | `x`, `y`, `size`, `str` | words, in the device font |

**Binding:** any field value may be a string `"$name"`, read from the params the agent sent (`"$title"`, `"$lit"`).
**Repetition:** an item with `"each": "$list"` is drawn once per element of that list param; inside it, `"@field"` reads the element
(`{ "k": "glyph", "each": "$bodies", "asset": "@glyph", "polar": ["@a", 0.72] }`).
**Polar placement:** `"polar": [angle, radius]` may replace `x`, `y` on any item that has them.
Nothing else: no expressions, no loops beyond `each`, no code. Every bound value is type-checked against `params`.

### Checks and the reference player (repo side)

- `packages/tools/format.mjs` holds the rules above and the firmware's limits (section 3); `tools/scripts/check-packages.mjs`
  (run by `hygiene.sh`) applies them to every package, checks the index's size and sha256 and that `build.mjs` reproduces the file.
- `packages/tools/play.mjs` draws a layout with its params onto the 1.46's braille lattice (52 x 32 cells of 2 x 4 dots) as a 412 px PNG:
  `node packages/tools/play.mjs astrology chart --params '{…}' --t 1`, or `--commands <file> --stills <dir>` to replay the
  `scene.*` commands a skill printed with `--dry-run`. What it does where the table is silent, for the firmware to compare:
  a ball's dark side hides what is behind it (with a faint earthshine) and an image's index 0 shows nothing; a `disc` in
  `#000000` clears what is under it (a body passing in front of the Sun); a mask lights a dot when it covers any part of it;
  `text` uses the device's 5x7 font, `size` being the glyph height.
- Param types: `number`, `text` (printable ASCII), `list` (flat objects, for `each`). `pts` is a literal list of points whose
  coordinates may be bindings (`[["@x1", "@y1"], ["@x2", "@y2"]]`).

## 2. Commands (agent -> relay -> device)

Over the existing command path (`link_cmd_register`), gated by device caps `pkg` and `scene`. The relay validates shapes and sizes.

| command | args | result |
|---|---|---|
| `pkg.install` | `{ id, url, sha256, size, name, job?, timeout_ms? }` | the device asks the owner with one tap, then answers at once `{ job?, id }` (or refuses: `invalid_args`, `bad_url`, `too_big`, `no_space`, `busy`, `unsupported` without touch). The outcome comes later as one event `{"t":"evt","name":"pkg","data":{ job?, id, state, ... }}`: `installed` (+`version`), `declined` (+`reason`: `declined`, `timeout`, `cancelled`) or `failed` (+`error`: `hash_mismatch`, `too_big`, `bad_package`, `no_space`, `download`, `tls`, `http_<code>`) |
| `pkg.remove` | `{ id }` | `{ removed: id }` or `not_installed` |
| `pkg.list` | `{}` | `{ packages: [{ id, name, version, kind, size }] }` |
| `scene.play` | `{ pkg, layout, params, ttl_s }` | replaces what is on screen (and the queue) with the layout: `{ pkg, layout, ttl_s }`. `not_installed`, `no_layout`, `bad_params`, `bad_package`, `invalid_args` |
| `scene.queue` | same as play | plays after the current one ends (at once when nothing plays); `busy` when 8 wait already |
| `scene.clear` | `{}` | melts back to the creature |

- **URL allowlist:** `url` must start with `https://raw.githubusercontent.com/apresmoi/molty.uno/` (one constant in firmware, the only place a host is named).
  The device downloads directly from there; package bytes never pass through the relay.
- **ttl_s** (default 60, max 600): an agent that died cannot leave a scene on screen. Back to the creature when it ends.
- A scene lives in RAM. Reboot = the creature.
- **Why install answers at once (firmware):** commands run on the link task, which also has to see the owner's tap,
  so it cannot wait for the question and the download. Same shape as `update`: refusals in the `res`, the outcome as an event.
- **The question:** the ask screen's labels are 8 letters with no header, so it shows the package name (its first 8
  letters, green) and `no` (white). The size is not shown; it is checked against 256 KB and the quota instead.
  The name shown is the one the agent sent; the stored name and version are the file's own.
- **Download:** straight from the allowlisted URL over verified TLS; redirects are not followed. The URL may use only
  `[A-Za-z0-9._/-]` after the prefix, no `..` or `//`, and must end in `.json`.
- Another screen owner (a say's words, a mood that shows the creature) drops the scene; a question covers it and it
  flows back after. Last writer wins.

## 3. On the device

- `main/pkg/`: `pkg_store` (install, remove, list, open; files under `/spiffs/pkg/`, an index file, 1 MB quota;
  a file is named by the first 8 hex digits of its SHA-256, `/spiffs/pkg/<sha8>.json`, because SPIFFS names are at
  most 31 characters; `index.json` maps ids to files),
  `pkg_parse` (strict, bounded JSON parse into PSRAM structs; rejects anything unknown).
- `main/scene/`: `scene_player` (a fill target beside blob, text and creature: `FD_ST_SCENE` in the director; rasterises the items into the fluid's target field each frame; handles on/off, ttl and the queue).
- SPIFFS can vanish (a factory flash overwrites it): a missing file is reported as not installed, and the agent reinstalls.
- Local API on molty.local for the console: `GET /api/pkg`, `DELETE /api/pkg?id=`, `POST /api/scene` (same args as `scene.play`), `POST /api/creature/select?id=`.
  Answers `{ ok: true, ... }` or `{ ok: false, error }`. `GET /api/creature/state` adds `scene: { pkg, layout, ttl_left }` while one plays.

### What the firmware accepts (as built)

Limits: 64 assets of at most 128 × 128 px, 16 palette colours; 32 layouts of at most 64 items and 8 params; 16 points a
line or curve; names `[A-Za-z0-9_-]{1,24}`; texts printable ASCII, at most 40 characters. A scene expands to at most 128
items and 40 letters in all; a list param has at most 32 elements, each a flat object of up to 8 numbers or texts.
Every declared param must be sent, and nothing else.

- `mask` bits and `image` nibbles are one continuous stream, rows top to bottom, no padding per row (high nibble first).
- `ring`, `arc` and `ticks` are centred on the glass centre. `ticks` run from `r` to `r + len` (negative `len`: inward),
  the first at `a0`. `w` (line width, default 0.03) is also allowed on `ticks`, `line` and `curve`.
- `glyph` `size` is the asset's width (height keeps its aspect); `text` `size` is the type size; both in glass units.
- `ball`: `lit` 0..1, `side` -1 or 1, `spin` in degrees per second; `img` (optional) wraps the sphere as an
  equirectangular map.
- Defaults: `color` #ffffff, `x` / `y` 0, `on` 0, `off` none, `a0` 0, `a1` 360, `n` 12, `len` 0.1, `size` 0.12, `lit` 1, `side` 1.
- Items paint in order, each covering what is under it: a `disc` of `#000000` erases, a `ball` covers its whole disc
  (its dark side too, with a faint earthshine), as `packages/tools/play.mjs` draws them. A scene queue holds 8.
- `"$name"` and `"@field"` are always bindings; a literal text cannot start with `$` or `@`.

## 4. Agents

A skill that uses a package (first: `skills/molty-astro`) keeps all domain knowledge on the agent's side. It reads `packages/index.json`,
(or the relay's `pkg.available`), sends `pkg.install` once, then `scene.play` / `scene.queue` with a few params. Astrology maths runs in the skill, never on the device and never in the relay.

`skills/molty-astro/scripts/astro.mjs` reads `pkg.list` and installs only when the package is missing or older than the
index; it waits for the install job (the owner's tap, at most about 2 min) and reinstalls once when a scene answers
`not_installed`. Its scenes stay under 1 KB of command each (a run of small layouts queued: at most 8 wait on the device).

## 5. Creature packages

`kind: "creature"`: one Molty, its body and all its behaviour as data (moods, gestures, walk, fidgets, ambient effects), in the motion format
of [`docs/CREATURES.md`](CREATURES.md). Duck, cat and octopus ship as packages and are also baked into the firmware image as defaults,
so a new device has them without a download. `POST /api/creature/select?id=` (console) or `creature.select { id }` (agent) swaps the active one.

- The file carries `creature` instead of `assets` and `layouts`: `{ about, body, walk, groups, events, base, gestures, moods, post, tints, ambient }`
  (`about`: how it looks and a line per gesture and mood, for the agent; [CREATURES.md](CREATURES.md) section 1).
  At most **40 KB** (tighter than the 256 KB of a scenes package), so a device holds the three defaults and a few more in RAM at once.
- Built by `packages/<id>/build.mjs` with the shared helpers in `tools/creatures/kit.mjs`; `make packages` rebuilds them all and their
  `packages/index.json` entries.
- The reference player is `device/creatures/js/creature-player.js` (the C port follows it, hash included). `make creatures-test` checks
  the player's rules and that every creature package plays clean within the format's limits and matches its index entry.

### On the device (as built)

- `main/creature/creature_player.c` plays a package exactly as the JS player does (`test/host/creature_build.sh` replays goldens
  `tools/creatures/golden.mjs` writes from the JS player: every mood, gesture and a walk, pose, bones, palette and ambient, for every
  creature package). `creature_body.c` meshes the parts as three.js does and draws them the lab renderer's way (its camera, light and
  "dither" style; eyes are holes in the colour). `creature_pkg.c` is one frame: the mood engine (`mood_engine.c`, the .molty runtime's
  too), the player, the body, the ambient. The device stands still: the walk layer plays with the walk at rest.
- **Built-in:** every `kind: "creature"` package in `packages/index.json` is embedded as it is at build time (`main/CMakeLists.txt`
  generates the table); nothing in the firmware names one. An installed package with a built-in's id replaces it.
- **The choice:** `creature.select { id }` (cap `creatures`) or `POST /api/creature/select?id=` answers `{ id, name }` once the file
  parsed (else `not_installed`, `bad_package`, `invalid_args`) and is kept in NVS. `creature.list {}` / `GET /api/creature/list`:
  `{ current, creatures: [{ id, name, version, source: "builtin" | "installed" }] }`.
- **Who it is:** `GET /api/creature/state` lists a package's `creature_info.gestures` and `.moods` (idle and its own); the link
  sends `{"t":"evt","name":"creature","data":{ id, name, about, gestures, moods }}` on each welcome and each change.
- **The .molty creatures stay:** uploading one (`POST /api/creature/file`, the console's catalog) makes it the creature and the choice
  (`current` is then `""`). A device that never chose keeps an uploaded .molty; otherwise it boots on the first built-in package.
  A chosen package that was removed since falls back to the first built-in.

## 6. Relay

`services/relay/src/core/commands.ts` validates, `room.ts` gates and forwards; nothing about any package is in the relay.

- Caps: `pkg.*` need `pkg`, `scene.*` need `scene`, `creature.*` need `creatures` in the device's hello caps; otherwise
  `409 unsupported` and nothing is sent.
- `pkg.install` `{ id, url (https, at most 512 bytes), sha256 (64 lower-case hex), size (1..262144), name (1..32 printable ASCII) }`.
  The host allowlist is the firmware's alone. The relay runs it as a job (agent: `{job}`; REST `wait: true` blocks at most
  24 s; `GET /v1/jobs/<job>`; MCP `molty_package`): it adds `job` and `timeout_ms` (60 s for the tap), takes the device's
  refusal from the `res`, and ends the job on the device's `pkg` event for that job (or, without `job`, that id):
  `installed` -> `{ installed, version }`; `declined` -> error `declined` (the reason in its message); `failed` -> its `error`.
  No event within 120 s: `pkg_timeout`.
- `pkg.remove` `{ id }`, `pkg.list` `{}`, `scene.clear` `{}`: forwarded as they are.
- `pkg.available` `{}`: the catalog, what an agent offers its human (`core/catalog.ts`). The relay reads
  `https://raw.githubusercontent.com/apresmoi/molty.uno/<ref>/packages/index.json` (`PACKAGES_REF`, default `main`; at most
  every 5 min, 30 s after a failure; the last good list when a refresh fails) and answers
  `{ ref, device, packages: [{ id, name, version, kind, description, layouts?, skills?, install, installed?, update? }] }`.
  `install` is the exact args of `pkg.install` (its url at the same ref). It needs no device and no cap: `device` is `online`
  (then it asks the device's `pkg.list`: `installed` is the version there or null, `update: true` when the index is newer),
  `offline`, `unsupported` (no `pkg` cap) or `unknown` (its list failed). An entry the relay could not turn into valid
  install args is left out. The index unreadable and nothing cached: `503 catalog_unavailable`.
- `creature.select` `{ id }` and `creature.list` `{}` need the cap `creatures`; forwarded as they are.
- `scene.play` / `scene.queue` `{ pkg, layout, params (an object, at most 2048 bytes of JSON), ttl_s (1..600, default 60) }`:
  `ttl_s` is always sent. `scene.play` counts as the agent taking the screen (like `mood`): the relay's automatic moods stand down.
- MCP: `molty_package { op: available|install|remove|list, … }`, `molty_scene { op: play|queue|clear, … }`.
