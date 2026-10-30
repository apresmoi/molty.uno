#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# firmware-size.sh — app image size vs its OTA slot, after `idf.py build`.
# The slot size is read from firmware/partitions.csv (ota_0), not hard-coded, so a
# partition change moves the budget with it. Fails above LIMIT_PCT (default 90):
# ESP-IDF itself only fails at 100%, by which point an OTA-able headroom is gone.
# Writes a markdown table to $GITHUB_STEP_SUMMARY when set (CI).
#
#   scripts/firmware-size.sh [build-dir] [label]   (default firmware/build; label = the
#                                                   board name, for the CI summary)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-$ROOT/firmware/build}"
LABEL="${2:-molty.bin}"
LIMIT_PCT="${LIMIT_PCT:-90}"

app="$BUILD/molty.bin"
[[ -f "$app" ]] || { echo "firmware-size: $app not found (run idf.py build first)" >&2; exit 1; }
size=$(wc -c < "$app" | tr -d ' ')
slot_hex=$(awk -F, '$1 ~ /^[ \t]*ota_0[ \t]*$/ { gsub(/[ \t]/, "", $5); print $5 }' "$ROOT/firmware/partitions.csv")
[[ -n "$slot_hex" ]] || { echo "firmware-size: no ota_0 row in firmware/partitions.csv" >&2; exit 1; }
slot=$((slot_hex))
pct=$(awk -v s="$size" -v t="$slot" 'BEGIN { printf "%.1f", 100 * s / t }')
free=$((slot - size))

line="app $LABEL: $size B of $slot B ota slot ($slot_hex) = ${pct}% used, $free B free (limit ${LIMIT_PCT}%)"
echo "$line"
if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
  {
    echo "### Firmware size: $LABEL"
    echo
    echo "| image | bytes | slot | used | free |"
    echo "|---|---:|---:|---:|---:|"
    echo "| molty.bin | $size | $slot ($slot_hex) | ${pct}% | $free |"
  } >> "$GITHUB_STEP_SUMMARY"
fi
if awk -v p="$pct" -v l="$LIMIT_PCT" 'BEGIN { exit !(p > l) }'; then
  echo "::error::firmware app ($LABEL) is ${pct}% of its OTA slot (limit ${LIMIT_PCT}%)" >&2
  exit 1
fi
