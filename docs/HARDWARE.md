# Hardware, build and flash

## Board

**Waveshare ESP32-S3-Touch-LCD-1.46** (the version without cover glass works too):
ESP32-S3, 16 MB flash, 8 MB PSRAM, 412×412 round LCD (SPD2010, QSPI) with capacitive
touch, QMI8658 IMU, TCA9554 I/O expander, I2S speaker amp and microphone, battery management.
Its native USB is routed to the USB-C port (no USB-UART bridge).

Pin assignments come from Waveshare's demo bundle for this board; see the headers in
`firmware/main/hw/`.

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
make console     # console/dist/index.html (typecheck + unit tests + build)
make firmware    # idf.py build; needs the console built first (it is baked into SPIFFS)
```

`make firmware` never flashes. The console is part of the flash image: the build stages
`console/dist/index.html` into the `spiffs` partition (`firmware/main/stage_console_html.cmake`)
and fails with an instruction if the console is not built yet.

`sdkconfig` and `managed_components/` are generated from the tracked `sdkconfig.defaults`
and `dependencies.lock`. Device options (e.g. the default relay URL) are under
`idf.py menuconfig` → **Molty**.

## First flash (and any partition-table change)

Native USB on this board has no reliable auto-reset, so the first flash is manual:

1. `make console firmware`
2. **Hold BOOT, plug the USB-C cable in while holding, then release.** (Holding BOOT and
   tapping RESET is not reliable on this board.)
3. Flash (replace the port; on macOS it is a `/dev/cu.usbmodem*` that appears on replug):
   ```sh
   cd firmware/build
   python -m esptool --chip esp32s3 -p PORT -b 460800 \
     --before default_reset --after hard_reset write_flash "@flash_args"
   ```
   If the partition table changed, do a full erase first: `idf.py erase-flash flash`.
4. **Unplug and replug** (no buttons) to boot: the post-flash reset does not cleanly
   leave download mode on this board.

## Updates after the first flash

Both scripts need firmware already running on the device.

```sh
cd firmware
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

Console development against a real device: `cd console && pnpm dev`, then open
`http://localhost:8170/?device=http://10.13.37.1`.
