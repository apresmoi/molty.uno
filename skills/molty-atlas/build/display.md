# Sky atlas: screens

Generated from display.yaml by `molty-display build`. Do not edit: change display.yaml and build again.

The night sky's best known objects as cards drawn in dots, and where each is now. Turn the rim to browse, tap the middle to open or turn. Package `atlas`, version 1.0.0. On a Molty without screens: say it in words instead.

Show a screen with `scene.play { pkg, layout, params, ttl_s }` (`scene.queue` plays it after the one showing). `layout` is the screen's name.
Send every input and nothing else. `inputs/<screen>.schema.json` (next to this file) checks params before you send them.

| screen | shows | lasts |
|---|---|---|
| [atlas](#atlas) | The sky atlas atlas.load sent. Turn the rim through the kinds, tap the middle to open one; turn it through the kind's objects, each its card; tap the middle to turn a card to where it is now (with a place); tap the kind above to go back. | held |

## atlas

The sky atlas atlas.load sent. Turn the rim through the kinds, tap the middle to open one; turn it through the kind's objects, each its card; tap the middle to turn a card to where it is now (with a place); tap the kind above to go back. Held until another screen replaces it or its `ttl_s` ends.

No inputs: send `"params": {}`.

A call that works:

```json
{"name":"scene.play","args":{"pkg":"atlas","layout":"atlas","params":{},"ttl_s":60}}
```
