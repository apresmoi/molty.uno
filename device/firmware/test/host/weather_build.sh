#!/usr/bin/env bash
# weather_build.sh: host build + run of the weather on the glass (main/creature/weather.c: told, eased, the streaks,
# rings, drops on the glass, the wet floor, the moonlit rule). Plain C, no ESP-IDF. WEATHER_SRC overrides the source.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CREATURE="$(cd "$HERE/../.." && pwd)/main/creature"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/molty-weather.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT
cc -std=c11 -Wall -Wextra -Werror -O2 -I"$CREATURE" "$HERE/weather_test.c" "${WEATHER_SRC:-$CREATURE/weather.c}" -lm -o "$OUT/weather_test"
"$OUT/weather_test"
