# Step 07 Manifest — Remove ATPD-owned eBPF

Audit mode: `repo-wide`

## Read first

- `CODEX_AUTOPILOT.md`
- `.rework-state`
- `.codex/CURRENT_ARCHITECTURE.md`
- this manifest: `.codex/steps/07-remove-atpd-owned-ebpf.md`

## Specialized plan

- `docs/refactor/ATPD_EBPF_MODULE_REMOVAL_PLAN.md`

Do not read unrelated refactor MDs.

## Primary starting files

- `src/ebpf.c`
- `src/ebpf_common.c`
- `include/ebpf.h`
- `src/`
- `include/`
- `Makefile`

These are starting points, not an exhaustive file list. Expand only from evidence.

## Required targeted searches

Run targeted symbol searches before opening additional files:

```bash
rg -n 'sys_bpf|__NR_bpf|SYS_bpf|BPF_|RLIMIT_MEMLOCK|ebpf_probe|ENABLE_EBPF|MODE_EBPF' src include tests Makefile 2>/dev/null || true
```

Open only relevant hits unless the audit mode is `repo-wide` and a broader ownership/deletion audit is required.

## Scope discipline

- Do not scan/read all source files up front.
- Do not read completed Step reports unless a concrete dependency requires one.
- Do not implement future-Step work; record it as TODO.
- Before deleting a symbol/file/field, perform a complete callsite audit for that item.
- Prefer narrow file/range reads after `rg` rather than dumping large files.

## Core gates

- No ATPD sys_bpf/probe ownership
- sing-box remains sole ebpf-in dataplane owner
- No fake ATPD eBPF telemetry

## Build/test policy

- Run the project's incremental build plus tests relevant to this Step.
- Use concise output on PASS.
- Expand logs only for failures.
- Full clean build/full suite is required at phase checkpoints and final stability/release Steps, not mechanically after every local Step unless the project requires it.

## Report

Write `reports/step-07-report.md` on PASS or `reports/step-07-failed.md` on hard stop.
Keep the report incremental: changed behavior, ownership, tests, commands, gates, TODOs, commit hash.

## Commit

`refactor(ebpf): remove ATPD-owned dataplane management`

After PASS and commit, update `.rework-state` and add only durable new architecture facts to `.codex/CURRENT_ARCHITECTURE.md`.