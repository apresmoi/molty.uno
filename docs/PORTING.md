# Porting molty.UNO to your board

molty.UNO runs on any ESP32-S3 board with a screen, 16 MB of flash and PSRAM. Everything
else (touch, IMU, mic, speaker, buttons, battery) is optional: the firmware asks the
board what it has and degrades without it. Supporting a board is one folder.

```
device/firmware/
├── main/                      the core: fluid, creature, link, net, diag — no pins, no panel sizes
│   └── board/molty_board.h    the interface a board implements
└── boards/
    ├── _template/             documented skeleton: copy this
    ├── common/                drivers shared by boards (QMI8658 IMU)
    └── <vendor>/<model>/
        ├── board.json         identity, display, capabilities (CI builds every one)
        ├── board.h            the pin map
        ├── board.c            the molty_board_t descriptor + bring-up
        ├── board.cmake        sources, -O2 sources, include dirs, IDF components
        ├── sdkconfig.board    flash, PSRAM mode, console, the Kconfig board choice
        └── *.c / *.h          its drivers
```

Reference ports: `waveshare/esp32-s3-touch-lcd-1.46` (412×412 QSPI panel, I/O expander,
audio, two buttons, native USB) and `waveshare/esp32-s3-touch-lcd-1.28` (240×240 SPI
panel, no audio, one button, USB-UART bridge).

## Requirements

| | Needed | Why |
|---|---|---|
| Chip | ESP32-S3 | the only target built today (C3, C6 and the classic ESP32: not yet) |
| Flash | 16 MB | the partition table: two 3.75 MB OTA slots + 8.4 MB SPIFFS (console page, creature) |
| PSRAM | 2 MB or more (8 MB recommended), quad or octal | framebuffer, console page (~340 KB), TLS and Wi-Fi buffers |
| Display | any size, round or rectangular, RGB565 | the fluid draws in the largest centred square |

molty.uno/build restates this table as "Will my board work?"
(`apps/website/src/features/build/content.ts`; its test fails when the two disagree).

## Capabilities

`init()` reports what actually came up; a part that fails to start counts as absent.
`GET /api/info` lists the result (`caps`), and the boot log prints it.

| Capability | Without it |
|---|---|
| `display` (required) | the fluid runs but nothing is shown |
| `touch` | no ripples from a finger; pairing is accepted on the accept button |
| `imu` | no tilt; `/api/imu/snapshot`, `/ws/imu` and the link's `imu` answer `unsupported` |
| `speaker` | `/api/audio/{tone,play,play_wav,volume}` and the link's `say`/`volume` answer `unsupported`; the hello frame leaves `say` out of `caps`, so the relay refuses it before spending TTS |
| `mic` | `/api/audio/record`, `/ws/audio/mic` and the link's `listen` answer `unsupported` (hello leaves `listen` out) |
| `btn_accept` | pairing is accepted by touch only (with neither, pairing is refused) |
| `btn_reject` | pairing is rejected only by its 110 s timeout |
| `battery` | no `battery_mv` in `/api/info` |
| `usb_net` | no `http://10.13.37.1` over USB-C: the device is reached over Wi-Fi only (see below) |
| `serial` | no Wi-Fi setup over the cable (Improv) and no serial logs; declare it when USB-C goes through a USB-UART bridge to the console UART |

## Steps

1. **Copy the template** to `device/firmware/boards/<vendor>/<model>/` (lowercase, e.g.
   `lilygo/t-rgb-2.1`). Its board.json `name` is `<vendor>-<model>`.
2. **Pins → `board.h`.** Every pin in one place, with where it came from (the vendor's
   demo or schematic). Read the vendor demo's display init and touch code, not just its
   pin list: reset polarity, backlight polarity, SPI mode and the panel's init sequence
   all matter.
