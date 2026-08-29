# Step 15 Manifest — Thin API control facade

Audit mode: `repo-symbol`

## Read first

- `CODEX_AUTOPILOT.md`
- `.rework-state`
- `.codex/CURRENT_ARCHITECTURE.md`
- this manifest: `.codex/steps/15-thin-api-control-facade.md`

## Specialized plan

- `docs/refactor/ATPD_API_CONTROL_BOUNDARY_REFACTOR_PLAN.md`

Do not read unrelated refactor MDs.

## Primary starting files

- `src/api.c`
- `include/api.h`
- `src/singbox_api.c`
- `src/config.c`
- `src/netlink.c`

These are starting points, not an exhaustive file list. Expand only from evidence.

## Required targeted searches

Run targeted symbol searches before opening additional files:

```bash
rg -n 'sleep|retry|g_config|base_url|secret|timeout|desired|reconcile|VPN' src include tests Makefile 2>/dev/null || true
```

Open only relevant hits unless the audit mode is `repo-wide` and a broader ownership/deletion audit is required.

## Scope discipline

- Do not scan/read all source files up front.
- Do not read completed Step reports unless a concrete dependency requires one.
- Do not implement future-Step work; record it as TODO.
- Before deleting a symbol/file/field, perform a complete callsite audit for that item.
- Prefer narrow file/range reads after `rg` rather than dumping large files.

## Core gates

- API does not own transport
- No synchronous sleep retry loop
- Desired state is separate from observation/reconcile

## Build/test policy

- Run the project's incremental build plus tests relevant to this Step.
- Use concise output on PASS.
- Expand logs only for failures.
- Full clean build/full suite is required at phase checkpoints and final stability/release Steps, not mechanically after every local Step unless the project requires it.

## Report

Write `reports/step-15-report.md` on PASS or `reports/step-15-failed.md` on hard stop.
Keep the report incremental: changed behavior, ownership, tests, commands, gates, TODOs, commit hash.

## Commit

`refactor(api): make control API a thin facade`

After PASS and commit, update `.rework-state` and add only durable new architecture facts to `.codex/CURRENT_ARCHITECTURE.md`.