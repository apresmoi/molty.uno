# Sending the commands yourself

For agents with no shell and no node. `scripts/rhythm.mjs` and `scripts/library.mjs` do exactly
this; read them as the reference.

All go to `POST $MOLTY_RELAY/v1/devices/<device>/cmd` with `Authorization: Bearer <token>` (the
molty skill's pairing), body `{"name": …, "args": {…}}`.

## Show and play

1. **Install** the `rhythm` package once, exactly as the astro skill installs `astrology`
   (`pkg.list`, then `pkg.install` with the entry of `packages/index.json`; Molty asks its human).
2. **The clock**: `{"name":"scene.play","args":{"pkg":"rhythm","layout":"clock","params":{},"ttl_s":600}}`.
   It draws whatever Molty's beat player holds; the sound stops when the clock leaves the screen.
3. **The rhythm**: `{"name":"rhythm.play","args":{"rhythms":[…],"pick":0,"bpm":100}}`.
   - `rhythms`: the library, 1 to 16 of them, each as `library.mjs` `wire()` makes it:
     `{"id":"chacarera","label":"CHACARERA","steps":6,"per":3,"beats":[1,4],"count_in":1,
     "layers":[{"id":"aro","sound":"rim","color":"#9fc0ff","hits":[1,4]},
     {"id":"parche","sound":"head","color":"#ff9a7a","hits":[3,5],"accent":[5]}],
     "bars":[{"name":"A","hits":[[1,4],[3,5]]},{"name":"B","hits":[[],[3,5]]}],"order":[0,0,1,0]}`.
     `lengths` (one a step) and `swing` (0 to 0.33) are optional. Up to 32 steps, 4 layers, 8 bars, 16 in an order.
   - `pick`: which one plays; `bpm` 40 to 220; optional `mute` (layer ids), `buzz` (true: the motor
     on each main beat), `paused` (true: the clock at rest until the accept button).
   - Answers `{"rhythm":"chacarera","bpm":100}`. Errors: `invalid_args`, `unsupported` (no `rhythm` in `relay.caps`).
4. **Change it**: `{"name":"rhythm.set","args":{…}}` with any of `bpm`, `pick`, `mute` (the whole
   list, `[]` for none), `next: true` (the next bar of the phrase, at the bar line), `buzz`, `paused`.
   A tempo change takes effect at once and never restarts; a new `pick` starts after a count-in.
   Answers the player's state `{rhythm, bpm, muted, paused, buzz}`; `not_playing` when nothing was played.
5. **Stop**: `{"name":"rhythm.stop"}`: silence, and the clock leaves the screen.

## Events you get

Molty handles every click and tap itself and tells you where things settled:

| event | data | when |
|---|---|---|
| `ring` | `{"spot":"tempo","value":112}` | the finger left the rim after a tempo change |
| `ring` | `{"spot":"rhythm","index":6,"id":"son-clave"}` | a rhythm was chosen with the rim and now plays |
| `tap` | `{"spot":"aro","muted":true}` | a drum's dot was tapped (muted, or back) |
| `rhythm` | `{"state":"paused","rhythm":"chacarera","bpm":104}` | `playing`, `paused` (the accept button), `stopped` (the other button, or the clock went) |

## Conventions

- One beat is `per` units; a step is one unit unless `lengths` says otherwise. A bar lasts
  `sum(lengths) / per` beats, `60 / bpm` seconds each.
- Step angles: step 1 at 12 o'clock, each step `360 * its start / the bar's length` further clockwise.
