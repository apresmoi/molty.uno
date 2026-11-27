# Waves: screens

Generated from display.yaml by `molty-display build`. Do not edit: change display.yaml and build again.

A chord Molty plays with one drum and draws on one circle, its outline the wave. Tap the middle to play or stop; turn the rim to choose or for the pitch. Package `waves`, version 1.0.0. On a Molty without screens: say it in words instead.

Show a screen with `scene.play { pkg, layout, params, ttl_s }` (`scene.queue` plays it after the one showing). `layout` is the screen's name.
Send every input and nothing else. `inputs/<screen>.schema.json` (next to this file) checks params before you send them.

| screen | shows | lasts |
|---|---|---|
| [scope](#scope) | The chord Molty plays, on one circle, from the speaker's own clock. Tap the centre to play and to stop; stopped, turn the rim to choose a chord (tap the note up top for the twelve notes); playing, turn it for the pitch and tap a number up top to mute that voice. | held |

## scope

The chord Molty plays, on one circle, from the speaker's own clock. Tap the centre to play and to stop; stopped, turn the rim to choose a chord (tap the note up top for the twelve notes); playing, turn it for the pitch and tap a number up top to mute that voice. Held until another screen replaces it or its `ttl_s` ends.

No inputs: send `"params": {}`.

A call that works:

```json
{"name":"scene.play","args":{"pkg":"waves","layout":"scope","params":{},"ttl_s":60}}
```
