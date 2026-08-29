# Step 04 Manifest — Config model immutability

Audit mode: `repo-symbol`

## Read first

- `CODEX_AUTOPILOT.md`
- `.rework-state`
- `.codex/CURRENT_ARCHITECTURE.md`
- this manifest: `.codex/steps/04-config-model-immutability.md`

## Specialized plan

- `docs/refactor/ATPD_CONFIG_MODEL_IMMUTABILITY_REFACTOR_PLAN.md`

Do not read unrelated refactor MDs.

## Primary starting files

- `include/atp_config.h`
- `include/config.h`
- `src/config.c`
- `src/cli.c`
- `src/service.c`

These are starting points, not an exhaustive file list. Expand only from evidence.

## Required targeted searches

Run targeted symbol searches before opening additional files:

```bash
rg -n 'foreground|verbose|no_color|dry_run|current_vpn_iface|ebpf_config_t|restart_delay|config_mutex|pthread_mutex' src include tests Makefile 2>/dev/null || true
```

Open only relevant hits unless the audit mode is `repo-wide` and a broader ownership/deletion audit is required.

## Scope discipline

- Do not scan/read all source files up front.
- Do not read completed Step reports unless a concrete dependency requires one.
- Do not implement future-Step work; record it as TODO.
- Before deleting a symbol/file/field, perform a complete callsite audit for that item.
- Prefer narrow file/range reads after `rg` rather than dumping large files.

## Core gates

- Config contains desired state only
- No mutex inside config value
- No runtime readiness/VPN observation in config

## Build/test policy

- Run the project's incremental build plus tests relevant to this Step.
- Use concise output on PASS.
- Expand logs only for failures.
- Full clean build/full suite is required at phase checkpoints and final stability/release Steps, not mechanically after every local Step unless the project requires it.

## Report

Write `reports/step-04-report.md` on PASS or `reports/step-04-failed.md` on hard stop.
Keep the report incremental: changed behavior, ownership, tests, commands, gates, TODOs, commit hash.

## Commit

`refactor(config): make configuration value immutable`

After PASS and commit, update `.rework-state` and add only durable new architecture facts to `.codex/CURRENT_ARCHITECTURE.md`.