3. **Display.** Write (or reuse) a driver with: a `width×height` RGB565 framebuffer in
   PSRAM, stored byte-swapped (high byte first in memory: the panel's wire order), a full
   flush, and a flush of full-width rows `[y0, y1)`. The 1.28's `lcd_gc9a01.c` is a
   minimal SPI example on IDF's `esp_lcd` panel IO. The fluid only ever writes the
   framebuffer and flushes rows; it is sized at runtime (below).
4. **Optional parts.** Touch returns panel pixels in the framebuffer's orientation.
   The IMU sample is the *device* frame — x right, y up as the screen is read — so map
   your chip's axes (`boards/common/qmi8658.c` takes `axes`, e.g. `{2, 1, 3}`; check by
   tilting: the fluid must run downhill). Speaker and mic are op tables
   (`molty_speaker_ops_t`, `molty_mic_ops_t`; see the 1.46's `mole_audio.c`). Leave an
   op `NULL` and its `MOLTY_CAP_` bit out when the board lacks it.
5. **`board.c`.** Fill `molty_board_desc`: `id` = board.json `name`, the display size and
   shape, the declared caps, `init()` (bring-up in your board's order; log and continue on
   failure, never abort the boot; return what came up) and `early_init()` if something
   must happen in the first milliseconds (the 1.46 latches its battery power there).
6. **`board.cmake`.** List the sources, the per-frame ones for `-O2` (the display flush),
   include dirs and the IDF components your drivers use (`esp_lcd`, `esp_driver_spi`,
   `esp_adc`, ...).
7. **`sdkconfig.board`.** The Kconfig board line, flash size/mode, PSRAM mode
   (`CONFIG_SPIRAM_MODE_OCT` for "R8" modules, `_QUAD` for "R2"), and the console:
   `CONFIG_ESP_CONSOLE_UART_DEFAULT=y` when USB-C goes through a USB-UART bridge (you get
   serial logs), `CONFIG_ESP_CONSOLE_NONE=y` when USB-C is the chip's native USB (TinyUSB
   needs it; declare `MOLTY_CAP_USB_NET`).
8. **Identification.** In `board.json`: `usb`, every USB vendor/product ID the board shows
   up as on a serial port (its USB-UART bridge, or `0x303a:0x1001` for native USB in
   download mode), and `detect`, its download-mode fingerprint: `{"chip": "ESP32-S3",
   "psram": "2MB" | "8MB" | "none", "flash": "16MB"}` (esptool prints both PSRAM, from the
   eFuses, and flash size). In `board.c`: `.psram_mb` / `.flash_mb`, the same numbers;
   the firmware checks them at boot and leaves every pin alone on hardware that differs.
   molty.uno/setup uses these to identify a board and offer only its firmware; the check
   below refuses two boards it could not tell apart.
9. **Optional: where to buy it and photos.** `buy`, a list of `{"label", "url"}` (https;
   Amazon links get the site's Associates tag added on molty.uno, so give a plain search
   or product URL), and `photos`, repo-relative paths to `.jpg`/`.png`/`.webp` files in
   your board folder (e.g. `device/firmware/boards/<vendor>/<model>/photo.jpg`). molty.uno/build
   lists every board under "community builds" from its board.json, with these when present.
10. **Kconfig.** Two lines in `device/firmware/main/Kconfig.projbuild`: a `config
   MOLTY_BOARD_<VENDOR>_<MODEL>` entry in the `MOLTY_BOARD` choice, and a
   `default "<vendor>/<model>" if MOLTY_BOARD_<VENDOR>_<MODEL>` under `MOLTY_BOARD_DIR`.
11. **Check and build.**
   ```sh
   python3 tools/scripts/firmware-boards.py check          # wiring: Kconfig, sdkconfig.board, board.c id
   make console                                      # the console page is baked into the image
   make firmware BOARD=<vendor>/<model>              # -> device/firmware/build/<vendor>-<model>/
   ```
   Zero new warnings, please. `make firmware-test` runs the host tests (the fluid at
   412×412 must stay bit-identical; it is also rendered at 240×240 and 320×240).

## The fluid on your panel

The fluid fills a square *stage*: the whole panel when it is square, the largest centred
square otherwise (a rectangle gets black bars). The default grid keeps the 1.46's cell
size in pixels, so braille dots and glyphs look the same on every panel: 412 px → 52×32
cells, 240 px → 30×19 (clamped to 24×16 .. 64×40; `POST /api/creature/state
{"grid":"WxH"}` overrides it). Speed-reader letters scale with the stage, never below
18 px.

## Flash it

The first flash of a new firmware (and any partition-table change) is a full flash:

```sh
cd device/firmware/build/<vendor>-<model>
python -m esptool --chip esp32s3 -p PORT -b 460800 erase_flash
python -m esptool --chip esp32s3 -p PORT -b 460800 \
  --before default_reset --after hard_reset write_flash "@flash_args"
```

