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
- **Release assets**: OTA image, full factory image, console page, CLI binaries for
  linux/darwin/windows × amd64/arm64, `SHA256SUMS`.
