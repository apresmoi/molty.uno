# Sending the commands yourself

All go to `POST $MOLTY_RELAY/v1/devices/<device>/cmd` with `Authorization: Bearer <token>` (the molty skill's pairing),
body `{"name": ..., "args": {...}}`. Cap `atlas` in `relay.caps`; the relay answers `unsupported` without it.
`scripts/atlas.mjs --dry-run` prints exactly what it would send. The full shapes: docs/AGENT-LINK.md "Atlas".

## Open it

1. **Install** the `atlas` package once (`pkg.install`, packages/index.json): one screen, 206 bytes.
2. **Every kind's objects**, one `atlas.load` a kind (at most 32 KB each, about 90 KB in all), the first with the
   kinds: `{"kinds":[{"id":"constellation","name":"CONSTELLATIONS","tint":"#9fc0ff","count":88,"up":36}, ...],
   "clock":1320,"kind":"constellation","objects":[...]}`. `clock`: the minutes of your human's day now (22:00 is
   1320), or -1 with no place. An object:
   `{"id":"con-ori","name":"Orion","facts":"2 HANDS WIDE","see":"eye","d":{"k":"stars","p":"<base64>","l":"<base64>"},
   "w":{"p":"<base64>","step":45,"t":[1226,190,584],"ha":44}}` (`w` only with a place: its path from now, 16 points
   every 45 minutes, and when it rises, is highest and sets, in minutes from now).
3. **The screen**: `{"name":"scene.play","args":{"pkg":"atlas","layout":"atlas","params":{},"ttl_s":600}}`.
4. **Where to start**: `{"name":"atlas.show","args":{"kind":"constellation","level":"objects","index":0,"face":"where"}}`
   (`level: "kinds"` for the ten kinds; `face: "drawing"` for the card's drawing).

Molty frees what it was sent 2 s after the screen goes. Send it again for a new evening (the paths cover 12 hours).

## Events you get

| event | data | when |
|---|---|---|
| `ring` | `{"spot":"object","index":7,"label":"ORION","id":"con-ori"}` | the rim settled on another object |
| `ring` | `{"spot":"kind","index":1,"label":"CONSTELLATIONS","id":"constellation"}` | at the kinds, on another kind |
| `scene` | `{"why":"close"}` | your human closed it (the other button, or a finger held a second) |

## Conventions

- Directions: azimuth from north through east; on the dome north is up and east right (Molty on a desk, as a compass).
- Heights: degrees above the horizon. Times: the place's local time, 24 hours, from `clock`.
- Size in the sky: full Moons (half a degree) for deep sky objects, hands at arm's length (about 20 degrees) for
  constellations. Brightness: magnitude.
