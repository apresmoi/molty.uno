---
name: molty-waves
description: Show and play a note, an interval or a chord on your human's Molty as one drum hit at different speeds. Each note's hits are dots on one circle, joined as a line, a triangle, a square; turn the rim and the hits speed up until they fuse into a pitch and the circle's outline becomes the wave. A polyrhythm you can count (three against two) turns into the chord you hear (a fifth). 12 notes with sharps and flats, 29 chord kinds from a single note to a seven sharp nine. Use when the human asks to see, hear or understand a note, an interval or a chord ("show me C major seventh", "what does a tritone look like", "why does a fifth sound smooth", "slow it down until I can count it"), and they have a Molty paired through the molty skill.
---

# Molty waves

A chord drawn on one circle (the waves package's scope, [build/display.md](build/display.md)) and played on Molty's speaker with one drum: each note is that drum hit at its own
speed, its hits the dots on the circle joined as a shape (its whole number in the chord: C major is 4, 5 and 6, a
square, a pentagon and a hexagon). Fast, the circle's outline is the sound's wave. You send it **once**: the tone
player, the clock, the wheel, the previews, the mutes all run on Molty. Nothing note by note crosses the network.

## Play

```sh
node "<skill-dir>/scripts/waves.mjs" show C maj7            # the chooser on C, stopped: a tap in the middle plays it
node "<skill-dir>/scripts/waves.mjs" play C major           # C major, middle of the piano, as a sounding wave
node "<skill-dir>/scripts/waves.mjs" play C tritone --count # slow enough to count: about a turn a second
node "<skill-dir>/scripts/waves.mjs" set --count            # slower, until the dots can be counted
node "<skill-dir>/scripts/waves.mjs" set --octave 4         # back up to the middle of the piano
node "<skill-dir>/scripts/waves.mjs" set --chord minor      # another chord on the same note
node "<skill-dir>/scripts/waves.mjs" set --tuning equal     # as a piano is tuned: the waves drift and beat
node "<skill-dir>/scripts/waves.mjs" stop                   # silence; the creature comes back
```

Also `--hz <n>` (the root's pitch, 1 to 5760), `set --mute 2` (that note silent, counted from the root, 1 = root),
`--mute none`, `set --pause` / `--resume`, `list` (every chord kind, its notes on C, its whole numbers). Notes: C,
C#, Db, D ... B (a flat is a lowercase b). Chords: `list`, or [data/chords.json](data/chords.json). `play` and
`show` install the waves package first when Molty lacks it (your human taps once to allow it).

## What your human can do on Molty

- **Turn the rim, stopped**: choose what plays (alone, octave, fifth, fourth, the thirds, tritone, major, minor, sus,
  diminished, augmented, the sevenths, sixths, ninths, seven sharp nine, fourths, a cluster). Each choice plays once,
  plucked, at the pitch it last played (middle C at first). The note up top is wherever that pitch is.
- **Tap the middle** (or the accept button): it plays, at the pitch they just heard; again: it stops, back to the
  choice, at the same pitch.
- **Turn the rim, playing**: the pitch moves in fine steps (a quarter of a semitone a click), with a firmer tick and
  a slight hold on each note, so a note is easy to find by feel; from C5 a semitone a click, from C6 a whole tone.
  Under about 20 a second it moves in tenths of a hit a second, then fifths, then whole ones. Turning fast it jumps
  an octave a click, with the strongest tick. Down: the hits slow until they can be counted. Up: past about 20 a
  second they fuse into a note and the outline becomes the wave. The middle reads hits a second, then hertz and the
  note with its octave (A1, A2, A3), and between notes how far off it is ("+25").
- **Tap a number up top** (or a drum's dots while slow): mute that drum; again to bring it back.
- **Close it**: the other button, or a finger held still on the glass for a second.

You hear what they did as events ([commands.md](references/commands.md#events-you-get)). Use them for what you say
next; never answer each one.

## Speak with it (the molty skill's voice rules hold)

- One short line when it starts: the `SAY` line. Then be quiet: Molty pauses the sound while it speaks or listens.
- Molty's screen carries no explanation; you do, only when asked, two sentences at most: "It is one drum. Past
  about twenty hits a second you stop hearing hits and hear a note." Or: "Four, five and six: the major chord is
  the harmonic series itself."
- On "slower", "count it": `set --count`. On "as a note", "higher": `set --octave 4` or `--hz`.

## The numbers

A chord's dots a turn are its smallest whole numbers within 18 cents of the piano's chord (major 4:5:6, minor
10:12:15, major seventh 8:10:12:15); the dominant seventh and ninth take the classic 4:5:6:7(:9). With
`--tuning just` (the default) the sound uses those numbers exactly, so the wave stands still and the rhythm loops;
`--tuning equal` plays the piano's tuning, and the wave slowly turns: what the ear hears as beating.

## Errors

| `molty-waves:` says | do |
|---|---|
| `unsupported` | no tone player on this Molty (no `wave` cap: older firmware, or no speaker): answer in words, mention an update |
| `not paired` | pair with the molty skill first |

Agents without a shell send the commands themselves: [commands.md](references/commands.md).
