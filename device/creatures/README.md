# Creatures

Molty's creatures and the browser reference for how they move. The firmware runs the
same creatures from exported `.molty` files.

```
device/creatures/
├── js/
│   ├── cute/         cute kit: kit.js (defineCreature) + one file per creature pack — see cute/README.md
│   ├── fluid/        fluid-of-characters engine (sim, targets, renderer, speed reader, sound)
│   ├── setup/        fluid-stage.js — the animated round screen the console embeds
│   ├── mood.js       mood engine (idle, listening, thinking, speaking, happy, sleepy, surprised)
│   ├── actions.js    one-shot actions (nod, wiggle, glances, yawn, …): MoodEngine.act(name);
│   │                 the firmware twin is cute_act.h, pinned by tools/act-golden.mjs
│   ├── proc.js       two procedural creatures (Mochi, Burr)
│   ├── stage.js      3D viewport + 412×412 device preview
│   ├── catalog.js    viewer roster
│   └── main.js       viewer UI
├── molty/            the 29 exported .molty files (bundled into the console)
├── tools/            molty-export.mjs (spec → .molty), molty-behaviours.mjs (hook stand-ins),
│                     act-golden.mjs (actions.js → the firmware's act_golden.txt)
├── index.html        creature viewer: moods, pokes, autopilot, device preview, compare A|B
├── fluid.html        the fluid of characters, as the device draws it
├── setup-mock.html   design mock of the console's setup flow
└── MOLTY-FORMAT.md   the .molty binary format
```

## Open the viewers

```sh
python3 -m http.server -d creatures 8000
# http://localhost:8000          creature viewer
# http://localhost:8000/fluid.html
```

A local server is required (ES modules don't load from `file://`). three.js r170 loads
from cdn.jsdelivr.net, so the first load needs network.

## Export creatures

```sh
node device/creatures/tools/molty-export.mjs all          # -> device/creatures/molty/*.molty
node device/creatures/tools/molty-export.mjs duck --json  # one creature + its resolved spec
```

The output is deterministic; commit the `.molty` files. The firmware's default creature
is `device/firmware/main/creature/default.molty` (the duck).
