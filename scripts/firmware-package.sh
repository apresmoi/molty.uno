#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# firmware-package.sh — turn an `idf.py build` into the firmware release assets:
#
#   molty-<ver>-ota.bin      the app image alone: for firmware/ota.sh and POST /api/ota
#   molty-<ver>-factory.bin  bootloader + partition table + otadata + app + SPIFFS console,
#                            merged by esptool into one image written at 0x0. It spans
#                            0x0..end of SPIFFS, so it also blanks NVS (Wi-Fi, pairing)
#                            and the coredump slot: a factory reset by design.
#
# Offsets and flash mode/freq/size come from the build's own flash_args, so this
# never disagrees with what `idf.py flash` would write. Needs ESP-IDF's python
# (esptool) on PATH: run it after `. $IDF_PATH/export.sh`.
#
#   scripts/firmware-package.sh <version> <out-dir> [build-dir]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${1:?version}"
OUT="${2:?out-dir}"
BUILD="${3:-$ROOT/firmware/build}"

mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
for f in flash_args molty.bin bootloader/bootloader.bin partition_table/partition-table.bin ota_data_initial.bin spiffs.bin; do
  [[ -f "$BUILD/$f" ]] || { echo "firmware-package: $BUILD/$f missing (run idf.py build first)" >&2; exit 1; }
done

cp "$BUILD/molty.bin" "$OUT/molty-$VERSION-ota.bin"
# flash_args: line 1 = --flash_mode/--flash_freq/--flash_size, then "<offset> <file>" rows.
( cd "$BUILD" && python -m esptool --chip esp32s3 merge_bin -o "$OUT/molty-$VERSION-factory.bin" @flash_args )

echo "firmware-package: $(cd "$OUT" && ls -l "molty-$VERSION-ota.bin" "molty-$VERSION-factory.bin")"
