---
name: molty-rhythm
description: Play a rhythm on your human's Molty as a clock that keeps time by itself, each drum on its own ring with its own sound, a hand that walks the steps, a tap in the middle to play and stop, and a rim they turn to choose a rhythm (stopped) or the tempo (playing). Twelve verified rhythms (chacarera, clave, bossa nova, the 12/8 bell, solea, aksak and more). Use when the human asks to see, hear, learn or practise a rhythm or a tempo ("show me chacarera at 100", "slower", "a clave"), and they have a Molty paired through the molty skill.
---

# Molty rhythm

A rhythm drawn as a clock ([clock](display.yaml#clock), on the rhythm package; its call:
[build/display.md](build/display.md)) and played on Molty's speaker. You pick it and send it **once**: the
loop, the drum sounds, the hand, the play and stop, the wheel and the mutes all run on Molty, timed
by its own speaker. Nothing beat by beat crosses the network.

Resolve `<skill-dir>` to this file's directory. Needs the **molty** skill paired first, and Molty
firmware with `pkg`, `scene` and `rhythm` in `relay.caps` (no `rhythm`: answer in words and mention a
firmware update).

## Play

```sh
node "<skill-dir>/scripts/rhythm.mjs" play chacarera --bpm 100   # the clock, a count-in, then it plays
node "<skill-dir>/scripts/rhythm.mjs" set --bpm 90                # slower, at once, without restarting
node "<skill-dir>/scripts/rhythm.mjs" set --rhythm son-clave      # another rhythm, after a count-in
node "<skill-dir>/scripts/rhythm.mjs" set --next                  # the next bar of the phrase
node "<skill-dir>/scripts/rhythm.mjs" stop                        # silence; the creature comes back
```

Also: `play <id> --mute aro` (that drum silent), `--still` (stopped, ready to choose: a tap in the
middle plays it), `--buzz` (a short buzz on each main beat, see below), `--ttl <s>` (how long it
shows, 600 at most), `--tick <percent> --tick-ms <ms>` (the rim's tick on the motor, only when asked
to tune it; Molty's own otherwise); `set --mute aro,parche | none`, `set --pause` (stopped, choosing)
`| --resume` (plays, after a count-in), `set --buzz on|off`; `list` (the library), `--dry-run` (print
the commands, send nothing).

Each `play` prints `SAY: <one line>` and `FACTS: <json>` (the drums, the beats, the source).

## What your human can do on Molty

- **Tap the middle**: it plays (after a count-in); tap again: it stops. The middle shows a play
  mark while stopped, a small stop mark while it plays.
- **Stopped, turn the rim** (a drag round the edge): choose a rhythm, one bead a rhythm, its name in
  the middle and its clock drawn at once. No sound until the tap.
- **Playing, turn the rim**: the tempo, 2 beats a minute a click (4 turning fast), 40 to 220, the
  number in the middle while it turns. It keeps playing. Molty ticks the motor once a click.
- **Tap a drum's dot**: mute that drum; tap again to bring it back.
- **Accept button**: play and stop, as the middle. **The other button**: stop and leave.

You hear where they settled as events ([events](references/commands.md#events-you-get)): a new tempo,
a rhythm chosen, a muted drum, played or stopped, gone. Use them for what you say next; never answer
each one.

## Speak with it (the molty skill's voice rules hold)

- **One short line when it starts**, the `SAY` line: the rhythm and the tempo (with `--still`: how
  to start it). Then be quiet:
  Molty pauses the rhythm while it speaks or listens, and starts it again after a count-in.
- On "slower", "faster", "at 120", "the clave instead": `set`, and say at most the new tempo or name.
- Explain only when asked, in two sentences at most, from `FACTS` and
  [the library](references/library.md). Say "one common way to play it": folk rhythms vary by region
  and player.

## The library

Twelve rhythms, each confirmed by two sources (`list` shows them): chacarera, chacarera trunca,
carnavalito, milonga campera, tango marcato, habanera, son clave, rumba clave, bossa nova, the 12/8
bell, solea, aksak 9/8. The data format, the sources and what was left out: [library.md](references/library.md).

## Install, once

The first `play` installs the rhythm package if Molty lacks it: **Molty asks your human on its
screen** (`install` or `not now`); the script waits for the tap. So the first time, say so before you
run it: `talk {"say":"Molty will ask to install the rhythm pack: tap install.","listen":false}`.
Or up front: `rhythm.mjs install`. `rhythm.mjs status` lists what Molty has.

## The buzz on the main beats

Off unless asked. With `--buzz` (or `set --buzz on`) the motor gives a short buzz on each main beat,
on the boards that have one. While it buzzes Molty cannot feel a shake, so shake to talk waits.
Turn it on only when your human asks for it.

## Errors

| `molty-rhythm:` says | do |
|---|---|
| `unsupported` | Molty's firmware has no rhythm player yet: answer in words, mention a firmware update |
| `no_speaker` | this Molty has no speaker: the clock still shows, silently |
| `declined`, `pkg_timeout` | the install did not happen: don't ask again unless they bring it up |
| `not_playing` | nothing plays: `play` first |
| `not paired` | pair with the molty skill first |
| `device_offline` | answer in words |

Agents without a shell send the commands themselves: [commands.md](references/commands.md).
