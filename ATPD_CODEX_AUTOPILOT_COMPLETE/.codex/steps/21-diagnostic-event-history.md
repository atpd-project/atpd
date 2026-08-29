# Step 21 Manifest — Diagnostic event history

Audit mode: `repo-symbol`

## Read first

- `CODEX_AUTOPILOT.md`
- `.rework-state`
- `.codex/CURRENT_ARCHITECTURE.md`
- this manifest: `.codex/steps/21-diagnostic-event-history.md`

## Specialized plan

- `docs/refactor/ATPD_ERROR_DIAGNOSTICS_HARDENING_PLAN.md`

Do not read unrelated refactor MDs.

## Primary starting files

- `src/atpd_error.c`
- `include/atpd_error.h`
- `src/atpd_context.c`
- `src/logger.c`
- `tests/`

These are starting points, not an exhaustive file list. Expand only from evidence.

## Required targeted searches

Run targeted symbol searches before opening additional files:

```bash
rg -n 'last_error|error_count|atpd_error|errno|mutex|record|get_last' src include tests Makefile 2>/dev/null || true
```

Open only relevant hits unless the audit mode is `repo-wide` and a broader ownership/deletion audit is required.

## Scope discipline

- Do not scan/read all source files up front.
- Do not read completed Step reports unless a concrete dependency requires one.
- Do not implement future-Step work; record it as TODO.
- Before deleting a symbol/file/field, perform a complete callsite audit for that item.
- Prefer narrow file/range reads after `rg` rather than dumping large files.

## Core gates

- One diagnostic history owner
- Copy-out getters
- No logging while holding diagnostic lock

## Build/test policy

- Run the project's incremental build plus tests relevant to this Step.
- Use concise output on PASS.
- Expand logs only for failures.
- Full clean build/full suite is required at phase checkpoints and final stability/release Steps, not mechanically after every local Step unless the project requires it.

## Report

Write `reports/step-21-report.md` on PASS or `reports/step-21-failed.md` on hard stop.
Keep the report incremental: changed behavior, ownership, tests, commands, gates, TODOs, commit hash.

## Commit

`refactor(error): centralize diagnostic event history`

After PASS and commit, update `.rework-state` and add only durable new architecture facts to `.codex/CURRENT_ARCHITECTURE.md`.