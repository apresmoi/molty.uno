#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# ci-changes.test.sh — tools/scripts/ci-changes.sh maps changed paths to the right CI jobs.
#   1. one-path cases (the table below) and a few multi-path ones: the listed jobs must be
#      true, every other job false;
#   2. the reads each job's code makes from elsewhere (tools/scripts/ci-changes.reads.mjs):
#      each of those files must run that job.
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
  local j val should
  for j in $ALL; do
    val=false
    case $'\n'"$got"$'\n' in *$'\n'"$j=true"$'\n'*) val=true ;; esac
    should=false
    case " $want " in *" $j "*) should=true ;; esac
    if [[ "$val" != "$should" ]]; then
      echo "FAIL $name: $j=$val, want $should"; fail=1
    fi
  done
}

# firmware always pulls in the console page and the board matrix
FW="console boards firmware"
CON="console console_e2e $FW"

# in JOBS order, without repeats: the jobs of a space-separated list
norm() { local j out=""; for j in $ALL; do case " $1 " in *" $j "*) out="$out $j" ;; esac; done; echo "${out# }"; }

# ── 1. one path, the jobs it must run (and no other). Add a row per rule. ──────────────────
#    path                                                        | jobs ("all" = every job)
check_table() {
  local path want paths=() wants=() got i=0 line
  while IFS='|' read -r path want; do
    path="${path%"${path##*[![:space:]]}"}"
    [[ -z "$path" || "$path" == \#* ]] && continue
    want="${want//\$FW/$FW}"; want="${want//\$CON/$CON}"; [[ "$want" == *all* ]] && want="$ALL"
    paths+=("$path"); wants+=("$(norm "$want")")
  done
  got="$(printf '%s\n' "${paths[@]}" | "$MAP" --each 2>/dev/null)"
  while IFS= read -r line; do
    line="${line#"${paths[$i]}"}"; line="${line# }"
    [[ "$line" == "${wants[$i]}" ]] || { echo "FAIL ${paths[$i]}: runs '${line}', want '${wants[$i]}'"; fail=1; }
    i=$((i + 1))
  done <<< "$got"
  [[ $i -eq ${#paths[@]} ]] || { echo "FAIL table: ${#paths[@]} paths, $i answers"; fail=1; }
}
check_table <<'TABLE'
# documentation in code folders runs no build
device/creatures/README.md                                       |
device/creatures/js/cute/README.md                               |
device/firmware/README.md                                        |
device/firmware/main/fluid/NOTES.md                              |
device/firmware/boards/waveshare/esp32-s3-touch-lcd-1.46/README.md |
device/console/README.md                                         |
services/relay/README.md                                         |
services/molty-voice/README.md                                   |
apps/cli/README.md                                               |
apps/website/README.md                                           |
apps/website/AGENTS.md                                           |
packages/astrology/PORT-SPEC.md                                  |
tools/sim/README.md                                              |
tools/alive-rig/README.md                                        |
moltys/cat/NOTES.md                                              |
CHANGELOG.md                                                     |
AGENTS.md                                                        |
THIRD_PARTY_NOTICES.md                                           |
LICENSE                                                          |
# pictures no build reads
device/firmware/main/fluid/sketch.png                            |
skills/molty-astro/assets/maps/moon.png                          |
skills/molty-astro/assets/signs/leo.svg                          |
docs/assets/molty-hero.webp                                      |
services/relay/diagram.svg                                       |
# documentation a job does read
README.md                                                        | website
CONTRIBUTING.md                                                  | website
device/creatures/MOLTY-FORMAT.md                                 | website
docs/HARDWARE.md                                                 | website
docs/PORTING.md                                                  | website
docs/examples/molty-dial/SKILL.md                                | website
docs/AGENT-LINK.md                                               | website relay
docs/DOCS-UPDATE-REPORT.md                                       |
docs/calls-proposal.html                                         |
docs/examples/molty-pip/molty.json                               |
skills/molty/SKILL.md                                            | website relay
skills/molty/references/updates.md                               | website
skills/molty-astro/references/commands.md                        | website
# pictures and data a build bakes still trigger it
device/console/src/assets/logo.png                               | $CON
device/console/src/assets/icon.svg                               | $CON
apps/website/src/assets/molty/cat.webp                           | website
apps/website/public/og.png                                       | website
device/firmware/boards/waveshare/esp32-s3-touch-lcd-1.46/photo.jpg | boards website
device/firmware/main/net/index.html                              | $FW firmware_test
device/firmware/main/creature/default.molty                      | $FW firmware_test website
packages/cat/cat.json                                            | $FW firmware_test website
packages/index.json                                              | $FW firmware_test website
packages/astrology/astrology.json                                | firmware_test website
packages/rhythm/rhythm.json                                      | firmware_test website
device/firmware/test/host/fluid_golden.txt                       | firmware_test
device/firmware/test/host/fixtures/astrology-commands.jsonl      | firmware_test website
device/firmware/web/built/molty.wasm                             | firmware_test website
device/firmware/web/fidelity.script                              | firmware_test website
# firmware
device/firmware/main/app_main.c                                  | $FW firmware_test
device/firmware/main/fluid/fluid_draw.c                          | $FW firmware_test website
device/firmware/main/net/http.c                                  | $FW firmware_test
device/firmware/main/audio/vad.c                                 | $FW firmware_test
device/firmware/main/CMakeLists.txt                              | $FW firmware_test
device/firmware/CMakeLists.txt                                   | $FW
device/firmware/partitions.csv                                   | $FW
device/firmware/sdkconfig.defaults                               | $FW
device/firmware/ota.sh                                           |
device/firmware/boards/waveshare/esp32-s3-touch-lcd-1.46/board.json | $FW firmware_test website
device/firmware/boards/waveshare/esp32-s3-touch-lcd-1.28/board.c | $FW firmware_test
device/firmware/web/web_molty.c                                  | firmware_test website
device/firmware/test/host/cute_test.c                            | firmware_test
tools/scripts/firmware-boards.py                                 | $FW website
tools/scripts/firmware-build.sh                                  | $FW
tools/scripts/firmware-package.sh                                |
# console and creatures
device/console/src/main.ts                                       | $CON
device/creatures/js/fluid/sim.js                                 | $CON
device/creatures/js/cute/kit.js                                  | $CON creatures
device/creatures/js/cute/duck.js                                 | creatures
device/creatures/js/actions.js                                   | $CON firmware_test
device/creatures/js/creature-player.js                           | firmware_test
device/creatures/js/clip-pack.js                                 | firmware_test
device/creatures/molty/duck.molty                                | creatures firmware_test
device/creatures/tools/molty-export.mjs                          | creatures
device/creatures/tools/act-golden.mjs                            | firmware_test
device/creatures/index.html                                      |
device/creatures/js/main.js                                      |
# packages, schema, skills
packages/tools/format.mjs                                        | firmware_test
packages/cat/build.mjs                                           |
packages/cat/description.txt                                     |
schema/molty-2.json                                              | firmware_test
moltys/cat/molty.json                                            |
skills/molty-rhythm/scripts/library.mjs                          | firmware_test website
skills/molty-rhythm/rhythms/samba.json                           | firmware_test website
skills/molty-astro/scripts/sky.mjs                               | website
skills/molty-astro/scripts/vendor/astronomy.min.js               | website
skills/molty-astro/display.yaml                                  | website
skills/molty-astro/screens/chart.yaml                            |
skills/molty/scripts/talk.sh                                     | website
# services, apps, tools
services/relay/src/worker.ts                                     | relay
services/relay/src/core/reflex.ts                                | relay python
services/relay/tools/reflex-turns.ts                             | relay python
services/relay/src/core/opener.ts                                | relay
apps/cli/cmd/molty/main.go                                       | cli
apps/website/src/pages/index.astro                               | website
apps/website/src/content/docs/getting-started/setup.md           | website
services/molty-voice/molty_voice/bridge.py                       | python
tools/sim/scenarios/default.yaml                                 | python
tools/creatures/golden.mjs                                       | firmware_test
tools/creatures/player.test.mjs                                  |
tools/web-molty/fidelity.mjs                                     | website
tools/alive-rig/alive-rig.mjs                                    |
tools/scripts/hygiene.sh                                         |
tools/scripts/check-skill.mjs                                    |
.claude-plugin/plugin.json                                       |
.codex-plugin/plugin.json                                        |
# shared files
VERSION                                                          | $CON relay cli python
.gitignore                                                       | creatures
tools/sim/.gitignore                                             |
.gitattributes                                                   | all
Makefile                                                         | all
# the gate itself and the workflow it runs in
.github/workflows/ci.yml                                         | all
tools/scripts/ci-changes.sh                                      | all
tools/scripts/ci-changes.test.sh                                 | all
tools/scripts/ci-changes.reads.mjs                               | all
.github/actions/setup/action.yml                                 | all
.github/workflows/new.yml                                        | all
# other workflows run on their own triggers
.github/workflows/release.yml                                    |
.github/workflows/deploy-website.yml                             |
.github/workflows/deploy-relay.yml                               |
.github/PULL_REQUEST_TEMPLATE/board.md                           |
# unknown paths run everything
newdir/thing.txt                                                 | all
device/hardware/board.kicad_pcb                                  | all
device/creatures/crisp-cat-old.json                              | all
TABLE

expect "nothing"           ""
expect "--all"             "$ALL"                     --all
expect "docs cleanup"      "website"                  docs/HARDWARE.md docs/assets/x.png README.md CHANGELOG.md device/creatures/README.md device/firmware/README.md
expect "mixed relay + cli" "relay cli website"        services/relay/README.md apps/cli/go.mod docs/CLI.md services/relay/src/worker.ts
expect "a README and code" "$FW firmware_test"        device/firmware/README.md device/firmware/main/net/http.c

# ── 2. every file a job's code reads from elsewhere runs that job ─────────────────────────
if ! reads="$(node "$HERE/ci-changes.reads.mjs")"; then echo "FAIL ci-changes.reads.mjs"; fail=1; fi
each="$(awk '{print $2}' <<< "$reads" | sort -u | "$MAP" --each 2>/dev/null)"
missing="$(awk 'NR == FNR { for (i = 2; i <= NF; i++) runs[$1 " " $i] = 1; next }
                NF && !runs[$2 " " $1]' <(printf '%s\n' "$each") <(printf '%s\n' "$reads"))"
while read -r job path; do
  [[ -z "$job" ]] && continue
  echo "FAIL $job reads $path, but a change to it does not run $job: add it to tools/scripts/ci-changes.sh"; fail=1
done <<< "$missing"

if [[ $fail -ne 0 ]]; then echo "ci-changes tests: FAILED"; exit 1; fi
echo "ci-changes tests: ok"
