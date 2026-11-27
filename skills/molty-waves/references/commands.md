# Sending the commands yourself

Shaped like molty-rhythm's. All go to `POST $MOLTY_RELAY/v1/devices/<device>/cmd` with
`Authorization: Bearer <token>` (the molty skill's pairing), body `{"name": …, "args": {…}}`.
The tone player shares the beat player's speaker: a Molty with `rhythm` in `relay.caps` has a speaker; firmware
without the tone player answers `unknown_command`.

## Show and play

1. **Install** the `waves` package once, as the rhythm skill installs `rhythm` (`pkg.install` with the entry of
   `packages/index.json`; `scripts/waves.mjs install` does it).
2. **The scope**: `{"name":"scene.play","args":{"pkg":"waves","layout":"scope","params":{},"ttl_s":600}}`.
   It draws whatever Molty's tone player holds; the sound stops when it leaves the screen.
3. **The chord**: `{"name":"wave.play","args":{"chords":[…],"pick":7,"note":0,"midi":60,"tuning":"just"}}`
   - `chords`: the rim's list, 1 to 32, each `{"id":"major","label":"MAJOR","semis":[0,4,7],"ratio":[4,5,6],"spell":"major"}`
     (`scripts/chords.mjs` `wire()` gives all of `data/chords.json`, about 2.6 KB). Up to 5 notes a chord, up to
     48 dots a note; `label` in capitals, up to 24; `spell` the key the note names are spelt in (`major`, `minor`).
   - `pick`: which one; `note`: its root, 0 (C) to 11 (B); `midi`: the root's pitch as a MIDI number, fractions
     allowed, -36 (1.02 Hz) up (clamped where the top note reaches 5760 Hz); or `hz` instead. `tuning`: `just` (the
     whole numbers, the default) or `equal` (the piano's).
   - `paused: true`: stopped on the chooser (a tap in the middle plays it), the chord heard once, plucked.
   - `mute`: notes silent, 1 = the root. Answers `{"chord":"major","note":"C","midi":60,"hz":261.63,"tuning":"just","muted":[],"paused":false}`.
4. **Change it**: `{"name":"wave.set","args":{…}}` with one of `midi`, `hz`, `octave` (the root in that octave),
   `count: true` (the octave where the clock turns about once a second), and any of `note`, `pick`, `tuning`,
   `mute`, `paused`. A pitch change glides there in 30 ms and never restarts; a new chord or note while playing
   starts it again in its octave after a 12 ms fade. `not_playing` before `wave.play`.
5. **Stop**: `{"name":"wave.stop"}`: silence, and the scope leaves the screen.

## Events you get

| event | data | when |
|---|---|---|
| `ring` | `{"spot":"pitch","value":55}` | the finger left the rim after a pitch change (MIDI) |
| `ring` | `{"spot":"chord","index":13,"label":"MAJOR SEVENTH","id":"major-7"}` | stopped, the rim was turned to another chord (its preview played) |
| `ring` | `{"spot":"note","index":1,"label":"Db"}` | at the notes, the rim was turned to another note |
| `tap` | `{"x":0.5,"y":0.13,"spot":"voice-2","muted":true}` | a drum was muted or brought back (its number up top, or its dots while slow) |
| `scene` | `{"why":"close"}` | the human closed it (the other button, or a finger held a second) |

Molty also sends `wave` `{"state":"playing"|"paused"|"stopped","chord","note","hz"}` when the human starts or stops
it; a relay that does not list `wave` among its event types drops it (the `scene` event still says it closed).

## Conventions

- Pitch: equal temperament, A4 = 440 Hz: `hz = 440 * 2^((midi - 69) / 12)`; MIDI 60 is middle C.
- With `just`, note i of the chord plays at `root * ratio[i] / ratio[0]`; with `equal`, at `root * 2^(semis[i] / 12)`.
  The clock turns at `root / ratio[0]` a second either way.
- Dot k of note i sits at `360 * k / ratio[i]` degrees clockwise from 12 o'clock (just tuning); all the dots meet at
  12 once a turn.
