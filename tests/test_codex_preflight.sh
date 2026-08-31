#!/usr/bin/env bash
set -euo pipefail

source_repo="$(git rev-parse --show-toplevel)"
test_root="$(mktemp -d)"
trap 'rm -rf "$test_root"' EXIT

new_repo() {
  local name="$1"
  local repo="$test_root/$name"
  git clone -q --no-hardlinks "$source_repo" "$repo"
  printf '%s\n' "$repo"
}

preflight() {
  local repo="$1"
  shift
  (cd "$repo" && ATPD_CODEX_REPORT_DIR="$test_root/reports" \
    "$source_repo/scripts/codex-preflight.sh" "$@" >/dev/null)
}

expect_fail() {
  if preflight "$@"; then
    printf 'expected preflight failure: %s\n' "$*" >&2
    exit 1
  fi
}

write_state() {
  local repo="$1"
  local status="$2"
  local last_commit="$3"
  printf '%s\n' \
    '# ATPD Codex automatic refactor state' \
    'current_step=11' \
    'last_completed_step=10' \
    "last_commit=$last_commit" \
    "status=$status" \
    'blocked_reason=' > "$repo/.rework-state"
}

# A clean ready checkpoint remains the normal between-step path.
clean_repo="$(new_repo clean)"
printf 'runtime checkpoint\n' > "$clean_repo/reports/runtime-checkpoint.md"
preflight "$clean_repo"

# Ready checkpoints do not permit source changes.
ready_dirty_repo="$(new_repo ready-dirty)"
printf '\n' >> "$ready_dirty_repo/src/main.c"
expect_fail "$ready_dirty_repo"

# In-progress checkpoints require the explicit resume mode.
in_progress_repo="$(new_repo in-progress)"
write_state "$in_progress_repo" in_progress 114c7c5
expect_fail "$in_progress_repo"

# An explicitly marked current-step resume accepts an in-scope source change.
resume_repo="$(new_repo resume)"
write_state "$resume_repo" in_progress 114c7c5
printf '\n' >> "$resume_repo/src/main.c"
preflight "$resume_repo" --resume

# Resume rejects a dirty source outside the manifest's auditable scope.
out_of_scope_repo="$(new_repo out-of-scope)"
write_state "$out_of_scope_repo" in_progress 114c7c5
printf '\n' >> "$out_of_scope_repo/src/main.c"
printf '\n' >> "$out_of_scope_repo/src/reactor.c"
expect_fail "$out_of_scope_repo" --resume

# Resume still rejects a bad checkpoint ancestry.
bad_ancestry_repo="$(new_repo bad-ancestry)"
write_state "$bad_ancestry_repo" in_progress 0000000000000000000000000000000000000000
expect_fail "$bad_ancestry_repo" --resume

# A missing current-step manifest remains a hard failure.
missing_manifest_repo="$(new_repo missing-manifest)"
write_state "$missing_manifest_repo" in_progress 114c7c5
mkdir -p "$test_root/empty-manifests"
if (cd "$missing_manifest_repo" && ATPD_CODEX_MANIFEST_DIR="$test_root/empty-manifests" \
    ATPD_CODEX_REPORT_DIR="$test_root/reports" \
    "$source_repo/scripts/codex-preflight.sh" --resume >/dev/null); then
  printf 'expected preflight failure for missing manifest\n' >&2
  exit 1
fi

printf 'codex preflight resume-gate tests passed\n'
