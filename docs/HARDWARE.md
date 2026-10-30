# Hardware, build and flash

## Supported boards

| Board (`device/firmware/boards/…`) | Display | Touch | IMU | Mic / speaker | Buttons | USB-C | Flash / PSRAM | Status |
|---|---|---|---|---|---|---|---|---|
| **Waveshare ESP32-S3-Touch-LCD-1.46** (`waveshare/esp32-s3-touch-lcd-1.46`) | 412×412 round, SPD2010 QSPI | SPD2010 | QMI8658 | yes / yes | BOOT accept, PWR reject | native USB: NCM network at 10.13.37.1 | 16 MB / 8 MB octal | reference |
| **Waveshare ESP32-S3-Touch-LCD-1.28** (`waveshare/esp32-s3-touch-lcd-1.28`) | 240×240 round, GC9A01A SPI | CST816S | QMI8658 | — / — | BOOT accept | CH343P USB-UART: serial logs + Wi-Fi setup over Improv, no USB network | 16 MB / 2 MB quad | experimental |

The 1.46 is the V1 board (the version without cover glass works too): it adds a TCA9554
I/O expander, an I2S speaker amp and microphone, and battery management with a
soft-power latch. The 1.28 has no audio, so `say`/`listen` and the audio endpoints
answer `unsupported`; it is reached over Wi-Fi only, set up over the USB cable from
**molty.uno/setup** (Improv Wi-Fi Serial; [PORTING.md](PORTING.md#flash-it)). Each board's pins, with their sources, are in its
`board.h`. Another board: [PORTING.md](PORTING.md).

## Prerequisites

| Tool | Version known to work | Used for |
|---|---|---|
| ESP-IDF | v5.5 | firmware (`idf.py`); expected at `~/esp/esp-idf`, override with `IDF_PATH` |
| Node | 22 | console, relay, creature tools |
| pnpm | 11 | console, relay (`npm` is not supported for the relay) |
| Go | 1.25+ | CLI |
| Python + esptool | any recent | flashing (ships with ESP-IDF) |

## Build

From the repo root (`make help` lists everything):

```sh
make console     # device/console/dist/index.html (typecheck + unit tests + build)
make firmware    # idf.py build: the 1.46 in device/firmware/build; needs the console first (it is baked into SPIFFS)
make firmware BOARD=waveshare/esp32-s3-touch-lcd-1.28   # another board, in device/firmware/build/<vendor>-<model>/
make firmware-all                                       # every board under device/firmware/boards/
```

`make firmware` never flashes. Each board builds with its own sdkconfig
(`tools/scripts/firmware-build.sh`): the board's `sdkconfig.board` (flash, PSRAM, console, the
Kconfig `MOLTY_BOARD` choice) is layered over `sdkconfig.defaults`, and a build whose
sdkconfig belongs to another board stops with an error. The console is part of the flash image: the build stages
`device/console/dist/index.html` into the `spiffs` partition (`device/firmware/main/stage_console_html.cmake`)
and fails with an instruction if the console is not built yet.

`sdkconfig` and `managed_components/` are generated from the tracked `sdkconfig.defaults`
(+ the board's `sdkconfig.board`) and `dependencies.lock`. Device options (e.g. the default
relay URL) are under `idf.py menuconfig` → **Molty**.

## First flash (and any partition-table change)

**Waveshare 1.28** (and any board with a USB-UART bridge): esptool resets it into the
bootloader by itself, and the serial port shows the boot log at 115200.

```sh
make console firmware BOARD=waveshare/esp32-s3-touch-lcd-1.28
cd device/firmware/build/waveshare-esp32-s3-touch-lcd-1.28
python -m esptool --chip esp32s3 -p PORT -b 460800 erase_flash
python -m esptool --chip esp32s3 -p PORT -b 460800 \
  --before default_reset --after hard_reset write_flash "@flash_args"
```

Then give it Wi-Fi from **molty.uno/setup** in Chrome or Edge (Connect → the serial port →
Connect to Wi-Fi), which can also do the install above without ESP-IDF; it identifies the
board itself and offers only that board's firmware. Details and a
local test: [PORTING.md](PORTING.md#flash-it).

**Waveshare 1.46.** Native USB on this board has no reliable auto-reset, so the first
flash is manual:

1. `make console firmware`
2. **Hold BOOT, plug the USB-C cable in while holding, then release.** (Holding BOOT and
   tapping RESET is not reliable on this board.)
3. Flash (replace the port; on macOS it is a `/dev/cu.usbmodem*` that appears on replug):
   ```sh
   cd device/firmware/build
   python -m esptool --chip esp32s3 -p PORT -b 460800 \
     --before default_reset --after hard_reset write_flash "@flash_args"
   ```
   If the partition table changed, do a full erase first: `idf.py erase-flash flash`.
4. **Unplug and replug** (no buttons) to boot: the post-flash reset does not cleanly
   leave download mode on this board.

## Updates after the first flash

Both scripts need firmware already running on the device.

```sh
cd device/firmware
./ota.sh          # over HTTP via POST /api/ota; rollback-protected
PORT=/dev/cu.usbmodemXXXX ./flash_auto.sh   # serial, hands-free via /api/reboot_bootloader
```

Only a partition-table or bootloader change needs the BOOT-button flash.

Gotchas:

- **Don't OTA while a WebSocket session is open** (e.g. a mic or IMU stream): the update
  can race and roll back.
- A freshly OTA'd image is marked valid only once the HTTP server starts; if it crashes
  before that, the next reset rolls back. Re-run `ota.sh`.
- `/api/info`'s `fw_build` comes from `__DATE__`/`__TIME__` and can be stale after an
  incremental build. Check an update landed by probing behaviour, not that field.

## Talking to the device

- `http://10.13.37.1` — over USB-C. The board appears as a USB Ethernet adapter (NCM) and
  hands your computer an address by DHCP; it does not advertise a default route.
- `http://molty.local` — over Wi-Fi via mDNS, once Wi-Fi is set up in the console.
- The scripts take `MOLTY_URL` (device) and `PORT` (serial port).

Console development against a real device: `cd device/console && pnpm dev`, then open
`http://localhost:8170/?device=http://10.13.37.1`.
