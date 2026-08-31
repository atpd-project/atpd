#!/usr/bin/env bash
set -euo pipefail

EXPECTED_BRANCH="${ATPD_CODEX_BRANCH:-ebpf-native-api}"
MASTER_PLAN="${ATPD_CODEX_MASTER_PLAN:-docs/refactor/ATPD_C_REFACTOR_MASTER_EXECUTION_PLAN.md}"
STATE_FILE="${ATPD_CODEX_STATE_FILE:-.rework-state}"
STEP_INDEX="${ATPD_CODEX_STEP_INDEX:-CODEX_STEPS.md}"
ARCH_FILE="${ATPD_CODEX_ARCH_FILE:-.codex/CURRENT_ARCHITECTURE.md}"
MANIFEST_DIR="${ATPD_CODEX_MANIFEST_DIR:-.codex/steps}"
REPORT_DIR="${ATPD_CODEX_REPORT_DIR:-reports}"
MODE="between-step"

if (( $# > 1 )); then
  printf 'Usage: %s [--resume]\n' "$0" >&2
  exit 2
fi
case "${1:-}" in
  "") ;;
  --resume) MODE="resume-current-step" ;;
  --help)
    printf 'Usage: %s [--resume]\n' "$0"
    printf '  default   validate a clean between-step checkpoint\n'
    printf '  --resume  validate an explicit current-step in-progress resume\n'
    exit 0
    ;;
  *)
    printf 'Usage: %s [--resume]\n' "$0" >&2
    exit 2
    ;;
esac

fail() { printf 'PRECHECK FAIL: %s\n' "$*" >&2; exit 1; }
info() { printf 'PRECHECK: %s\n' "$*"; }

command -v git >/dev/null 2>&1 || fail "git is not installed or not in PATH"
command -v awk >/dev/null 2>&1 || fail "awk is not installed or not in PATH"
command -v rg >/dev/null 2>&1 || printf 'PRECHECK WARNING: ripgrep (rg) not found; install it for token-efficient search-first execution.\n' >&2

repo_root="$(git rev-parse --show-toplevel 2>/dev/null)" || fail "current directory is not inside a Git repository"
cd "$repo_root"
info "repository: $repo_root"

case "$repo_root" in
  /mnt/[a-zA-Z]/*)
    printf 'PRECHECK WARNING: repository is under %s. Prefer the WSL Linux filesystem, e.g. ~/work/atpd.\n' "$repo_root" >&2
    ;;
esac

branch="$(git branch --show-current)"
[[ "$branch" == "$EXPECTED_BRANCH" ]] || fail "expected branch '$EXPECTED_BRANCH', found '$branch'"
info "branch: $branch"

for required in "$MASTER_PLAN" "$STATE_FILE" "$STEP_INDEX" "$ARCH_FILE" "$MANIFEST_DIR"; do
  [[ -e "$required" ]] || fail "required harness path not found: $required"
done
mkdir -p "$REPORT_DIR"

get_state() {
  local key="$1"
  awk -F= -v k="$key" '$1 == k {sub(/^[^=]*=/, ""); print; exit}' "$STATE_FILE"
}

current_step="$(get_state current_step)"
last_completed_step="$(get_state last_completed_step)"
last_commit="$(get_state last_commit)"
status="$(get_state status)"
blocked_reason="$(get_state blocked_reason)"

[[ "$current_step" =~ ^[0-9]+$ ]] || fail "invalid current_step in $STATE_FILE"
[[ "$last_completed_step" =~ ^[0-9]+$ ]] || fail "invalid last_completed_step in $STATE_FILE"
(( current_step >= 1 && current_step <= 31 )) || fail "current_step out of range: $current_step"
(( last_completed_step >= 0 && last_completed_step <= 30 )) || fail "last_completed_step out of range: $last_completed_step"

if [[ "$status" == "complete" ]]; then
  [[ "$MODE" == "between-step" ]] || fail "cannot resume a complete refactor"
  [[ "$last_completed_step" -eq 30 ]] || fail "status=complete but last_completed_step=$last_completed_step"
  info "state is complete; all 30 Steps are recorded as finished"
  exit 0
fi

expected_step=$((last_completed_step + 1))
[[ "$current_step" -eq "$expected_step" ]] || fail "state mismatch: current_step=$current_step but last_completed_step+1=$expected_step"

case "$status" in
  ready)
    [[ "$MODE" == "between-step" ]] || fail "status=ready is a between-step checkpoint; use normal preflight"
    ;;
  in_progress)
    [[ "$MODE" == "resume-current-step" ]] || fail "state is in_progress; resume explicitly with --resume"
    ;;
  blocked) fail "state is blocked${blocked_reason:+: $blocked_reason}" ;;
  *) fail "invalid status '$status' in $STATE_FILE" ;;
esac

if [[ "$last_completed_step" -gt 0 ]]; then
  [[ -n "$last_commit" ]] || fail "last_completed_step is non-zero but last_commit is empty"
  git cat-file -e "${last_commit}^{commit}" 2>/dev/null || fail "last_commit '$last_commit' does not exist"
  git merge-base --is-ancestor "$last_commit" HEAD || fail "last_commit '$last_commit' is not an ancestor of HEAD"
fi

manifest="$(find "$MANIFEST_DIR" -maxdepth 1 -type f -name "$(printf '%02d' "$current_step")-*.md" -print)"
manifest_count="$(printf '%s\n' "$manifest" | sed '/^$/d' | wc -l | tr -d ' ')"
[[ "$manifest_count" -eq 1 ]] || fail "expected exactly one manifest for Step $current_step, found $manifest_count"

# Verify specialized plans named by this manifest exist, except explicit no-plan Step 30.
missing=0
while IFS= read -r plan_path; do
  [[ -z "$plan_path" ]] && continue
  if [[ ! -f "$plan_path" ]]; then
    printf 'PRECHECK FAIL: manifest references missing plan: %s\n' "$plan_path" >&2
    missing=1
  fi
done < <(grep -oE 'docs/refactor/[A-Za-z0-9_.-]+\.md' "$manifest" | sort -u || true)
[[ "$missing" -eq 0 ]] || exit 1

is_runtime_path() {
  case "$1" in
    .rework-state|reports/*) return 0 ;;
    *) return 1 ;;
  esac
}

scope_files="$(sed -n '/^## Primary starting files/,/^## /p' "$manifest" |
  sed -n 's/^[[:space:]]*-[[:space:]]*`\([^`]*\)`.*/\1/p')"

