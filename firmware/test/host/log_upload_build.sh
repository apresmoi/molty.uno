#!/usr/bin/env bash
# log_upload_build.sh — host build + run of the activity log's device rules
# (main/diag/log_upload.c: job tags, upload filter + frames, persisted copy + write policy)
# + cJSON from ESP-IDF: set IDF_PATH, default ~/esp/esp-idf.
# LOG_UPLOAD_SRC overrides the source (mutation checks).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIAG="$(cd "$HERE/../.." && pwd)/main/diag"
IDF_JSON="${IDF_PATH:-$HOME/esp/esp-idf}/components/json/cJSON"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/molty-log-upload.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT
cc -std=c11 -Wno-deprecated-declarations ${LINK_TEST_CFLAGS:-} -I"$IDF_JSON" -c "$IDF_JSON/cJSON.c" -o "$OUT/cJSON.o"
cc -std=c11 -Wall -Wextra -Werror -O2 ${LINK_TEST_CFLAGS:-} \
  -I"$DIAG" -I"$IDF_JSON" "$HERE/log_upload_test.c" \
  "${LOG_UPLOAD_SRC:-$DIAG/log_upload.c}" "$OUT/cJSON.o" -lm -o "$OUT/log_upload_test"
"$OUT/log_upload_test"
