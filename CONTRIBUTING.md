# Contributing

Thanks for helping. Small, focused pull requests are easiest to review.

## Before you open a PR

1. Run the tests for what you touched (`make help` lists them; `make test` runs all
   host/unit suites). If you changed the console, also run `make console-e2e`.
2. If you changed firmware, make sure `make console firmware` builds.
3. If you changed a creature spec in `creatures/js/cute/`, re-export with
   `make creatures` and commit the changed `.molty` files.
4. Keep the three default relay URLs in sync if you change one
   (see [docs/AGENT-LINK.md](docs/AGENT-LINK.md#default-relay)).
5. Add a line under `## Unreleased` in [CHANGELOG.md](CHANGELOG.md) for user-visible
   changes. Don't edit `VERSION`; it is bumped when a release is cut
   ([docs/RELEASING.md](docs/RELEASING.md)).

CI ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)) runs the same checks on
every pull request, plus `make hygiene` (no secrets or local paths committed) and a
firmware size gate (`make firmware-size`: the app must stay under 90% of its OTA slot).

## Conventions

- Firmware: C11, ESP-IDF v5.5. Board drivers live in their board's folder under
  `firmware/boards/` and the core reaches hardware only through
  `firmware/main/board/molty_board.h` (new board: `docs/PORTING.md`); anything
  per-frame stays allocation-free. The fluid's 412×412 output is pinned by golden
  hashes (`firmware/test/host/fluid_golden.txt`): never re-capture them to go green. New HTTP routes go in the matching `net/` or
  feature file and are registered from `http_server_start()`.
- The `.molty` format is shared by the JS exporter and the C reader: change
  `creatures/MOLTY-FORMAT.md`, `creatures/tools/molty-export.mjs` and
  `firmware/main/creature/cute_spec.c` together.
- The relay core (`relay/src/core`) must not import Cloudflare APIs (a test enforces it).
- New source files: add `SPDX-License-Identifier: MIT`. Keep third-party notices intact
  and add new dependencies to `THIRD_PARTY_NOTICES.md`.

## New boards

A new board is one folder under `firmware/boards/` ([docs/PORTING.md](docs/PORTING.md)).
Open its pull request with the board template (add `?template=board.md` to the PR URL,
[`.github/PULL_REQUEST_TEMPLATE/board.md`](.github/PULL_REQUEST_TEMPLATE/board.md)): it
asks for a photo, a short video and `/api/info` from the real hardware.

## Hardware

You need the board to test firmware behaviour, but not to build or to run the host
tests. Flashing instructions: [docs/HARDWARE.md](docs/HARDWARE.md).

By contributing you agree that your contributions are licensed under the MIT licence.
