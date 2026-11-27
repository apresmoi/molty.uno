# Draw on Molty

A skill that draws on Molty is one folder with three things in it:

| in the folder | what it is |
|---|---|
| `SKILL.md` | words for the agent: when to show which screen |
| `display.yaml` | the drawing: screens made of shapes |
| `assets/` | symbols and pictures as plain SVG and PNG files (only if you need them) |

## Your first screen

Make a folder, say `my-skill/`, and put this in `my-skill/display.yaml`:

```yaml
molty: display/2
package:
  id: hello
  name: Hello
  version: 1.0.0
  about: A ring, a pointer and a word.
screens:
  hello:
    about: A pointer on a ring.
    inputs:
      angle: { type: angle, about: Where the pointer points., example: 90 }
    draw:
      - circle: { radius: 0.9, color: ink }
      - spoke: { angle: "{{ angle }}", length: 0.7, width: bold }
      - label: { text: HELLO, at: { y: -0.4 }, size: 0.14 }
```

Check it and build it (one command; it tells you the file, the line and the fix when something is wrong):

```sh
node packages/tools/molty-display.mjs build my-skill
```

See it on your computer: `node packages/tools/molty-display.mjs preview my-skill`, then open the address it prints.

Show it on Molty. The agent sends this through the molty skill (its `molty_scene` tool, or the script
your skill ships). The build writes the call for every screen, in `build/display.md`:

```json
{"name":"scene.play","args":{"pkg":"hello","layout":"hello","params":{"angle":90},"ttl_s":60}}
```

## Places and units

Units never change, so learn them once.

| what | unit |
|---|---|
| a distance, a radius, a size, a width | a fraction of the screen's radius: 1 reaches the rim |
| an angle | degrees, 0 at 3 o'clock, going counter-clockwise (90 is 12 o'clock) |
| a time | seconds from the moment the screen starts |

A place is the centre, an x and y, or an angle and a distance:

```yaml
at: center                           # the middle of the glass, the default
at: { x: 0.2, y: -0.4 }              # right and up are positive; leave one out and it is 0
at: { angle: 195, distance: 0.825 }  # around the centre
```

## Colors and line widths

Use Molty's names and a new skill looks like Molty. A color can also be `"#rrggbb"`, a width a number.

| name | is | use it for |
|---|---|---|
| `ink`, `grid`, `dim`, `white` | bright to faint | main strokes, scales, background, emphasis (the default) |
| `erase` | black | clears what is under it |
| `hairline`, `thin`, `bold` | 0.012, 0.03, 0.06 | crisp one-dot lines, the default, a pointer |

Add your own under `tokens`: `tokens: { colors: { coral: "#ff9a7a" }, sizes: { lit_ring: 0.075 } }`.

## Shapes

`draw` is a list. Each entry is one shape: its name, then what it takes. Later shapes paint on top.
Every shape except `tap_zone` also takes `color`.

