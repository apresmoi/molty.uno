#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# hygiene.sh — cheap checks on what is committed (git ls-files), for a public repo:
#   1. no local paths or credentials in tracked files
#   2. no committed secret files (.env, .dev.vars, keys)
#   3. the licence files every release ships are present
#   4. the product version copies agree (scripts/version.mjs check)
#   5. the agent skill + plugin packaging (scripts/check-skill.mjs)
# Exits non-zero listing every hit. Run from anywhere inside the repo.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
fail=0
bad() { echo "::error::$*"; fail=1; }

# 1. Content. `git grep` searches tracked files only; -I skips binaries.
#    This file is excluded because it has to spell the patterns out.
patterns=(
  '/Users/[A-Za-z0-9._-]+'              # macOS home paths
  '/home/[a-z][a-z0-9_-]*/'             # Linux home paths
  'C:\\Users\\'                         # Windows home paths
  '-----BEGIN ([A-Z]+ )?PRIVATE KEY-----'
  'AKIA[0-9A-Z]{16}'                    # AWS access key id
  'gh[pousr]_[A-Za-z0-9]{36}'           # GitHub tokens
  'github_pat_[A-Za-z0-9_]{40,}'
  'sk-[A-Za-z0-9_-]{32,}'               # OpenAI-style secret keys
  'xox[baprs]-[A-Za-z0-9-]{10,}'        # Slack tokens
  'AIza[0-9A-Za-z_-]{35}'               # Google API keys
)
for p in "${patterns[@]}"; do
  if hits=$(git grep -nIE -e "$p" -- . ':!scripts/hygiene.sh' ':!**/pnpm-lock.yaml' ':!**/go.sum'); then
    while IFS= read -r h; do bad "forbidden pattern /$p/: $h"; done <<< "$hits"
  fi
done

# 2. Files that must never be committed.
if files=$(git ls-files | grep -E '(^|/)(\.env(\..*)?|\.dev\.vars|id_rsa|id_ed25519|.*\.pem|.*\.p12|.*\.key|credentials\.json)$' | grep -vE '\.env\.example$'); then
  while IFS= read -r f; do bad "secret-looking file is tracked: $f"; done <<< "$files"
fi

# 3. Licence files (the release bundles them with every firmware binary).
for f in LICENSE THIRD_PARTY_NOTICES.md; do
  [[ -s "$f" ]] || bad "$f is missing or empty"
  git ls-files --error-unmatch "$f" >/dev/null 2>&1 || bad "$f is not tracked"
done

# 4. Version copies.
node scripts/version.mjs check || fail=1

# 5. The agent skill (skills/molty/) and its plugin manifests.
node scripts/check-skill.mjs || fail=1

if [[ $fail -ne 0 ]]; then echo "hygiene: FAILED"; exit 1; fi
echo "hygiene: ok"
