# Notes for coding agents

Read `README.md` (layout, build/test table) and `docs/ARCHITECTURE.md` first.

## Commands

```sh
make test              # firmware host tests + relay tests/typecheck + cli tests + console tests
make console           # console/dist/index.html
make firmware          # idf.py build (needs ESP-IDF v5.5 at $IDF_PATH and `make console` first)
make console-e2e       # headless console walk-through against console/tools/mock-device.mjs
make creatures         # re-export creatures/molty/*.molty (output must be committed)
```

## Rules

- Never flash a device, run `ota.sh` / `flash_auto.sh`, or deploy the relay unless the
  user asks. `make firmware` only compiles.
- The console is baked into the firmware image: rebuild `console/` before `firmware/`.
- The `.molty` format spans `creatures/tools/molty-export.mjs` (writer),
  `firmware/main/creature/cute_spec.c` (reader) and `creatures/MOLTY-FORMAT.md` (spec);
  change them together and keep `make creatures` output byte-stable.
- The default relay URL lives in exactly three constants (firmware Kconfig, console,
  CLI); see `docs/AGENT-LINK.md#default-relay`.
- Don't put personal data (device IDs, IPs, account names, local paths) in code, tests
  or docs; use the example values already in the tests.
