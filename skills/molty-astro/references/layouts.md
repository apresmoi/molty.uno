# The astrology package's layouts and params

For agents that send the commands themselves (no shell, no node). `scripts/astro.mjs` and
`scripts/sky.mjs` do exactly this; read them as the reference.

## Commands

All go to `POST $MOLTY_RELAY/v1/devices/<device>/cmd` with `Authorization: Bearer <token>`
(the molty skill's pairing), body `{"name": …, "args": {…}}`.

1. **Index.** `GET https://raw.githubusercontent.com/apresmoi/molty.uno/main/packages/index.json`
   → `{"astrology": {name, version, kind, path, size, sha256}}`.
2. **Installed?** `{"name":"pkg.list"}` → `{packages:[{id, version, …}]}`. Skip 3 when `astrology`
   is there at the index's version.
3. **Install** (Molty asks its human with one tap; tell them first):
   `{"name":"pkg.install","args":{"id":"astrology","url":"https://raw.githubusercontent.com/apresmoi/molty.uno/main/<path>","sha256":"<sha256>","size":<size>,"name":"Astrology"},"wait":true}`
   → `{installed, version}`, or `202 {job, status:"pending"}`: poll `GET $MOLTY_RELAY/v1/jobs/<job>?wait_s=8`
   (about 2 min at most; errors `declined`, `pkg_timeout`, `hash_mismatch`, …).
4. **Play**: `{"name":"scene.play","args":{"pkg":"astrology","layout":"globe","params":{…},"ttl_s":8}}`,
   then `scene.queue` for each next one (at most 8 wait). Keep `params` small (a few hundred bytes; at most 2048).
   Every declared param must be sent, nothing else; a list holds at most 32 flat objects. `not_installed` → install again, then play.
5. `{"name":"scene.clear"}` melts back to the creature; `{"name":"pkg.remove","args":{"id":"astrology"}}` uninstalls.

## Conventions

- Glass units: the round screen has radius 1, `x` right, `y` up. Angles in degrees, 0 = right,
  counter-clockwise.
- **Chart angle** of an ecliptic longitude `lon` (tropical, geocentric, of date):
  `(180 + lon) mod 360`, so 0 Aries sits at 9 o'clock and the zodiac runs counter-clockwise.
- Body names in params are lower case: `sun moon mercury venus mars jupiter saturn uranus neptune pluto`.
  Glyph assets are `g-<body>`; globe images are `sun moon moon-red mercury venus mars jupiter saturn`;
  sign assets are the sign names, `aries` … `pisces`.
- Round numbers to 2 or 3 decimals.

## tonight: globe, saturn, sign (play one, queue the rest)

Seven scenes: `globe` for the Moon, then Mercury, Venus, Mars, Jupiter; `saturn`; `sign` for the Moon's sign.

| layout | params |
|---|---|
| `globe` (2.5 s) | `img` (`moon`, `mercury`, `venus`, `mars`, `jupiter`), `lit` (illuminated fraction, 0..1), `side` (Moon: `1` waxing, `-1` waning; a planet: `1` when east of the Sun, `(lon - sunLon) mod 360 < 180`, else `-1`), `r` (0.62 for the Moon, 0.58 for a planet), `spin` (degrees per second; 0 for the Moon) |
| `saturn` (2.5 s) | `lit`, `side` as above; `rings`: Saturn's ring tilt seen from Earth, bucketed, `rings-p<t>` (tilt >= 0) or `rings-n<t>`, `t` the nearest of 3, 9, 15, 21, 27 degrees |
| `sign` (4 s) | `sign`: the Moon's sign, e.g. `leo` |

## chart (held until `ttl_s`)

| param | value |
|---|---|
| `bodies` | ten `{"g": "g-<body>", "a": <chart angle>, "r": <radius>, "c": "<colour>"}`; radius 0.64, pulled in by 0.11 per level when a body is within 13 degrees of one already placed at that level (place in order of longitude); colours as in `aspect` |
| `inner` | the smallest radius minus 0.07 (the ring the aspect lines end on) |
| `sextiles`, `squares`, `trines`, `oppositions` | lists of `{"x1", "y1", "x2", "y2"}`: a line between two bodies' chart angles on the `inner` ring (conjunctions have none). Tightest orb first; leave the loosest off until the params fit in about 900 bytes |

## signs (36 s)

No params: all twelve, each in turn, all twelve again.

## aspect (5 s each; queue one per kind)

One pair on the degree grid: the tightest pair in that aspect within a 6 degree orb, or the next
date it comes within 1 degree.

| param | value |
|---|---|
| `a` | chart angle of body A |
| `b` | `a + d`, where `d` is B's chart angle minus A's, wrapped to -180..180 (the arc runs from `a` to `b`) |
| `c` | the aspect's colour: conjunction `#ffd678`, sextile `#6fe0a0`, square `#ff6a55`, trine `#7fb0ff`, opposition `#ff5fa0` |
| `bodies` | `[{"g": "g-<A>", "a": a, "r": 0.6, "c": <A's colour>}, {"g": "g-<B>", "a": <B's chart angle>, "r": 0.47 when abs(d) < 14 else 0.6, "c": <B's colour>}]`; colours: sun `#ffd678`, moon `#e6e6e0`, mercury `#c8bcb0`, venus `#fff0c4`, mars `#ff7a55`, jupiter `#ffc266`, saturn `#e8d890`, uranus `#9fe3e0`, neptune `#8fb4ff`, pluto `#c0a0d0` |
| `lines` | `{"x1", "y1", "x2", "y2", "c", "on"}`: the aspect's polygon on radius 0.6 from angle `a` (sextile 6 sides, square 4, trine 3; `c` `#565b72`, `on` 0), then the chord from A to B on radius 0.6 (`c` the aspect's colour, `on` 1.2; none for a conjunction) |
| `deg` | the separation in whole degrees, as text, e.g. `"121"` (the device font is ASCII: no degree sign) |
| `tx`, `ty` | where the number sits: at angle `a + d/2`, radius 0.14; `0, 0` for a conjunction |

## Events (6 s each)

Each moving body is a `path`: 14 keyframes `{"x", "y", "on", "off"}`, sampled evenly over the
event; keyframe `i` shows from `on = 0.3 + 5.4 i / 14` to the next one's `on` (the last to 6).
Positions are offsets on the sky from the still body (north up, east left), scaled as below.

| layout | params |
|---|---|
| `transit` | `path`: Mercury or Venus (whichever is next) against the Sun, from 15 % before the transit's start to 15 % after its end; scale 0.55 / the Sun's angular radius |
| `occultation` | `planet` (`venus`, `mars`, `jupiter` or `saturn`), `path`: the Moon against it, 2.2 h either side; scale 0.45 / the Moon's angular radius |
| `appulse` | `a`, `b` (two of mercury..saturn passing within 0.5 degrees, A at least 12 degrees from the Sun), `path`: B against A, 4 days either side; scale 0.32 / max(separation, 0.08) |
| `solar-eclipse` | `moon_r` (the Moon's radius, Sun = 0.5), `path`: the Moon against the Sun, 2.6 h either side (from the point of greatest eclipse when it has one); scale 0.5 / the Sun's angular radius |
| `lunar-eclipse` | `umbra`, `pen` (Earth's shadow radii), `moon_r`, `path` with an extra `"img"` per keyframe: `moon-red` inside the umbra, else `moon`; the Moon against the anti-Sun, 3.2 h either side; scale 0.8 / the penumbra's radius |
