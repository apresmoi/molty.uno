# Changelog

All notable changes to Molty are recorded here. One version covers the whole product
(firmware, console, relay, CLI); it lives in [`VERSION`](VERSION). How to cut a release:
[docs/RELEASING.md](docs/RELEASING.md).

## Unreleased

First public release.

- **Firmware** (Waveshare ESP32-S3-Touch-LCD-1.46): the fluid of characters, the `.molty`
  creature runtime with 26 cute-kit creatures, speaker + microphone, IMU, touch, USB-C
  networking (NCM) + Wi-Fi with mDNS, rollback-protected OTA, and the agent link
  (pair with an on-device code and a tap). `/api/info` reports the product version as `fw`.
- **Console**: the setup flow (Wi-Fi → Molty → Agent → Tap → Done) and settings pages,
  served by the device from its SPIFFS partition.
- **Relay** (Cloudflare Worker): device WSS, pairing, REST and remote MCP for agents,
  Workers AI TTS/STT. `GET /v1/health` reports the version.
- **CLI**: `molty` device verbs and a stdio MCP server; `molty --version`.
- **Boards**: a board layer (`firmware/main/board/molty_board.h`, one folder per board
  under `firmware/boards/`, [docs/PORTING.md](docs/PORTING.md)). The fluid is sized at
  runtime from the board's display; absent hardware degrades (`unsupported` voice/IMU
  endpoints, button-only pairing). Second board: Waveshare ESP32-S3-Touch-LCD-1.28
  (240×240, no audio, Wi-Fi only), experimental. OTA refuses another board's image; the
  agent link reports `board` and only the `caps` the board has.
- **Setup over USB for serial boards**: boards whose USB-C is a USB-UART bridge (the 1.28)
  get Wi-Fi from the browser over the cable (Improv Wi-Fi Serial on the console UART);
  `molty.uno/setup` installs molty.UNO per board and sets Wi-Fi with ESP Web Tools, with
  the images served by the site.
- **Release assets**: per board, OTA image and full factory image
  (`molty-X.Y.Z-<board>-{ota,factory}.bin`); console page, CLI binaries for
  linux/darwin/windows × amd64/arm64, `SHA256SUMS`.
