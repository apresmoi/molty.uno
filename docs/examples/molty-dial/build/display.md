# Dial: screens

Generated from display.yaml by `molty-display build`. Do not edit: change display.yaml and build again.

A gauge with a pointer and a centre you tap to read. Package `dial`, version 1.0.0. On a Molty without screens: say it in words instead.

Show a screen with `scene.play { pkg, layout, params, ttl_s }` (`scene.queue` plays it after the one showing). `layout` is the screen's name.
Send every input and nothing else. `inputs/<screen>.schema.json` (next to this file) checks params before you send them.

| screen | shows | lasts |
|---|---|---|
| [dial](#dial) | A gauge pointing at one value. | held |

## dial

A gauge pointing at one value. Held until another screen replaces it or its `ttl_s` ends.

| send | type | about |
|---|---|---|
| `value` | angle in degrees | Where the pointer points. |
| `reading` | text a tap shows, up to 160 characters | What a tap on the centre shows. |

A call that works:

```json
{"name":"scene.play","args":{"pkg":"dial","layout":"dial","params":{"value":135,"reading":"Humidity 64 percent, a bit damp."},"ttl_s":60}}
```
