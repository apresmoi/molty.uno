#!/usr/bin/env bash
# run_all.sh — every firmware host test (plain C, no device, no ESP-IDF build).
# link_build.sh compiles cJSON from ESP-IDF: set IDF_PATH (default ~/esp/esp-idf).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
for t in fluid_build.sh fluid_golden_build.sh director_build.sh ask_build.sh gesture_build.sh vad_build.sh cute_build.sh improv_build.sh link_build.sh trace_build.sh update_build.sh log_upload_build.sh guard_build.sh; do
  echo "== $t"
  "$HERE/$t"
done
echo "== all firmware host tests passed"
