# Step 30 Manifest — RC/stable validation

Audit mode: `repo-wide`

## Read first

- `CODEX_AUTOPILOT.md`
- `.rework-state`
- `.codex/CURRENT_ARCHITECTURE.md`
- this manifest: `.codex/steps/30-rc-stable-validation.md`

## Specialized plan

- No specialized MD; use master invariants and prior reports only as needed.

Do not read unrelated refactor MDs.

## Primary starting files

- `src/`
- `include/`
- `tests/`
- `scripts/`
- `reports/`

These are starting points, not an exhaustive file list. Expand only from evidence.

## Required targeted searches

Run targeted symbol searches before opening additional files:

```bash
rg -n 'ASan|UBSan|TSan|soak|Android|Magisk|KernelSU|APatch' src include tests Makefile 2>/dev/null || true
```

Open only relevant hits unless the audit mode is `repo-wide` and a broader ownership/deletion audit is required.

## Scope discipline

- Do not scan/read all source files up front.
- Do not read completed Step reports unless a concrete dependency requires one.
- Do not implement future-Step work; record it as TODO.
- Before deleting a symbol/file/field, perform a complete callsite audit for that item.
- Prefer narrow file/range reads after `rg` rather than dumping large files.

## Core gates

- Sanitizer matrix completed where supported
- Android recovery/transition scenarios documented
- Stable requires soak/release gate, not build-only

## Build/test policy

- Run the project's incremental build plus tests relevant to this Step.
- Use concise output on PASS.
- Expand logs only for failures.
- Full clean build/full suite is required at phase checkpoints and final stability/release Steps, not mechanically after every local Step unless the project requires it.

## Report

Write `reports/step-30-report.md` on PASS or `reports/step-30-failed.md` on hard stop.
Keep the report incremental: changed behavior, ownership, tests, commands, gates, TODOs, commit hash.

## Commit

`test(release): complete RC stability matrix`

After PASS and commit, update `.rework-state` and add only durable new architecture facts to `.codex/CURRENT_ARCHITECTURE.md`.