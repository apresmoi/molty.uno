#!/usr/bin/env bash
# vad_build.sh — host build + run of the listen end-of-speech detector test
# (main/audio/vad.c — no ESP-IDF deps). VAD_SRC overrides the source (mutation checks).
# Usage: test/host/vad_build.sh   (run from anywhere)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AUDIO="$(cd "$HERE/../.." && pwd)/main/audio"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/molty-vad.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT

cc -std=c11 -Wall -Wextra -Werror -O2 \
  -I"$AUDIO" \
  "$HERE/vad_test.c" \
  "${VAD_SRC:-$AUDIO/vad.c}" \
  -lm \
  -o "$OUT/vad_test"
"$OUT/vad_test"
