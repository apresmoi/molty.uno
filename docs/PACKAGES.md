# Packages

A package teaches a Molty to draw something new: a creature, or a family of scenes such as an astrology wheel.
It is **data only**, never code. The firmware knows how to play packages, not what any of them are about:
it has no idea what a planet, a zodiac sign or a cat is.

```
repo packages/<id>/      built package files live here (source of truth for now)
          │  the agent's skill says "install <id>"            (one tiny command through the relay)
          ▼
device    asks the owner with one tap, downloads the file straight from the repo, checks its SHA-256,
          keeps it in flash (/spiffs/pkg/<id>.json)
          │  the agent says "play <layout> of <id> with these few params"   (a few hundred bytes)
          ▼
screen    the device draws the layout from the package's own shapes and images
```

## 1. The package file

One JSON file, `packages/<id>/<id>.json`, at most **256 KB**. Built from sources by `packages/<id>/build.mjs`.
`packages/index.json` lists every package: `{ "<id>": { "name", "version", "kind", "path", "size", "sha256" } }`.
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

## 2. Commands (agent -> relay -> device)

Over the existing command path (`link_cmd_register`), gated by device caps `pkg` and `scene`. The relay validates shapes and sizes.

| command | args | result |
|---|---|---|
| `pkg.install` | `{ id, url, sha256, size, name }` | the device asks the owner with one tap ("Install Astrology, 41 KB?"). Accept: download, verify, store. `{ installed: id, version }`, or error `declined`, `bad_url`, `too_big`, `hash_mismatch`, `no_space`, `busy` |
| `pkg.remove` | `{ id }` | `{ removed: id }` or `not_installed` |
| `pkg.list` | `{}` | `{ packages: [{ id, name, version, kind, size }] }` |
| `scene.play` | `{ pkg, layout, params, ttl_s }` | replaces what is on screen with the layout. `not_installed`, `no_layout`, `bad_params` |
| `scene.queue` | same as play | plays after the current one ends |
| `scene.clear` | `{}` | melts back to the creature |

- **URL allowlist:** `url` must start with `https://raw.githubusercontent.com/apresmoi/molty.uno/` (one constant in firmware, the only place a host is named).
  The device downloads directly from there; package bytes never pass through the relay.
- **ttl_s** (default 60, max 600): an agent that died cannot leave a scene on screen. Back to the creature when it ends.
- A scene lives in RAM. Reboot = the creature.

## 3. On the device

- `main/pkg/`: `pkg_store` (install, remove, list, open; files under `/spiffs/pkg/`, an index file, 1 MB quota),
  `pkg_parse` (strict, bounded JSON parse into PSRAM structs; rejects anything unknown).
- `main/scene/`: `scene_player` (a fill target beside blob, text and creature: `FD_ST_SCENE` in the director; rasterises the items into the fluid's target field each frame; handles on/off, ttl and the queue).
- SPIFFS can vanish (a factory flash overwrites it): a missing file is reported as not installed, and the agent reinstalls.
- Local API on molty.local for the console: `GET /api/pkg`, `DELETE /api/pkg?id=`, `POST /api/scene` (same args as `scene.play`), `POST /api/creature/select?id=`.

## 4. Agents

A skill that uses a package (first: `skills/molty-astro`) keeps all domain knowledge on the agent's side. It reads `packages/index.json`,
sends `pkg.install` once, then `scene.play` / `scene.queue` with a few params. Astrology maths runs in the skill, never on the device and never in the relay.

## 5. Creature packages

`kind: "creature"`: one Molty, its body and all its behaviour as data (moods, gestures, walk, fidgets, ambient effects), in the motion format
the creature team defines (`docs/CREATURES.md`). Duck, cat and octopus ship as packages and are also baked into the firmware image as defaults,
so a new device has them without a download. `POST /api/creature/select?id=` (console) or `creature.select { id }` (agent) swaps the active one.
