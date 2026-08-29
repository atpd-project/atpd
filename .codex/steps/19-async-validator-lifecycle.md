# Step 19 Manifest — Async validator lifecycle

Audit mode: `local`

## Read first

- `CODEX_AUTOPILOT.md`
- `.rework-state`
- `.codex/CURRENT_ARCHITECTURE.md`
- this manifest: `.codex/steps/19-async-validator-lifecycle.md`

## Specialized plan

- `docs/refactor/ATPD_ASYNC_VALIDATE_LIFECYCLE_HARDENING_PLAN.md`

Do not read unrelated refactor MDs.

## Primary starting files

- `src/async_validate.c`
- `include/async_validate.h`
- `tests/`

These are starting points, not an exhaustive file list. Expand only from evidence.

## Required targeted searches

Run targeted symbol searches before opening additional files:

```bash
rg -n 'waitpid|WNOHANG|EOF|timeout|pipe|CLOEXEC|cancel|finish|reap' src include tests Makefile 2>/dev/null || true
```

Open only relevant hits unless the audit mode is `repo-wide` and a broader ownership/deletion audit is required.

## Scope discipline

- Do not scan/read all source files up front.
- Do not read completed Step reports unless a concrete dependency requires one.
- Do not implement future-Step work; record it as TODO.
- Before deleting a symbol/file/field, perform a complete callsite audit for that item.
- Prefer narrow file/range reads after `rg` rather than dumping large files.

## Core gates

- Child reaped exactly once
- EOF/timeout race cannot hang
- Shutdown can cancel without zombies

## Build/test policy

- Run the project's incremental build plus tests relevant to this Step.
- Use concise output on PASS.
- Expand logs only for failures.
- Full clean build/full suite is required at phase checkpoints and final stability/release Steps, not mechanically after every local Step unless the project requires it.

## Report

Write `reports/step-19-report.md` on PASS or `reports/step-19-failed.md` on hard stop.
Keep the report incremental: changed behavior, ownership, tests, commands, gates, TODOs, commit hash.

## Commit

`fix(async-validate): unify child completion lifecycle`

After PASS and commit, update `.rework-state` and add only durable new architecture facts to `.codex/CURRENT_ARCHITECTURE.md`.