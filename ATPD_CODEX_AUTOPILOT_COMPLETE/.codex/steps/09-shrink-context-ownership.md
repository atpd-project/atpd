# Step 09 Manifest — Shrink context ownership

Audit mode: `repo-wide`

## Read first

- `CODEX_AUTOPILOT.md`
- `.rework-state`
- `.codex/CURRENT_ARCHITECTURE.md`
- this manifest: `.codex/steps/09-shrink-context-ownership.md`

## Specialized plan

- `docs/refactor/ATPD_CONTEXT_STATE_OWNERSHIP_REFACTOR_PLAN.md`
- `docs/refactor/ATPD_CONTEXT_PUBLIC_BOUNDARY_REFACTOR_PLAN.md`

Do not read unrelated refactor MDs.

## Primary starting files

- `include/atpd_context.h`
- `src/atpd_context.c`
- `src/session.c`
- `src/netlink.c`
- `src/status.c`
- `src/atpd_error.c`

These are starting points, not an exhaustive file list. Expand only from evidence.

## Required targeted searches

Run targeted symbol searches before opening additional files:

```bash
rg -n 'g_atpd_ctx|sessions|xfrm_fd|last_error|readiness|uptime|killswitch|atpd_error_record|emergency_drain' src include tests Makefile 2>/dev/null || true
```

Open only relevant hits unless the audit mode is `repo-wide` and a broader ownership/deletion audit is required.

## Scope discipline

- Do not scan/read all source files up front.
- Do not read completed Step reports unless a concrete dependency requires one.
- Do not implement future-Step work; record it as TODO.
- Before deleting a symbol/file/field, perform a complete callsite audit for that item.
- Prefer narrow file/range reads after `rg` rather than dumping large files.

## Core gates

- No public mutable g_atpd_ctx
- Context owns no sessions/eBPF/XFRM fd
- Reload does not reset daemon uptime

## Build/test policy

- Run the project's incremental build plus tests relevant to this Step.
- Use concise output on PASS.
- Expand logs only for failures.
- Full clean build/full suite is required at phase checkpoints and final stability/release Steps, not mechanically after every local Step unless the project requires it.

## Report

Write `reports/step-09-report.md` on PASS or `reports/step-09-failed.md` on hard stop.
Keep the report incremental: changed behavior, ownership, tests, commands, gates, TODOs, commit hash.

## Commit

`refactor(context): shrink runtime context ownership`

After PASS and commit, update `.rework-state` and add only durable new architecture facts to `.codex/CURRENT_ARCHITECTURE.md`.