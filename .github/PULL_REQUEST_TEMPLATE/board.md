<!-- New board: docs/PORTING.md. Open PRs with this template by adding ?template=board.md
     to the "Open a pull request" URL. Every box needs proof from the real hardware. -->

## Board

- Board: <!-- vendor + model, link to its wiki or datasheet -->
- Folder: `firmware/boards/<vendor>/<model>/`
- Pins from: <!-- the vendor demo / schematic you read (also linked in board.h) -->

## Proof

- [ ] **Photo**: the fluid fills the screen, the right way up
      <!-- drag the image in here -->
- [ ] **Video** (a few seconds): a tap ripples under the finger, tilting moves the fluid downhill
      <!-- drag the video in here; leave out what the board lacks (no touch, no IMU) and say so -->
- [ ] **`/api/info`**, with the `caps` you expect:

```json
<!-- curl http://<device>/api/info -->
```

- [ ] **App size** from `make firmware BOARD=<vendor>/<model>`:

```text
<!-- the "molty.bin binary size ... (NN% free)" line -->
```

- [ ] **Audio** (only if the board has a speaker / mic): `say` and `listen` work through the
      agent link <!-- what you ran, what you heard / what came back -->

## Checklist

- [ ] `board.json` has `"status": "experimental"` and me as `maintainer`
- [ ] optional: `buy` (where to get it) and `photos` (repo-relative, inside the board folder)
- [ ] the two Kconfig lines in `firmware/main/Kconfig.projbuild`
- [ ] `python3 scripts/firmware-boards.py check` passes
- [ ] `make console firmware BOARD=<vendor>/<model>` builds with zero new warnings
- [ ] `make firmware-test` passes
