# Rhythm: screens

Generated from display.yaml by `molty-display build`. Do not edit: change display.yaml and build again.

A rhythm Molty plays and draws as a clock, a ring a drum. Tap the middle to play or stop; turn the rim to choose or for the tempo. Package `rhythm`, version 1.1.0. On a Molty without screens: say it in words instead.

Show a screen with `scene.play { pkg, layout, params, ttl_s }` (`scene.queue` plays it after the one showing). `layout` is the screen's name.
Send every input and nothing else. `inputs/<screen>.schema.json` (next to this file) checks params before you send them.

| screen | shows | lasts |
|---|---|---|
| [clock](#clock) | The rhythm Molty plays, as a clock, from the speaker's own clock. Tap the centre to play and to stop; stopped, turn the rim to choose a rhythm; playing, turn it for the tempo and tap a drum's dot to mute it. | held |

## clock

The rhythm Molty plays, as a clock, from the speaker's own clock. Tap the centre to play and to stop; stopped, turn the rim to choose a rhythm; playing, turn it for the tempo and tap a drum's dot to mute it. Held until another screen replaces it or its `ttl_s` ends.

| send | type | about |
|---|---|---|
| `tick_ms` | number, 0 to 60 | How long the motor ticks for each click of the rim, 5 to 60 ms; 0 for Molty's own (22). |
| `tick` | number, 0 to 100 | How strong that tick is, 25 to 100 percent; 0 for Molty's own (80). |

A call that works:

```json
{"name":"scene.play","args":{"pkg":"rhythm","layout":"clock","params":{"tick_ms":0,"tick":0},"ttl_s":60}}
```
