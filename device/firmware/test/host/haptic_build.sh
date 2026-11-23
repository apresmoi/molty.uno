#!/usr/bin/env bash
# haptic_build.sh: host build + run of the vibration motor's plan (main/haptic/haptic_plan.c: a buzz's
# segments, the 2 s cap, the duty, the shake gate's quiet window). Plain C, no ESP-IDF.
# HAPTIC_SRC overrides the source (mutation checks).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HAPTIC="$(cd "$HERE/../.." && pwd)/main/haptic"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/molty-haptic.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT
cc -std=c11 -Wall -Wextra -Werror -O2 -I"$HAPTIC" "$HERE/haptic_test.c" "${HAPTIC_SRC:-$HAPTIC/haptic_plan.c}" -o "$OUT/haptic_test"
"$OUT/haptic_test"
