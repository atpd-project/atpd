# Step 26 Manifest — UI rendering boundary

Audit mode: `local`

## Read first

- `CODEX_AUTOPILOT.md`
- `.rework-state`
- `.codex/CURRENT_ARCHITECTURE.md`
- this manifest: `.codex/steps/26-ui-rendering-boundary.md`

## Specialized plan

- `docs/refactor/ATPD_UI_RENDERING_BOUNDARY_HARDENING_PLAN.md`

Do not read unrelated refactor MDs.

## Primary starting files

- `src/ui.c`
- `include/ui.h`
- `src/status.c`
- `src/uds.c`
- `tests/`

These are starting points, not an exhaustive file list. Expand only from evidence.

## Required targeted searches

Run targeted symbol searches before opening additional files:

```bash
rg -n 'no_color|ANSI|width|UTF\-8|FILE|render' src include tests Makefile 2>/dev/null || true
```

Open only relevant hits unless the audit mode is `repo-wide` and a broader ownership/deletion audit is required.

## Scope discipline

- Do not scan/read all source files up front.
- Do not read completed Step reports unless a concrete dependency requires one.
- Do not implement future-Step work; record it as TODO.
- Before deleting a symbol/file/field, perform a complete callsite audit for that item.
- Prefer narrow file/range reads after `rg` rather than dumping large files.

## Core gates

- No global output sink/config read
- Plain/UDS output has no ANSI
- UTF-8 truncation is safe

## Build/test policy

- Run the project's incremental build plus tests relevant to this Step.
- Use concise output on PASS.
- Expand logs only for failures.
- Full clean build/full suite is required at phase checkpoints and final stability/release Steps, not mechanically after every local Step unless the project requires it.

## Report

Write `reports/step-26-report.md` on PASS or `reports/step-26-failed.md` on hard stop.
Keep the report incremental: changed behavior, ownership, tests, commands, gates, TODOs, commit hash.

## Commit

`refactor(ui): isolate presentation rendering`

After PASS and commit, update `.rework-state` and add only durable new architecture facts to `.codex/CURRENT_ARCHITECTURE.md`.