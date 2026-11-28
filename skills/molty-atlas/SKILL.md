---
name: molty-atlas
description: A pocket atlas of the night sky on your human's Molty, the best known constellations, bright stars, the Moon and planets, double stars, clusters, nebulae, galaxies and meteor showers, each drawn in dots from real data, with where it is now for your human (direction, height, when it is highest) and whether the eye, binoculars or a telescope shows it. Use when the human asks what something in the sky is, where it is, what is up tonight, what to look at with binoculars, or wants to browse the sky ("show me Orion", "where is Saturn", "what's that bright one in the west", "what can I see tonight"), and they have a Molty paired through the molty skill.
---

# Molty atlas

The night sky's best known objects as cards on Molty: ten kinds on the rim, each kind's objects best known first, each
card a drawing from real data and, a tap away, where it is now for your human. Molty browses it all by itself: you send
every kind's objects once (about 90 KB in ten commands) and it needs nothing more while your human turns the rim.
molty-astro stays the skill for the zodiac, aspects and eclipses; this one never reads signs.

Resolve `<skill-dir>` to this file's directory. Needs the **molty** skill paired first, and Molty firmware with `atlas`,
`pkg` and `scene` in `relay.caps`. The sky maths is astronomy-engine (MIT), the copy molty-astro vendors.

## The place: ask once, keep it, never finer than a town

Positions need where your human is. Use what you already know (your memory, their profile). If you don't, ask once, by
voice: "Where are you? A town is enough." Then keep it, with the town's rough coordinates (you know them):

```sh
node "<skill-dir>/scripts/atlas.mjs" place --name Berlin --lat 52.52 --lon 13.40 --tz Europe/Berlin
node "<skill-dir>/scripts/atlas.mjs" place --forget       # when they move, or ask you to forget it
```

It stays in your config, next to the molty skill's pairing (`~/.config/molty/atlas-place.json`), rounded to 0.1 degree
(about 10 km). Molty and the relay never get it, only directions, heights and times worked out from it; it is never
logged. With no place every card still shows its drawing; the where face stays hidden and the script prints `ASK:`.

## Show the sky

```sh
node "<skill-dir>/scripts/atlas.mjs" show orion             # Orion's card (its drawing)
node "<skill-dir>/scripts/atlas.mjs" show saturn --where    # where Saturn is now
node "<skill-dir>/scripts/atlas.mjs" tonight --see binoculars  # what is up now, best known first; opens the first
node "<skill-dir>/scripts/atlas.mjs" open galaxies          # a kind; `open` alone: the ten kinds
node "<skill-dir>/scripts/atlas.mjs" find "ring nebula"     # an id from a name (m57)
```

Options: `--at <ISO time>`, `--order up` (up now first instead of best known first), `--dry-run` (print the commands).
Each run prints `SAY: <one line>`, `FACTS: <json>` and, with no place, `ASK: <question>`. The first time Molty asks your
human to install the atlas package (one tap); say so first, or run `atlas.mjs install` up front. `atlas.mjs status`
says what Molty has.

## What your human can do on Molty

- **The kinds** (Moon and planets, constellations, bright stars, double stars, open clusters, globular clusters,
  nebulae, planetary nebulae, galaxies, meteor showers): the rim chooses one, the middle opens it.
- **A kind:** the rim steps through its objects, best known first, a bead each (lit while it is up); turning fast skips
  five; passing the first ticks firmer. Each step shows that object's card.
- **The middle** turns a card to where it is now (the horizon seen from above, north up and east right, its path, the
  direction and height, when it is highest, rises or sets) and back. Only with a place.
- **The kind's name above** a card: back to the kinds. **Close:** the other button, or a finger held still a second.

You hear where the wheel settled (a `ring` event naming the object, [commands.md](references/commands.md#events-you-get));
never answer each one.

## Speak with it (the molty skill's voice rules hold)

- One short line when you open a card: the `SAY` line ("Orion: south-east, 30 degrees up. Highest at 01:10."). Then
  nothing unless asked: browsing by hand is silent.
- Answer follow-ups from `FACTS` in two sentences at most; the glass carries no explanations, you do, when asked.
- "What's that bright one in the west?": `tonight`, or `show` the brightest planet or star up there.

## Errors

| `molty-atlas:` says | do |
|---|---|
| `unsupported` | this Molty's firmware has no atlas: answer in words, mention an update |
| `nothing called ...` | `find` with fewer words, or say you don't know it |
| `not paired` | pair with the molty skill first |

Agents without a shell send the commands themselves: [commands.md](references/commands.md). Where every number and
drawing comes from, and its licence: [sources.md](references/sources.md). Screens: [atlas](display.yaml#atlas),
[build/display.md](build/display.md).
