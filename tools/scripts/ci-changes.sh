#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# ci-changes.sh — which CI jobs (.github/workflows/ci.yml) a set of changed paths needs.
#
#   git diff --name-only --no-renames A...B | tools/scripts/ci-changes.sh
#   tools/scripts/ci-changes.sh --all            every job (unknown range, manual run)
#
# Reads repo-relative paths on stdin, prints one `<job>=true|false` line per job
# (GITHUB_OUTPUT format). hygiene is not listed: it always runs (it scans every
# tracked file, so a path that runs no job below is still checked).
# The rule: a job runs only if its result can depend on the changed file. Each line
# of the table in `jobs_for` is a path pattern and the jobs whose builds, tests or
# checks read it; the first pattern that matches wins, so specific lines come first.
# A path no line knows runs EVERYTHING, so a new folder is never silently untested.
# To add a path: put a line above the folder's catch-all naming exactly the jobs
# that read it (`none` when only hygiene does), and a case in ci-changes.test.sh.
# Tests: tools/scripts/ci-changes.test.sh (cases, plus the reads each job's code
# makes, found by tools/scripts/ci-changes.reads.mjs).
set -euo pipefail

JOBS="console console_e2e boards firmware firmware_test relay cli creatures website python"

# The console page is baked into the firmware (ci.yml: firmware needs console), so
# anything the console reads is also a firmware input: CONSOLE names all three.
CONSOLE="console console_e2e firmware"
# shellcheck disable=SC2086
jobs_for() {
  case "$1" in
    # ── the gate and what every job runs through ──────────────────────────────────
    .github/workflows/ci.yml|.github/actions/*|tools/scripts/ci-changes.*) echo all ;;
    Makefile) echo all ;;                       # every job's entry point
    .gitattributes|*/.gitattributes) echo all ;; # eol/filter rewrite the bytes every job checks out
    # other workflows run on their own triggers; no ci.yml job reads them
    .github/workflows/deploy-website.yml|.github/workflows/deploy-relay.yml|.github/workflows/release.yml) echo none ;;
    .github/PULL_REQUEST_TEMPLATE/*|.github/ISSUE_TEMPLATE/*) echo none ;;
    .github/*) echo all ;;
    # baked into the console (vite define), its mock device, the firmware (CMake PROJECT_VER),
    # the relay (vitest define, deploy bundle), the cli (-ldflags) and the sim (run.py)
    VERSION) echo $CONSOLE relay cli python ;;
    # the creatures check lists untracked .molty files with --exclude-standard
    .gitignore|device/.gitignore|device/creatures/.gitignore|device/creatures/molty/.gitignore) echo creatures ;;
    */.gitignore) echo none ;;

    # ── markdown a job reads ──────────────────────────────────────────────────────
    # the website syncs some docs (sync-docs.mjs) and its diagrams test reads every .md under
    # docs/ (but the docs update report) and skills/, README, CONTRIBUTING and MOLTY-FORMAT
    README.md|CONTRIBUTING.md|device/creatures/MOLTY-FORMAT.md) echo website ;;
    docs/AGENT-LINK.md) echo website relay ;;   # relay events.test.ts
    docs/DOCS-UPDATE-REPORT.md) echo none ;;
    docs/*.md) echo website ;;
    skills/molty/SKILL.md) echo website relay ;; # relay skill.test.ts
    skills/*.md) echo website ;;
    # proposals, README pictures, examples: hygiene's package tests read docs/examples
    docs/*) echo none ;;

    # ── pictures a build bakes ────────────────────────────────────────────────────
    device/console/*.md) echo none ;;
    device/console/*) echo $CONSOLE ;;          # vite inlines the page's assets
    apps/website/README.md|apps/website/AGENTS.md) echo none ;;
    apps/website/*) echo website ;;
    # board photos: the website's /build page (repoBoards.ts glob), checked by firmware-boards.py
    device/firmware/boards/*.jpg|device/firmware/boards/*.jpeg|device/firmware/boards/*.png|device/firmware/boards/*.webp) echo boards website ;;

    # ── documentation in code folders: no build, test or check reads these ──────────
    *.md|*.png|*.jpg|*.jpeg|*.gif|*.webp|*.svg|*.pdf) echo none ;;
    LICENSE|.claude-plugin/*|.codex-plugin/*|.agents/*) echo none ;;   # hygiene (check-skill.mjs)

    # ── device/creatures ──────────────────────────────────────────────────────────
    # the console's setup stage imports the fluid engine (@viewer); creature_build.sh and
    # cute_build.sh replay the JS players and the mood engine against the C ones
    device/creatures/js/fluid/*) echo $CONSOLE ;;
    device/creatures/js/actions.js|device/creatures/js/mood.js) echo $CONSOLE firmware_test ;;
    device/creatures/js/cute/kit.js) echo $CONSOLE creatures ;;
    device/creatures/js/cute/*) echo creatures ;;   # the exporter imports the kit specs
    device/creatures/js/creature-player.js|device/creatures/js/clip-*.js) echo firmware_test ;;
    device/creatures/molty/*) echo creatures firmware_test ;;
    device/creatures/tools/act-golden.mjs) echo firmware_test ;;
    device/creatures/tools/*) echo creatures ;;
    # the browser viewer (its pages and its own modules): no CI job builds or tests it
    device/creatures/*.html|device/creatures/js/*) echo none ;;

    # ── device/firmware ───────────────────────────────────────────────────────────
    # main/: the image; the host tests; the website's Molty hashes these folders' sources
    # (device/firmware/web/built/molty.json) and fails when they move on without it
    device/firmware/main/audio/*|device/firmware/main/net/*|device/firmware/main/power/*|device/firmware/main/update/*) echo firmware firmware_test ;;
    device/firmware/main/*/*) echo firmware firmware_test website ;;
    device/firmware/main/*) echo firmware firmware_test ;;
    device/firmware/boards/*/board.json) echo firmware firmware_test website ;;
    device/firmware/boards/*) echo firmware firmware_test ;;   # link_build.sh, lcd_build.sh
    device/firmware/web/*) echo firmware_test website ;;     # the image never reads web/
    device/firmware/test/host/fixtures/*) echo firmware_test website ;;
    device/firmware/test/*) echo firmware_test ;;
    device/firmware/flash_auto.sh|device/firmware/ota.sh) echo none ;;   # flashing helpers
    device/firmware/*) echo firmware ;;   # CMakeLists.txt, sdkconfig.defaults*, partitions.csv

    # ── packages, schema, Moltys ──────────────────────────────────────────────────
    # the image bakes the creature packages in packages/index.json; every package plays in the
    # host tests; the website imports some
    packages/cat/*.json|packages/duck/*.json|packages/octopus/*.json|packages/index.json) echo firmware firmware_test website ;;
    packages/*/*.json) echo firmware_test website ;;
    packages/tools/*) echo firmware_test ;;   # molty-creature build in creature_build.sh
    packages/*/build.mjs|packages/*/description.txt) echo none ;;   # hygiene: check-packages.mjs
    schema/*) echo firmware_test ;;           # packages/tools reads them
    moltys/*) echo none ;;                    # hygiene: the Molty format's tests build them

    # ── skills ────────────────────────────────────────────────────────────────────
    skills/molty-rhythm/scripts/*|skills/molty-rhythm/rhythms/*) echo firmware_test website ;;
    skills/molty-waves/scripts/*|skills/molty-waves/data/*) echo firmware_test website ;;   # waves_build.sh sends its chords
    skills/*/scripts/*|skills/molty-astro/display.yaml) echo website ;;   # skills.test.ts, sky.mjs
    skills/*) echo none ;;                    # hygiene: check-skill.mjs, display tests

    # ── services, apps, tools ─────────────────────────────────────────────────────
    # the sim's reflex-turns test runs the relay's ReflexSession (tools/reflex-turns.ts)
    services/relay/src/core/reflex.ts|services/relay/src/core/words.ts) echo relay python ;;
    services/relay/tools/reflex-turns.ts|services/relay/tools/ts-resolve.mjs) echo relay python ;;
    services/relay/*) echo relay ;;
    services/molty-voice/*|tools/sim/*) echo python ;;   # the sim installs molty-voice editable
    apps/cli/*) echo cli ;;
    tools/scripts/firmware-boards.py) echo firmware website ;;   # firmware-build.sh; website build.test.ts
    tools/scripts/firmware-build.sh|tools/scripts/firmware-size.sh) echo firmware ;;
    tools/scripts/firmware-package.sh) echo none ;;   # release.yml only
    tools/scripts/hygiene.sh|tools/scripts/check-skill.mjs|tools/scripts/check-packages.mjs|tools/scripts/version.mjs) echo none ;;
    tools/scripts/sky-moon-map.mjs) echo none ;;   # writes main/creature/sky_moon_map.h, committed
    tools/creatures/*.test.mjs) echo none ;;       # make creatures-test: no CI job runs it
    tools/creatures/*) echo firmware_test ;;       # golden.mjs, clip-golden.mjs
    tools/web-molty/*) echo website ;;             # molty.test.ts plays fidelity.mjs
    tools/alive-rig/*|tools/blender/*) echo none ;;   # not run in CI (blender: hygiene's tests)

    *) echo unknown ;;
  esac
}

# one variable per job (on_<job>); bash 3.2 (macOS) has no associative arrays
reset() { for j in $JOBS; do eval "on_$j=false"; done; all=false; }
add() {
  for j in $(jobs_for "$1"); do
    case "$j" in
      all) all=true ;;
      none) ;;
      unknown) echo "ci-changes: no rule for $1: running everything" >&2; all=true ;;
      *) eval "on_$j=true" ;;
    esac
  done
}
finish() {
  if $all; then for j in $JOBS; do eval "on_$j=true"; done; fi
  # firmware bakes the console page the console job builds (console-dist artifact), and
  # boards supplies firmware's matrix: both run whenever firmware does.
  if $on_firmware; then on_console=true; on_boards=true; fi
}

reset
case "${1:-}" in
  --all) all=true ;;
  # for the tests: one line per path, `<path> <jobs it runs on its own>`
  --each)
    while IFS= read -r p || [[ -n "$p" ]]; do
      [[ -z "$p" ]] && continue
      reset; add "$p"; finish
      line="$p"; for j in $JOBS; do eval "\$on_$j" && line="$line $j"; done
      echo "$line"
    done
    exit 0 ;;
  "")
    while IFS= read -r p || [[ -n "$p" ]]; do
      [[ -z "$p" ]] && continue
      add "$p"
    done ;;
  *) sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2 ;;
esac
finish
for j in $JOBS; do eval "echo \"$j=\$on_$j\""; done
