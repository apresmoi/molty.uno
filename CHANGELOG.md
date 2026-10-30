# Changelog

All notable changes to Molty are recorded here. One version covers the whole product
(firmware, console, relay, CLI); it lives in [`VERSION`](VERSION). How to cut a release:
[docs/RELEASING.md](docs/RELEASING.md).

## Unreleased

First public release.

- **Firmware** (Waveshare ESP32-S3-Touch-LCD-1.46): the fluid of characters, the `.molty`
  creature runtime with 29 cute-kit creatures, speaker + microphone, IMU, touch, USB-C
  networking (NCM) + Wi-Fi with mDNS, rollback-protected OTA, and the agent link
  (pair with an on-device code and a tap). `/api/info` reports the product version as `fw`.
- **Console**: the setup flow (Wi-Fi → Molty → Agent → Tap → Done) and settings pages,
  served by the device from its SPIFFS partition.
- **Relay** (Cloudflare Worker): device WSS, pairing, REST and remote MCP for agents,
  Workers AI TTS/STT. `GET /v1/health` reports the version.
- **CLI**: `molty` device verbs and a stdio MCP server; `molty --version`.
- **Molty talks back** (relay + CLI): `shake`, `tap` and `button` events reach every paired
  agent through a per-agent inbox with a read cursor (storage-backed, 200 events / 24 h,
  30/min). Agents receive them riding along on every REST/MCP response, by pulling
  (`GET /v1/devices/<id>/events`, `molty_events`, long-poll), through an agent-defined signed
  webhook (`PUT /v1/webhook`, `molty_webhook`: templated HTTPS request, HMAC
  `Molty-Signature`, retries, auto-disable) or with `molty events [--exec CMD]`. skill.md
  explains the modes so each agent sets up its own end. `mood` takes an optional `hold_s`.
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
- **molty.uno/build**: will my board work (from docs/PORTING.md), the reference build's
  parts (board, cable, optional battery and an untested vibration add-on with its wiring),
  community builds from every `board.json` (new optional `buy` and `photos`), and a
  "want one built?" waitlist. The relay stores it: `POST /waitlist` into D1 (address,
  time, form only), per-IP rate limit, honeypot, optional Turnstile. New board PRs use
  `.github/PULL_REQUEST_TEMPLATE/board.md`.
- **Release assets**: per board, OTA image and full factory image
  (`molty-X.Y.Z-<board>-{ota,factory}.bin`); console page, CLI binaries for
  linux/darwin/windows × amd64/arm64, `SHA256SUMS`.
