#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# ci-changes.test.sh — tools/scripts/ci-changes.sh maps changed paths to the right CI jobs.
# Each case lists paths and the jobs that must be true (every other job must be false).
# Run by the `changes` job in ci.yml before it trusts the map.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MAP="$HERE/ci-changes.sh"
ALL="console console_e2e boards firmware firmware_test relay cli creatures website python"
fail=0

# expect <name> <expected jobs, space-separated, or ""> <path>...
expect() {
  local name="$1" want="$2"; shift 2
  local got
  if [[ "${1:-}" == "--all" ]]; then got="$("$MAP" --all 2>/dev/null)"
  else got="$(printf '%s\n' "$@" | "$MAP" 2>/dev/null)"; fi
  local j line val should
  for j in $ALL; do
    line="$(grep "^$j=" <<< "$got" || true)"
    val="${line#*=}"
    should=false
    case " $want " in *" $j "*) should=true ;; esac
    if [[ "$val" != "$should" ]]; then
      echo "FAIL $name: $j=$val, want $should"; fail=1
    fi
  done
}

FW="console boards firmware"   # firmware always pulls in the console artifact + the matrix

expect "docs only"         "website"                  docs/HARDWARE.md docs/assets/x.png README.md CHANGELOG.md
expect "agent-link doc"    "website relay"            docs/AGENT-LINK.md
expect "skills only"       "relay"                    skills/molty/SKILL.md skills/molty/references/updates.md
expect "plugin manifests"  ""                         .claude-plugin/plugin.json .codex-plugin/plugin.json
expect "firmware only"     "$FW firmware_test"        device/firmware/main/app_main.c
expect "firmware md"       "$FW firmware_test"        device/firmware/main/README.md
expect "firmware boards"   "$FW firmware_test website" device/firmware/boards/waveshare/esp32-s3-touch-lcd-1.46/board.json
expect "firmware scripts"  "$FW"                      tools/scripts/firmware-boards.py
expect "host tests only"   "firmware_test"            device/firmware/test/host/cute_test.c
expect "console only"      "$FW console_e2e"          device/console/src/main.ts
expect "console catalog"   "$FW console_e2e website"  device/console/src/catalog/catalog.json
expect "creatures js"      "$FW console_e2e creatures website" device/creatures/js/cute/duck.js
expect "creatures .molty"  "$FW console_e2e creatures website firmware_test" device/creatures/molty/duck.molty
expect "relay only"        "relay"                    services/relay/src/worker.ts
expect "cli only"          "cli"                      apps/cli/cmd/molty/main.go
expect "website only"      "website"                  apps/website/src/pages/index.astro
expect "sim / voice"       "python"                   tools/sim/molty_sim/run.py services/molty-voice/x.py
expect "voice only"        "python"                   services/molty-voice/molty_voice/bridge.py
expect "sim only"          "python"                   tools/sim/scenarios/default.yaml
expect "hygiene scripts"   ""                         tools/scripts/hygiene.sh tools/scripts/check-skill.mjs LICENSE
expect "nothing"           ""
expect "workflow change"   "$ALL"                     .github/workflows/ci.yml
expect "other workflow"    "$ALL"                     .github/workflows/release.yml
expect "Makefile"          "$ALL"                     Makefile
expect "VERSION"           "$ALL"                     VERSION
expect "the gate itself"   "$ALL"                     tools/scripts/ci-changes.sh
expect "unknown path"      "$ALL"                     newdir/thing.txt
expect "--all"             "$ALL"                     --all
expect "mixed relay + cli" "relay cli website"        services/relay/README.md apps/cli/go.mod docs/CLI.md

if [[ $fail -ne 0 ]]; then echo "ci-changes tests: FAILED"; exit 1; fi
echo "ci-changes tests: ok"
