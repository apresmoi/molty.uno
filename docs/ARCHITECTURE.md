# Architecture

Molty is a **surface, not an agent**. The device is a thin client: a round screen, a
mic, a speaker, an IMU and touch. The brain is whatever agent you connect (a local
CLI agent, a cloud bot, a chat app). Agents drive Molty through one small command
set — `mood`, `show`, `say`, `listen`, `state`, `volume`, `imu`, `status`, `diag`,
`log` — over either path below.

```
                        ┌──────────────── home network ────────────────┐
 agent on a laptop ──── │ molty CLI / MCP ──HTTP──▶ Molty (ESP32-S3)   │
 (same LAN or USB-C)    │                 10.13.37.1 · molty.local     │
                        └──────────────────────────────┬───────────────┘
                                                       │ outbound WSS
                                                       ▼
 agent anywhere ──MCP / REST──▶ relay (Cloudflare Worker + one Durable Object per device)
                                 ├─ pairing (link code + tap on the device), agent tokens
                                 ├─ command routing, async jobs (say / listen)
                                 └─ TTS / STT provider (Workers AI | OpenAI | none)

 console (one HTML file served by the device) ── setup: Wi-Fi → pick a Molty → connect an agent
```

## Components

| Dir | What | Talks to |
|---|---|---|
| `firmware/` | ESP-IDF app: fluid renderer, `.molty` creature runtime, HTTP API, agent-link client, USB-C NCM + Wi-Fi; one board folder per supported board (display, touch, IMU, audio drivers) | relay (WSS out), console + CLI (HTTP in) |
| `console/` | Setup + settings UI, built to one HTML file and baked into the firmware's SPIFFS image | device HTTP API |
| `relay/` | Cloudflare Worker: device sockets, pairing, REST + remote MCP, voice | devices, agents |
| `cli/` | `molty` CLI + stdio MCP server, direct HTTP or via the relay | device or relay |
| `creatures/` | Cute-kit creature specs, the JS fluid engine, viewers, the `.molty` exporter | console (imported at build time) |

## Firmware layout

```
firmware/main/                 the core: no pins, no panel sizes
├── app_main.c      boot order
├── board/          molty_board.h: the one seam to the hardware (+ guarded wrappers)
├── net/            USB-C NCM + Wi-Fi + mDNS (network.c), HTTP API (http.c),
│                   console page + SPIFFS (console_store.c)
├── fluid/          the fluid of characters: sim, targets, draw, speed reader, 5x7 font
├── creature/       creature registry + the .molty cute-kit runtime (cute_spec, cute_rig)
├── link/           agent link: WSS client, identity, pairing, command dispatcher, audio
└── diag/           log ring, frame histograms, /api/diag, /api/log
firmware/boards/               one folder per board, exactly one compiled in
├── waveshare/esp32-s3-touch-lcd-1.46/   SPD2010 QSPI LCD + touch, TCA9554, I2S audio
├── waveshare/esp32-s3-touch-lcd-1.28/   GC9A01 SPI LCD, CST816S touch, battery ADC
├── common/         QMI8658 IMU (bus, address and axis map from the board)
└── _template/      the documented skeleton for a new board
```

A board describes its display (size, round or rectangular) and its optional parts —
touch, IMU, mic, speaker, accept/reject buttons, battery, USB-C network — in a const
`molty_board_t`; `init()` reports what came up and the core degrades around what is
missing (voice and IMU endpoints answer `unsupported`, pairing falls back to a button,
the agent link drops `say`/`listen` from its caps). The Kconfig `MOLTY_BOARD` choice
picks the folder; its `sdkconfig.board` sets flash, PSRAM and console. Adding a board:
[PORTING.md](PORTING.md).

The device renders one creature, **fluid**: a per-cell fluid drawn as characters on the
round LCD, sized at runtime from the board's display (412×412 on the 1.46: 52×32 cells;
240×240 on the 1.28: 30×19 at the same cell size). Everything Molty shows is a *target* the fluid flows toward: a blob,
"thinking", speed-read text, effects, or a character creature. A character creature is a
`.molty` file (≈0.4–1.3 KB, see `creatures/MOLTY-FORMAT.md`): the firmware holds the
generic runtime and generates the meshes; creatures are data, swapped at runtime with
`POST /api/creature/file`, no reflash.

## Design notes

- **Device dials out.** The device's local HTTP API has no auth, so it is never exposed
  to the internet. Remote agents reach the device only through the relay, over a socket
  the device opens.
- **Trust on first use + on-device approval.** The device generates a random ID and key
  on first boot; the relay pins the key hash on first connect. An agent pairs with the
  device's rotating link code, and a human taps the device to accept. Agents get a
  scoped, revocable token.
- **Async audio.** Agent runtimes often cap a tool call at ~10 s / 64 KiB, so `say` and
  `listen` return a job ID; agents poll with `molty_wait` / `GET /v1/jobs/<job>`.
- **Swappable seams.** The relay core is plain TypeScript (no Cloudflare imports, enforced
  by a test), so another host can wrap it. Voice providers sit behind one interface.
  The device has one command dispatcher (`link/link_cmd.c`) meant to serve both the relay link and, later, local HTTP.
- **One creature engine, two implementations.** `creatures/js` (three.js, browser) is the
  reference; `firmware/main/creature` reproduces it in C from the exported `.molty` data.
  The console imports the JS engine directly for its animated preview.
- **Future direction.** An agent-runtime "node" adapter (the device as a peripheral that
  exposes `say/listen/show/mood` commands to a gateway) can sit beside the relay's MCP and
  REST surfaces without changing the device protocol.

Protocol details: [AGENT-LINK.md](AGENT-LINK.md). CLI: [CLI.md](CLI.md). Hardware, build
and flashing: [HARDWARE.md](HARDWARE.md).
