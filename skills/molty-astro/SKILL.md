---
name: molty-astro
description: Show the real sky on your human's Molty: tonight's moon and planets, the full zodiac chart, the twelve signs, aspects, and the next eclipses, transits and close passes, by installing Molty's astrology package once and sending a few computed params per scene. Use when the human asks about the moon, the planets, their sign, the zodiac, a horoscope-style reading, aspects, eclipses or "what's in the sky tonight", and they have a Molty paired through the molty skill.
---

# Molty astro

The sky drawn on Molty's round screen. Molty knows how to draw (the **astrology package**:
signs, planet symbols, moon and planet globes, layouts); this skill knows the sky: it computes
positions here, with astronomy-engine (MIT, vendored), and sends Molty only a few numbers per
scene. Nothing about astrology runs on Molty or on the relay.

Resolve `<skill-dir>` to this file's directory. Needs the **molty** skill paired first (its
pairing stores the device and token this skill reads), and Molty firmware with `pkg` and
`scene` in `relay.caps`.

## Show the sky

```sh
node "<skill-dir>/scripts/astro.mjs" tonight          # moon, then Mercury..Saturn, then the moon's sign (7 scenes, 19 s)
node "<skill-dir>/scripts/astro.mjs" chart            # all ten bodies on the zodiac wheel with their aspects (held 60 s)
node "<skill-dir>/scripts/astro.mjs" signs            # the twelve signs (36 s)
node "<skill-dir>/scripts/astro.mjs" aspects [trine]  # each aspect on the degree grid, or one (5 s each)
node "<skill-dir>/scripts/astro.mjs" events [lunar-eclipse]  # next transit, occultation, close pass, solar and lunar eclipse (6 s each)
```

Options: `--date 2026-11-01` (a bare date means 21:00 UTC that day; default today) or an ISO
instant, `--ttl <s>` (how long a held scene stays, max 600), `--dry-run` (print the commands, send
nothing). Event kinds: `transit`, `occultation`, `appulse`, `solar-eclipse`, `lunar-eclipse`.

Each run prints `SAY: <one line>` and `FACTS: <json>`. The first scene replaces the screen,
the rest queue behind it; Molty melts back to its creature when they end.

## Install, once

The first scene installs the package if Molty lacks it (or has an older one): **Molty asks your
human on its screen** (two buttons: "Astrolog" and "no") and waits up to a minute for one tap; the
script waits for the outcome (about 2 min at most). So the first time, say
so before you run it: `talk {"say":"Molty will ask to install the astrology pack: tap yes.","listen":false}`.
Or install up front: `astro.mjs install`. `astro.mjs status` lists what Molty has;
`astro.mjs uninstall` removes it (Molty frees the space; ask your human first).

Molty downloads the file itself, from this repo (`packages/index.json` names it, with its size
and sha256): the relay never carries it, and Molty refuses a file whose hash differs.

## Speak with it (the molty skill's voice rules hold)

- **Speak first.** Woken by "what's the moon doing?": run the scene (it takes a second), then one
  `talk` with the `SAY` line, `"listen": false` unless you want their answer. Needs a long
  search (an event years out)? `talk {"say":"Looking at the sky. [working]"}` first.
- **Short.** At most 2 sentences, ~25 words a talk. The screen carries the detail: never read out
  every planet. Use `FACTS` to answer follow-ups.
- **Don't talk over a scene with words on screen**: `show` and a `say`'s words take the screen.
  Speak once, let the scene play.
- **Entertainment.** Positions are real (tropical zodiac, geocentric); any reading is a sample for
  fun. No houses or ascendant: those need a birth place and time, which this skill never asks for.

## Reading the sky

- **Moon**: percent lit and waxing (growing, lit on the right as seen from the north) or waning.
  Its sign is the zodiac sign of its longitude: "Moon in Leo".
- **Planets**: "Mars, 19 degrees Leo" = 19 degrees into Leo's 30. Mercury and Venus show as thin
  crescents when they pass between us and the Sun.
- **Aspects**: angles between two bodies along the zodiac, within 6 degrees (the orb): conjunction
  0, sextile 60, square 90, trine 120, opposition 180. Tightest first.
- **Events**: the next real ones from the date, Earth-centred; local visibility is not checked.

## Errors

| `molty-astro:` says | do |
|---|---|
| `unsupported` | Molty's firmware has no packages yet: answer in words, mention a firmware update ([molty skill: updates](https://github.com/apresmoi/molty.uno/blob/main/skills/molty/references/updates.md)) |
| `declined` | the human said no, or nobody tapped in time (the message says which): don't ask again unless they bring it up |
| `pkg_timeout` | Molty never reported the install: `astro.mjs status` shows whether it landed |
| `hash_mismatch`, `bad_url`, `too_big`, `bad_package`, `download`, `tls`, `http_<code>` | the download or the file went wrong: retry later, tell your human once |
| `no_space`, `busy` | Molty is full or installing something else: say so |
| `not paired` | pair with the molty skill first |
| `device_offline` | answer in words |

A wiped store (a factory flash) is handled: the script reinstalls once and plays again.

## Harnesses

- **Claude Code, Codex**: the molty plugin ships this skill next to `molty`; needs `node` 18+.
- **OpenClaw and other long-lived agents**: install `skills/molty-astro` beside `skills/molty`
  from `https://github.com/apresmoi/molty.uno/tree/main/skills`; same scripts.
- **Grok and other hosted bots without a shell**: send the commands over REST yourself
  ([layouts.md](references/layouts.md)): `pkg.install` from `packages/index.json`, then
  `scene.play` with params you compute with your own code tool (any ephemeris, e.g.
  astronomy-engine for Python or JS). [layouts.md](references/layouts.md) defines every param.

## References

- [layouts.md](references/layouts.md): every layout and its params, the commands in full, for agents without a shell.
