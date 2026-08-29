# Step 01 Manifest — Result model

Audit mode: `repo-symbol`

## Read first

- `CODEX_AUTOPILOT.md`
- `.rework-state`
- `.codex/CURRENT_ARCHITECTURE.md`
- this manifest: `.codex/steps/01-result-model.md`

## Specialized plan

- `docs/refactor/ATPD_RESULT_ERROR_MODEL_REFACTOR_PLAN.md`

Do not read unrelated refactor MDs.

## Primary starting files

- `include/atp_error.h`
- `src/atpd_error.c`
- `src/main.c`
- `src/uds.c`
- `src/api.c`

These are starting points, not an exhaustive file list. Expand only from evidence.

## Required targeted searches

Run targeted symbol searches before opening additional files:

```bash
rg -n 'ATP_ERR_|atp_error_t|atp_strerror|return\ ATP_ERR|exit\(' src include tests Makefile 2>/dev/null || true
```

Open only relevant hits unless the audit mode is `repo-wide` and a broader ownership/deletion audit is required.

## Scope discipline

- Do not scan/read all source files up front.
- Do not read completed Step reports unless a concrete dependency requires one.
- Do not implement future-Step work; record it as TODO.
- Before deleting a symbol/file/field, perform a complete callsite audit for that item.
- Prefer narrow file/range reads after `rg` rather than dumping large files.

## Core gates

- No ATP_ERR_EBPF
- Internal negative result is not leaked as process exit/wire code
- Result status and diagnostic history are distinct

## Build/test policy

- Run the project's incremental build plus tests relevant to this Step.
- Use concise output on PASS.
- Expand logs only for failures.
- Full clean build/full suite is required at phase checkpoints and final stability/release Steps, not mechanically after every local Step unless the project requires it.

## Report

Write `reports/step-01-report.md` on PASS or `reports/step-01-failed.md` on hard stop.
Keep the report incremental: changed behavior, ownership, tests, commands, gates, TODOs, commit hash.

## Commit

`refactor(result): clarify internal result model`

After PASS and commit, update `.rework-state` and add only durable new architecture facts to `.codex/CURRENT_ARCHITECTURE.md`.