resume_scope_allows() {
  local path="$1"
  local source header

  while IFS= read -r source; do
    [[ -z "$source" ]] && continue
    [[ "$path" == "$source" ]] && return 0
    case "$source" in
      src/*.c)
        header="include/$(basename "${source%.c}").h"
        [[ "$path" == "$header" ]] && return 0
        ;;
    esac
  done <<< "$scope_files"

  case "$path" in
    tests/*|Makefile|android/Makefile) return 0 ;;
    *) return 1 ;;
  esac
}

status_all="$(git status --porcelain --untracked-files=all)"
if [[ "$MODE" == "between-step" ]]; then
  status_filtered="$(printf '%s\n' "$status_all" | awk '
    {
      p=substr($0,4)
      if (p==".rework-state") next
      if (p ~ /^reports\//) next
      print
    }
  ')"
  if [[ -n "$status_filtered" ]]; then
    printf 'PRECHECK FAIL: working tree has non-runtime changes:\n%s\n' "$status_filtered" >&2
    printf 'Use --resume only for an explicitly marked, auditable current-step in-progress state.\n' >&2
    exit 1
  fi
  info "mode: between-step"
  info "working tree: clean except allowed runtime checkpoint/report files"
else
  info "mode: resume-current-step"
  while IFS= read -r entry; do
    [[ -z "$entry" ]] && continue
    path="${entry:3}"
    [[ "$path" == *" -> "* ]] && fail "renames are not an auditable resume path: $path"
    is_runtime_path "$path" && continue
    resume_scope_allows "$path" || fail "dirty path is outside Step $current_step resume scope: $path"
    info "resume scope: $path"
  done <<< "$status_all"
fi

info "master plan: $MASTER_PLAN"
info "step index: $STEP_INDEX"
info "architecture checkpoint: $ARCH_FILE"
info "last completed step: $last_completed_step"
info "next step: $current_step"
info "manifest: $manifest"

if command -v make >/dev/null 2>&1; then info "make: $(command -v make)"; else printf 'PRECHECK WARNING: make not found.\n' >&2; fi
if command -v cc >/dev/null 2>&1; then info "cc: $(command -v cc)";
elif command -v gcc >/dev/null 2>&1; then info "gcc: $(command -v gcc)";
elif command -v clang >/dev/null 2>&1; then info "clang: $(command -v clang)";
else printf 'PRECHECK WARNING: no C compiler found in PATH.\n' >&2; fi

printf '\nPRECHECK PASS\n'
if [[ "$MODE" == "resume-current-step" ]]; then
  printf 'Next action: audit git diff/status, then read the Step %s manifest and required plan before resuming existing changes.\n' "$current_step"
else
  printf 'Next action: read CODEX_AUTOPILOT.md, the Step %s entry in %s, %s, and only the specialized plan/source hits required by the manifest.\n' "$current_step" "$STEP_INDEX" "$manifest"
fi