With a USB-UART bridge (CH340/CH343/CP210x) esptool resets into the bootloader by itself;
with native USB, hold BOOT while plugging in (HARDWARE.md). Afterwards
`BOARD=<vendor>/<model> MOLTY_URL=http://<device> device/firmware/ota.sh` updates over HTTP
(it uploads `device/firmware/build/<vendor>-<model>/molty.bin` after checking its board tag); `/api/ota` refuses an image built for another board (each image
carries its board folder).

**Wi-Fi without USB-C networking.** The console's Wi-Fi setup runs over the USB-C
network. A board without `usb_net` but with `serial` (a USB-UART bridge on the console
UART, like the 1.28) is set up over the cable instead: the firmware speaks
[Improv Wi-Fi Serial](https://www.improv-wifi.com/serial/) on the console UART, next to
its log lines (`device/firmware/main/net/improv_serial.c`). Open **molty.uno/setup** in Chrome or
Edge on a desktop, Connect, choose the serial port, then Connect to Wi-Fi. The password
goes from the browser over the cable to the device only. The same page can install
molty.UNO on the board first (ESP Web Tools; images served by the site itself).

There is no board menu: picking the wrong one is how a board gets another board's image.
The page identifies the board instead and offers that board's firmware only:

1. Only ports whose USB ID belongs to a board (`board.json` `usb`) are listed.
2. molty firmware answering Improv names its board (device info hardware field,
   `<board id>/esp32-s3`, ESPHome's form): definitive. Meanwhile the fluid shows "hi", so
   you see which device is on the cable.
3. Otherwise the chip is read in its ROM bootloader (esptool-js): chip, embedded PSRAM size,
   flash size, plus the USB ID, matched against every `detect`. Exactly one board must
   match; none, or more than one, and the page refuses and changes nothing.

A wrong image is still recoverable: the ROM bootloader always accepts a serial flash. At
boot each image compares the chip's PSRAM and flash with its board's and, on a mismatch,
skips the board's init and reports `board_mismatch` in `/api/info` (an image for the other
Waveshare usually stops earlier, in PSRAM bring-up: octal vs quad).
To try it against a local build:

```sh
make console firmware BOARD=<vendor>/<model>
cd apps/website && pnpm install && pnpm stage-firmware --local && pnpm dev   # http://localhost:4330/setup/
```

What the device answers: device info `molty.UNO`, the version, `ESP32-S3`, the board's
name; after a successful join the result URL is `http://<ip>/`, the console over Wi-Fi
(also `http://molty.local`). Your board needs nothing for this beyond declaring
`MOLTY_CAP_SERIAL` and `CONFIG_ESP_CONSOLE_UART_DEFAULT=y` in its `sdkconfig.board`.

Without a browser, the credentials can also be written into NVS from your computer, once,
after the full flash:

```sh
printf 'key,type,encoding,value\nwifi,namespace,,\nssid,data,string,MyNetwork\npass,data,string,my-password\n' > wifi.csv
python "$IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py" generate wifi.csv nvs.bin 0x6000
python -m esptool --chip esp32s3 -p PORT write_flash 0x9000 nvs.bin && rm wifi.csv nvs.bin
```

The device joins on boot and logs `WiFi connected: ip=…` on the serial port; browse
`http://molty.local` or that address. The console can change the network from then on.

## Submit your board

Open a pull request with the board folder and the two Kconfig lines, using the board
template: add `?template=board.md` to the "Open a pull request" URL
([`.github/PULL_REQUEST_TEMPLATE/board.md`](../.github/PULL_REQUEST_TEMPLATE/board.md)).
It asks for proof from the hardware:

- a photo of the fluid filling the screen, the right way up;
- a short video: a tap ripples under the finger, tilting moves the fluid downhill;
- the `/api/info` output, with the `caps` you expect;
- the app size from `make firmware BOARD=...`;
- if the board has audio: `say` and `listen` working through the agent link;
- `board.json` with `"status": "experimental"` and you as `maintainer`, optionally `buy`
  and `photos` (step 9);
- where the pins came from (links in `board.h`).

CI builds every `device/firmware/boards/**/board.json` (folders starting with `_` are not
boards) and releases publish `molty-<version>-<vendor>-<model>-{ota,factory}.bin` for
each.
