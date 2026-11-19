# Sending the commands yourself

For agents with no shell and no node. `scripts/astro.mjs` and `scripts/sky.mjs` do exactly this;
read them as the reference. Every screen, its inputs and a call that works:
[build/display.md](../build/display.md).

## Commands

All go to `POST $MOLTY_RELAY/v1/devices/<device>/cmd` with `Authorization: Bearer <token>`
(the molty skill's pairing), body `{"name": …, "args": {…}}`.

1. **Index.** `GET https://raw.githubusercontent.com/apresmoi/molty.uno/main/packages/index.json`
   → `{"astrology": {name, version, kind, path, size, sha256}}`.
2. **Installed?** `{"name":"pkg.list"}` → `{packages:[{id, version, …}]}`. Skip 3 when `astrology`
   is there at the index's version.
3. **Install** (Molty asks its human "INSTALL ASTROLOGY?" with `install` and `not now` on the rim; tell them
   first. After the tap a ring grows round the screen while Molty downloads the file, and flashes when it is in):
   `{"name":"pkg.install","args":{"id":"astrology","url":"https://raw.githubusercontent.com/apresmoi/molty.uno/main/<path>","sha256":"<sha256>","size":<size>,"name":"Astrology"},"wait":true}`
   → `{installed, version}`, or `202 {job, status:"pending"}`: poll `GET $MOLTY_RELAY/v1/jobs/<job>?wait_s=8`
   (about 2 min at most; errors `declined`, `pkg_timeout`, `hash_mismatch`, …).
4. **Play**: `{"name":"scene.play","args":{"pkg":"astrology","layout":"globe","params":{…},"ttl_s":8}}`,
   then `scene.queue` for each next one (at most 8 wait). Keep `params` small (a few hundred bytes, more with readings; at most 8192).
   Every input must be sent, nothing else; a list holds at most 32 flat objects. `not_installed` → install again, then play.
   Molty downloads the file itself, from this repo: the relay never carries it, and Molty refuses a file whose hash differs.
5. `{"name":"scene.clear"}` melts back to the creature; `{"name":"pkg.remove","args":{"id":"astrology"}}` uninstalls.

## Conventions

- The round screen has radius 1, `x` right, `y` up. Angles in degrees, 0 = right, counter-clockwise.
- **Chart angle** of an ecliptic longitude `lon` (tropical, geocentric, of date):
  `(180 + lon) mod 360`, so 0 Aries sits at 9 o'clock and the zodiac runs counter-clockwise.
- Body names are lower case: `sun moon mercury venus mars jupiter saturn uranus neptune pluto`.
  A body's symbol is `g-<body>`; the globes are `sun moon moon-red mercury venus mars jupiter saturn`;
  a sign's symbol is its name, `aries` … `pisces`.
- Round numbers to 2 or 3 decimals.

## What to play

| the human asks for | play, then queue |
|---|---|
| tonight | `globe` for the Moon, then Mercury, Venus, Mars, Jupiter; `saturn`; `sign` for the Moon's sign (7 screens) |
| the chart | `chart`, held until its `ttl_s` |
| the signs | `signs` (36 s, no inputs) |
| an aspect | `aspect` (5 s each, one per kind): the tightest pair in that aspect within a 6 degree orb, or the next date it comes within 1 degree |
| an event | `transit`, `occultation`, `appulse`, `solar-eclipse` or `lunar-eclipse` (6 s each): the next real one from the date |

## Taps on the chart

A tap sends you a `tap` event with `"spot": "<id>"` (`sun`, `gemini`, `sun-moon`). What was tapped grows and stays
sharp while its reading shows in the middle of the wheel, one word at a time; Molty does that by itself.

A sign takes a tap anywhere in its 30 degree block of the zodiac ring, and Molty then opens `sign-closeup` itself,
from what the chart was sent (the sign's own fields, its `ruler`, the bodies `of` it), for the sign's `ttl`. It shows
the sign's reading as the subtitle and goes back to the chart after it, or at a tap beside it. The tap event says so:
`opened: "sign-closeup"`.

To read a sign aloud, poll `GET $MOLTY_RELAY/v1/devices/<device>/events?wait_s=25` (the molty skill's long-poll):

- a `tap` with a sign's `spot` and `opened` → `say` the reading (the sign's line, then each planet's; your words then
  take the subtitle). Molty comes back by itself: `closed: true` on a tap, or a `scene` event, says it is back.
- a `tap` with a sign's `spot` and no `opened` (a Molty that did not open it) → `scene.play` `sign-closeup` with the
  reading's length plus 3 s as `ttl_s`, `scene.queue` the chart behind it, then `say` the reading.
- a `tap` with no `spot` while a close-up you sent shows → `scene.play` the chart.
