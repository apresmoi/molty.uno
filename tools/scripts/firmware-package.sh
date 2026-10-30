#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# firmware-package.sh — turn one board's build into its firmware release assets:
#
#   molty-<ver>-<board>-ota.bin      the app image alone: for device/firmware/ota.sh and
#                                    POST /api/ota (which refuses another board's image)
#   molty-<ver>-<board>-factory.bin  bootloader + partition table + otadata + app + SPIFFS
#                                    console, merged by esptool into one image written at
#                                    0x0. It spans 0x0..end of SPIFFS, so it also blanks
#                                    NVS (Wi-Fi, pairing) and the coredump slot: a factory
#                                    reset by design.
#
# <board> is a folder under device/firmware/boards/ or its board.json name; the asset names use
# the name (waveshare-esp32-s3-touch-lcd-1.46). Offsets and flash mode/freq/size come
# from the build's own flash_args, so this never disagrees with what `idf.py flash`
# would write. Needs ESP-IDF's python (esptool) on PATH: run it after
# `. $IDF_PATH/export.sh`.
#
#   tools/scripts/firmware-package.sh <version> <out-dir> <board> [build-dir]
#     build-dir defaults to device/firmware/build/<name> (tools/scripts/firmware-build.sh)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VERSION="${1:?version}"
OUT="${2:?out-dir}"
BOARD="${3:?board (folder under device/firmware/boards/ or its name)}"
NAME="$(python3 "$ROOT/tools/scripts/firmware-boards.py" name "$BOARD")"
BUILD="${4:-$ROOT/device/firmware/build/$NAME}"

mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
for f in flash_args molty.bin bootloader/bootloader.bin partition_table/partition-table.bin ota_data_initial.bin spiffs.bin; do
  [[ -f "$BUILD/$f" ]] || { echo "firmware-package: $BUILD/$f missing (run tools/scripts/firmware-build.sh $BOARD first)" >&2; exit 1; }
done
# the app image must be the one built for this board (its image tag)
python3 "$ROOT/tools/scripts/firmware-boards.py" check-image "$BUILD/molty.bin" "$BOARD"

cp "$BUILD/molty.bin" "$OUT/molty-$VERSION-$NAME-ota.bin"
# flash_args: line 1 = --flash_mode/--flash_freq/--flash_size, then "<offset> <file>" rows.
( cd "$BUILD" && python -m esptool --chip esp32s3 merge_bin -o "$OUT/molty-$VERSION-$NAME-factory.bin" @flash_args )

echo "firmware-package: $(cd "$OUT" && ls -l "molty-$VERSION-$NAME-ota.bin" "molty-$VERSION-$NAME-factory.bin")"
