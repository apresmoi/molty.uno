# Notes for coding agents

Read `README.md` (layout, build/test table) and `docs/ARCHITECTURE.md` first.

## Commands

```sh
make test              # firmware host tests + relay tests/typecheck + cli tests + console tests
make console           # device/console/dist/index.html
make firmware          # idf.py build (needs ESP-IDF v5.5 at $IDF_PATH and `make console` first)
make console-e2e       # headless console walk-through against device/console/tools/mock-device.mjs
make creatures         # re-export device/creatures/molty/*.molty (output must be committed)
```

## Rules

- Never flash a device, run `ota.sh` / `flash_auto.sh`, or deploy the relay unless the
  user asks. `make firmware` only compiles.
- The console is baked into the firmware image: rebuild `device/console/` before `device/firmware/`.
- The `.molty` format spans `device/creatures/tools/molty-export.mjs` (writer),
  `device/firmware/main/creature/cute_spec.c` (reader) and `device/creatures/MOLTY-FORMAT.md` (spec);
  change them together and keep `make creatures` output byte-stable.
- Creature actions (`act`, say marks, idle life) span `device/firmware/main/creature/cute_act.h`
  and its JS twin `device/creatures/js/actions.js`; change both, then
  `node device/creatures/tools/act-golden.mjs` (output committed; the cute host test checks it).
  The action names are also `ACTIONS` in `services/relay/src/core/commands.ts` (check-skill
  compares all three).
- The default relay URL lives in exactly three constants (firmware Kconfig, console,
  CLI); see `docs/AGENT-LINK.md#default-relay`.
- Don't put personal data (device IDs, IPs, account names, local paths) in code, tests
  or docs; use the example values already in the tests.
