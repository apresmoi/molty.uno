#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# ci-changes.sh — which CI jobs (.github/workflows/ci.yml) a set of changed paths needs.
#
#   git diff --name-only --no-renames A...B | tools/scripts/ci-changes.sh
#   tools/scripts/ci-changes.sh --all            every job (unknown range, manual run)
#
# Reads repo-relative paths on stdin, prints one `<job>=true|false` line per job
# (GITHUB_OUTPUT format). hygiene is not listed: it always runs. The map below is
# the dependency map documented at the top of ci.yml; keep the two in step.
# A path no rule knows runs EVERYTHING, so a new top-level folder is never silently
# untested; add it to a rule (or to the no-job list) instead.
# Tests: tools/scripts/ci-changes.test.sh.
set -euo pipefail

JOBS="console console_e2e boards firmware firmware_test relay cli creatures website python"
# one variable per job (on_<job>); bash 3.2 (macOS) has no associative arrays
for j in $JOBS; do eval "on_$j=false"; done
all=false
known=false
hit() { for j in "$@"; do eval "on_$j=true"; done; known=true; }

if [[ "${1:-}" == "--all" ]]; then
  all=true
elif [[ $# -gt 0 ]]; then
  sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2
else
  while IFS= read -r p || [[ -n "$p" ]]; do
    [[ -z "$p" ]] && continue
    known=false

    case "$p" in
      # Shared by every job: the workflows themselves, this gate, the Makefile (every
      # job's entry point), VERSION (baked into console, firmware, relay, cli) and
      # .gitignore (what counts as untracked/committed, e.g. the creatures check).
      .github/*|Makefile|VERSION|.gitignore|.gitattributes|tools/scripts/ci-changes.*)
        all=true; known=true ;;
    esac

    case "$p" in
      # the console: its own sources + the creature engine and .molty files it imports
      # (vite aliases @viewer/@molty) and its e2e loads; firmware bakes the built page
      # (the console's colours and type come from the website's tokens)
      device/console/*|device/creatures/*|apps/website/src/styles/tokens.css) hit console console_e2e firmware ;;
    esac
    case "$p" in
      # the website renders the console's Molty catalog (@molty-catalog)
      device/console/src/catalog/*) hit website ;;
    esac

    case "$p" in
      # the creature exporter + its committed .molty output; the website renders the
      # creatures (@creatures, @molty-files) and syncs their READMEs as docs
      device/creatures/*) hit creatures website ;;
    esac
    case "$p" in
      # cute_build.sh tests the exported .molty files
      device/creatures/molty/*) hit firmware_test ;;
    esac

    case "$p" in
      device/firmware/test/*) hit firmware_test ;;
      device/firmware/*) hit firmware firmware_test ;;
      tools/scripts/firmware-*) hit firmware ;;
    esac
    case "$p" in
      # /build and /setup read every board.json (repoBoards.ts, setup-match.test.ts)
      device/firmware/boards/*) hit website ;;
    esac

    case "$p" in
      services/relay/*) hit relay ;;
      # relay tests read skills/molty/SKILL.md and docs/AGENT-LINK.md
      skills/molty/*|docs/AGENT-LINK.md) hit relay ;;
    esac

    case "$p" in
      # tools/sim installs molty-voice (editable): a voice change can break the sim
      services/molty-voice/*|tools/sim/*) hit python ;;
    esac

    case "$p" in
      apps/cli/*) hit cli ;;
      apps/website/*) hit website ;;
    esac

    case "$p" in
      # docs feed the website (sync-docs.mjs; build.test.ts reads docs/PORTING.md), as do
      # root markdown files (CONTRIBUTING.md is synced)
      docs/*) hit website ;;
      */*) ;;
      *.md) hit website ;;
    esac

    case "$p" in
      # Covered by hygiene alone (no build job): the skill's plugin packaging, the
      # hygiene/version/skill checks, the release-only packager and licences.
      .claude-plugin/*|.codex-plugin/*|.agents/*|LICENSE \
      |tools/scripts/hygiene.sh|tools/scripts/check-skill.mjs|tools/scripts/version.mjs)
        known=true ;;
    esac

    $known || { echo "ci-changes: no rule for $p: running everything" >&2; all=true; }
  done
fi

if $all; then
  for j in $JOBS; do eval "on_$j=true"; done
fi
# firmware bakes the console page the console job builds (console-dist artifact), and
# boards supplies firmware's matrix: both run whenever firmware does.
if $on_firmware; then on_console=true; on_boards=true; fi

for j in $JOBS; do eval "echo \"$j=\$on_$j\""; done