| shape | takes | draws |
|---|---|---|
| `circle` | `radius`, `at`, `width` | a circle outline |
| `arc` | `radius`, `start`, `end`, `at`, `width` | part of a circle, between two angles |
| `ticks` | `count`, `radius`, `length`, `first_angle`, `width` | evenly spaced marks round the centre |
| `spoke` | `angle`, `radius` (where it starts), `length`, `width` | one mark at an angle: a pointer |
| `line` | `from` and `to`, or `points` (2 to 16); `smooth`; `width` | straight segments, or a smooth curve |
| `dot` | `radius`, `at` | a filled circle |
| `globe` | `radius`, `at`, `map`, `lit`, `lit_side`, `spin` | a shaded ball wearing a map: a moon phase, a planet |
| `symbol` | `asset`, `size`, `at`, `rotate` | one of your assets |
| `label` | `text`, `size`, `at` | up to 40 letters in Molty's own font |
| `tap_zone` | see [Taps](#taps) | nothing: a place to tap |
| `beat_clock` | `radius`, `dot` | the rhythm Molty plays, as a clock (see [A clock and a wheel](#a-clock-and-a-wheel)) |
| `wheel` | `id`, `radius`, `grab_from`, `step`, `by`, `fast`, `min`, `max`, `value`, `face`, `sets`, `labels`, `tick_ms`, `tick` | a ring at the edge the finger turns |
| `wave_ring` | `radius` | the chord Molty plays, on one circle (in a `beat_clock`'s place; its wheels set `pitch` and `pick`) |
| `centre_tap` | `radius` | nothing: a tap in the middle plays and stops the clock |

```yaml
draw:
  - ticks: { count: 12, radius: 0.82, length: 0.08, width: hairline, color: grid }
  - dot: { radius: 0.05 }
```

## Inputs and binding

An input is something the agent sends with the screen. Give each a `type`, an `about` (one sentence for
the agent) and an `example`. Use it with `"{{ name }}"`, in quotes.

| type | the agent sends |
|---|---|
| `number`, `angle`, `distance`, `amount`, `seconds` | a number (`amount` is 0 to 1; `min` and `max` narrow it) |
| `text`, `reading` | words: up to 40 for a label, up to 160 for what a tap shows |
| `color` | `"#rrggbb"` |
| `asset` | the name of one of your assets (`of: assets/planets` keeps it to one folder) |
| `groups` | group names with spaces between, for a tap to light |
| `list` | up to 32 small objects; `of` names their fields, `max` says how many at most |

```yaml
inputs:
  value: { type: angle, about: Where the pointer points., example: 135 }
draw:
  - spoke: { angle: "{{ value }}", length: 0.8 }
```

A binding fills a whole value. No sums, no text with a gap in it: the skill works those out and sends the result.

## Repeat and reuse

`for` draws its shapes once for each element of a list input. Inside it, `"{{ body.angle }}"` reads a field.

```yaml
inputs:
  bodies:
    type: list
    max: 10
    about: The planets to place.
    of:
      angle: { type: angle, about: Where it sits. }
draw:
  - for: body in bodies
    draw:
      - dot: { radius: 0.04, at: { angle: "{{ body.angle }}", distance: 0.64 } }
```

Two more tools, both worked out when you build, so they cost nothing to send:
- `data`: a fixed table in `display.yaml` (the twelve signs and their angles). `for: sign in zodiac` repeats over it.
- `parts`: a group of shapes with a name. Place it with `- use: wheel`, pass values with `with: { sign_size: 0.1 }`.

`skills/molty-astro/display.yaml` uses both.

## Taps

A `tap_zone` draws nothing. Tap it and Molty shows its `reads` at once, word by word.

```yaml
- tap_zone:
    at: { angle: "{{ body.angle }}", distance: 0.64 }
    radius: 0.08
    reads: "{{ body.reading }}"   # what Molty shows
    id: "{{ body.id }}"           # names the zone in the tap event the agent gets
```

| key | meaning |
|---|---|
| `at` + `radius` | a round zone |
| `along: { from, to }` + `radius` | a band along a line, for tapping a line |
| `sector: { from, to, inner, outer }` | a wedge of a ring: from one angle to another, between two distances |
| `opens` | a tap opens another screen on Molty; it comes back when that ends, or at a tap beside it |
| `highlights` | group names to light. Left out: whatever is drawn inside the zone |
| `grow` | how much the lit shapes grow, 1 to 3 (1.25 if you say nothing) |

```yaml
- tap_zone:
    sector: { from: 180, to: 210, inner: 0.75, outer: 0.9 }   # this wedge
    reads: Aries, bold and quick.
    id: aries
    opens: { screen: sign-closeup, seconds: 12 }              # opens that screen, for 12 s
```

The opened screen finds its inputs by name: in the tapped element of the list, else among this screen's inputs. A
list passes on only the elements whose `of` is the zone's `id`. For a wedge around a zone an input places, say
`sector: { width: 30, inner: 0.75, outer: 0.9 }`: so many degrees wide, around `at`. A tap right on a zone's own
circle or line beats one that only falls in a wedge.

Any shape can join a group with `group: "{{ body.id }}"`. A lit shape grows and brightens while the reading
plays. A shape with `only_when: highlighted` shows only then.

## A clock and a wheel

Three shapes draw from Molty's own state, and the finger acts on them on Molty, with no round trip:

```yaml
draw:
  - beat_clock: { radius: 0.672 }                                  # the rhythm the beat player holds
  - wheel: { id: tempo, sets: tempo, step: 12, by: 2, fast: 4, min: 40, max: 220, value: 100 }   # while it plays
  - wheel: { id: rhythm, sets: pick, step: 30, face: 1 }           # while it is stopped
  - centre_tap: { radius: 0.26 }                                   # plays, stops
```

- `beat_clock` draws the rhythm `rhythm.play` sent: a ring a drum, a mark a step, a dot a hit, the hand and each hit
  swelling as it sounds, from the speaker's own clock; stopped, the hand at rest and a play mark in the middle. A tap
  on a dot mutes that drum. One a screen. Nothing it does changes the picture whole: the screen forms once.
- `wheel` turns in clicks: a drag that starts from `grab_from` (0.85) out never stirs the water or taps anything.
  Each click adds `by` to a number, or with `labels: "{{ names }}"` (a list whose elements each have a `label`) steps
  to the next one. `sets: tempo` or `pick` drives the beat player. Each click ticks the motor (`tick_ms` at `tick`
  percent; left out, Molty's own). The agent hears one `ring` event when it settles. `face: 0` is up while the clock
  plays (or with no clock), `face: 1` while it is stopped.
- `centre_tap` plays the clock (after a count-in) and stops it; it needs a `beat_clock` (or a `wave_ring`) on the screen.
- `wave_ring` draws the chord `wave.play` sent (the molty-waves skill): each voice's hits as dots joined as its shape,
  the hand, the outline the sound pushes out; its `sets: pitch` wheel turns the pitch (fine steps, a detent on each
  note; its own tick the fine step's), its `sets: pick` wheel the chord kinds, its `centre_tap` plays and stops.
- Sent with regions (`rhythm.play`'s `regions`), a `sets: pick` wheel shows one region's rhythms, the region's name small
  above: a tap on it goes up to the regions, and `centre_tap` there opens the one chosen.

The molty-rhythm skill (`skills/molty-rhythm/`) is the example; [PACKAGES.md](PACKAGES.md#layouts) has every rule.

## Assets

Put files in `assets/` and list them. An asset's name is its file name without the ending.

```yaml
assets:
  - { files: "assets/signs/*.svg", as: symbol, pixels: 64 }
  - { file: assets/moon.png, as: map }
```

| `as` | from | on screen |
|---|---|---|
| `symbol` | an SVG made of lines, or a PNG (its solid pixels) | one color: the `color` of the shape that draws it |
| `picture` | a PNG | its own colors, up to 15, and see-through |
| `map` | a PNG twice as wide as tall | wrapped round a `globe` |

An SVG needs `pixels` (its width on Molty, 128 at most). A PNG is used at its own size.
`smooth: true` rounds an SVG's lines into curves. `"assets/{sun,moon}.png"` lists files in the order you want.

## Timing

```yaml
duration: 6                           # seconds; or hold (stays until replaced)
subtitles: middle                     # where readings show: bottom, middle, or a y
draw:
  - dot: { radius: 0.1, show: { from: 0.4, until: 2 } }
```

`show` says when a shape is on screen: without `from` it is there from the start, without `until` it stays to
the end. Both take bindings, so a list can carry each step's times.

## SKILL.md and the docs you get

In `SKILL.md`, link each screen where you say when to use it: `[hello](display.yaml#hello)`. The build writes
`build/display.md` (every screen, its inputs and a call that works: link that too) and
`build/inputs/<screen>.schema.json`, which lets a skill check what it is about to send.

| command | does |
|---|---|
| `molty-display check <folder>` | finds mistakes: spelling and types, then what Molty cannot take, then links in `SKILL.md` |
| `molty-display build <folder>` | check, then the package, its cost, and `build/` |
| `molty-display preview <folder>` | a page on your computer: pick a screen, edit inputs, scrub time, click tap zones |

`molty-display` is `node packages/tools/molty-display.mjs`. A skill in `skills/` builds into `packages/<id>/`,
where Molty fetches it once merged: commit the folder and the built package, then run
`node tools/scripts/check-packages.mjs --write-index`. Any other folder builds into its own `build/`. Editors
complete `display.yaml` when it starts with `# yaml-language-server: $schema=<path to>/schema/display-2.json`.

## Limits and edge cases

| what | limit |
|---|---|
| screens in a package | 32 |
| shapes written out in a screen (a part or a data table counts every shape) | 64 |
| shapes on screen once every list is at its `max` | 128 |
| inputs to a screen | 8 |
| elements in a list, fields in an element | 32, 12 |
| letters in a label, and in all labels on screen; characters in a reading | 40; 160 |
| groups a screen can light; names in one `highlights` | 32; 4 |
| assets; pixels a side | 64; 128 |
| the built package | 256 KB |
| text | plain ASCII; cannot start with `$` or `@` |
| names | screens, inputs and fields: lower case, digits and `_` (a screen may use `-`), 24 at most |
| every input | must be sent, and must be used by some shape |
| `sent_as` on an input or a field | the name the agent sends, when you want a clearer one in the drawing |
| read from inputs | a point of a `line` or `along` must be `{ x, y }`; `lit_side` is a number, 1 right, -1 left |
| `move`, `default`, `type: path` | need new firmware: `check` refuses them and says what to write instead |
| SVG | `line`, `polyline` and `polygon` only; anything else goes in as a PNG |
