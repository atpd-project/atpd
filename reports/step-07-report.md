# Step 07 Report - Remove ATPD-owned eBPF

Result: PASS

Plans used:

- `.codex/steps/07-remove-atpd-owned-ebpf.md`
- `docs/refactor/ATPD_EBPF_MODULE_REMOVAL_PLAN.md`

Changed behavior:

- Removed ATPD-owned eBPF probe, BPF syscall helpers, capability state, init phase, cleanup hooks, CLI commands, status telemetry, and obsolete error/config symbols.
- Deleted `src/ebpf.c`, `src/ebpf_common.c`, `include/ebpf.h`, `include/ebpf_common.h`, and `include/compat.h`.
- Removed deleted eBPF sources from both root and Android build manifests.
- Updated README, Android test, migration, example, and runtime wording to identify sing-box as the sole `ebpf-in` owner.
- Kept sing-box `ebpf-in` configuration, lifecycle supervision, and Native API observability intact.

Tests added: none.

Commands and gates:

- `./scripts/codex-preflight.sh`: branch/state checks passed; reported expected in-progress related changes as dirty.
- `make`: PASS.
- `make clean && make`: PASS; only the existing LTO serial-compilation warning.
- `make test`: PASS (VPN mode, logger safety, result/version/config, strict validation, Android service tests).
- Removed CLI check: `atpd help` does not advertise eBPF commands; `atpd ebpf probe` is rejected.
- Ownership invariant search: PASS; no ATPD `sys_bpf`, BPF probe, old eBPF state, telemetry, or memlock symbols in `src`, `include`, `tests`, `Makefile`, or `android`.
- `git diff --check`: PASS.

Deferred: future Steps remain untouched; Step 8 was not started.

Implementation commit: pending signed commit.
