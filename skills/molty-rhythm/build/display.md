# Rhythm: screens

Generated from display.yaml by `molty-display build`. Do not edit: change display.yaml and build again.

A rhythm Molty plays and draws as a clock, a ring a drum, a rim to turn for the tempo. Play it with the molty-rhythm skill. Package `rhythm`, version 1.0.0. On a Molty without screens: say it in words instead.

Show a screen with `scene.play { pkg, layout, params, ttl_s }` (`scene.queue` plays it after the one showing). `layout` is the screen's name.
Send every input and nothing else. `inputs/<screen>.schema.json` (next to this file) checks params before you send them.

| screen | shows | lasts |
|---|---|---|
| [clock](#clock) | The rhythm Molty plays, as a clock, from the speaker's own clock. Turn the rim for the tempo; tap the centre to choose another rhythm with the rim; tap a drum's dot to mute it. | held |

## clock

The rhythm Molty plays, as a clock, from the speaker's own clock. Turn the rim for the tempo; tap the centre to choose another rhythm with the rim; tap a drum's dot to mute it. Held until another screen replaces it or its `ttl_s` ends.

No inputs: send `"params": {}`.

A call that works:

```json
{"name":"scene.play","args":{"pkg":"rhythm","layout":"clock","params":{},"ttl_s":60}}
```
