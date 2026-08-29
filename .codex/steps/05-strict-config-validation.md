# Step 05 Manifest — Strict config validation

Audit mode: `local`

## Read first

- `CODEX_AUTOPILOT.md`
- `.rework-state`
- `.codex/CURRENT_ARCHITECTURE.md`
- this manifest: `.codex/steps/05-strict-config-validation.md`

## Specialized plan

- `docs/refactor/ATPD_CONFIG_VALIDATOR_STRICTNESS_HARDENING_PLAN.md`

Do not read unrelated refactor MDs.

## Primary starting files

- `src/config_validator.c`
- `src/config.c`
- `include/config.h`
- `tests/`

These are starting points, not an exhaustive file list. Expand only from evidence.

## Required targeted searches

Run targeted symbol searches before opening additional files:

```bash
rg -n 'config_validate_key|unknown\ key|strtol|atoi|bool|duplicate|alias' src include tests Makefile 2>/dev/null || true
```

Open only relevant hits unless the audit mode is `repo-wide` and a broader ownership/deletion audit is required.

## Scope discipline

- Do not scan/read all source files up front.
- Do not read completed Step reports unless a concrete dependency requires one.
- Do not implement future-Step work; record it as TODO.
- Before deleting a symbol/file/field, perform a complete callsite audit for that item.
- Prefer narrow file/range reads after `rg` rather than dumping large files.

## Core gates

- Unknown key fails
- Invalid numeric/bool fails
- No silent truncation

## Build/test policy

- Run the project's incremental build plus tests relevant to this Step.
- Use concise output on PASS.
- Expand logs only for failures.
- Full clean build/full suite is required at phase checkpoints and final stability/release Steps, not mechanically after every local Step unless the project requires it.

## Report

Write `reports/step-05-report.md` on PASS or `reports/step-05-failed.md` on hard stop.
Keep the report incremental: changed behavior, ownership, tests, commands, gates, TODOs, commit hash.

## Commit

`fix(config): enforce strict typed validation`

After PASS and commit, update `.rework-state` and add only durable new architecture facts to `.codex/CURRENT_ARCHITECTURE.md`.