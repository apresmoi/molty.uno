---
title: Flash a release
description: Put prebuilt Molty firmware on the board without installing ESP-IDF.
---

:::caution[Releases are not published yet]
Prebuilt firmware images will be attached to GitHub releases. Until the first one is
out, [build from source](/getting-started/hardware/). The steps below describe the
release path and the board quirks that apply to both.
:::

## 1. Get esptool

```sh
python3 -m pip install esptool
```

## 2. Download the image

Download the firmware image from the latest release on GitHub. It contains the
bootloader, partition table, firmware and the setup console in one file, flashed at
offset `0x0`.

## 3. Put the board in download mode

Native USB on this board has no reliable auto-reset, so the first flash is manual:

1. **Hold BOOT, plug the USB-C cable in while holding, then release.**
   Holding BOOT and tapping RESET is not reliable on this board.
2. Find the port. On macOS it is a `/dev/cu.usbmodem*` that appears on replug; on Linux
   usually `/dev/ttyACM0`.

## 4. Flash

```sh
python3 -m esptool --chip esp32s3 -p PORT -b 460800 \
  --before default_reset --after hard_reset write_flash 0x0 molty-firmware.bin
```

## 5. Unplug and replug

Unplug and replug the cable, without pressing any buttons. The post-flash reset does
not cleanly leave download mode on this board.

The fluid appears on the screen. Continue with [Set up your Molty](/getting-started/setup/).

## Later updates

Once Molty runs, updates go over HTTP with rollback protection; you never need the BOOT
button again unless the partition table changes. See
[Updates after the first flash](/getting-started/hardware/#updates-after-the-first-flash).
