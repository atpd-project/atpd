# ATPD Current Architecture Invariants

> This file is a compact, cumulative checkpoint for Codex.
> It records architecture facts that are already established by completed Steps.
> Keep it short. Update only when a completed Step changes an invariant.

## Baseline invariants

- Target implementation: ATPD C on branch `ebpf-native-api`.
- ATPD is an independent privileged routing/control daemon.
- sing-box is an independent child/worker.
- sing-box owns the `ebpf-in` dataplane.
- ATPD must not duplicate sing-box eBPF program/map/probe lifecycle.
- One runtime/resource state should have one authoritative owner.
- Configuration represents desired configuration, not observed runtime state.
- Runtime owners should expose coherent snapshots to consumers.
- Status aggregates owner snapshots; it does not become another state owner.
- Do not create a new god global or turn `atpd_context` into `atpd_global` v2.
- Do not create a replacement umbrella header such as `common.h`, `base.h`, or `all.h`.
- Child PID, FD, timer, reactor registration, and async callback ownership must be explicit.
- `ATPD_GO_REWRITE_PLAN.md` is outside this C refactor execution.

## Completed-step facts

- Step 01: Generic operation outcomes use typed `atp_result_t`; diagnostics remain owned by `atpd_error`, and internal result codes are not process or wire status codes.
- Step 02: Product version comes from `VERSION`; generated Git/dirty metadata lives under `build/` and is exposed through the version API.
- Step 03: Build hardening is controlled by Makefile flags; core `atp.h` no longer provides `cfg_*` compatibility aliases or Fortify policy.
- Step 04: Configuration is a plain desired-state value; runtime readiness, VPN observation, CLI state, and synchronization are owned outside `atp_config_t`.
- Step 05: Configuration parsing uses one strict typed schema; unknown, malformed, duplicate, invalid-type, and truncating inputs are rejected before candidate acceptance.
- Step 06: Reload validates and merges a stack candidate before commit; active config/runtime are retained on failure and reload generation advances only after successful apply.
- Step 07: ATPD-owned eBPF files/sys_bpf probing removed; sing-box is sole ebpf-in owner.
- Step 08: atpd_global container and aliases removed; daemon orchestration owns explicit private dependencies and request flags.
- Step 09: atpd_context is opaque and only owns lifecycle/VPN snapshots; session, XFRM, readiness, statistics, and error state remain with their subsystem owners.
- Step 10: Startup phases record completed ownership and roll back through registered reverse-order cleanup; service stop/reap/destroy is synchronous before context release, and reactor teardown follows its dependents.
- Step 11: Daemon-mode parents wait for an explicit startup result; main delegates service lifecycle to its owner, prioritizes shutdown over pending control work, and publishes STOPPED only after teardown.
- Step 12: Reactor initialization and signal replacement are transactional; cached FD interests follow successful kernel updates.
- Step 13: Service stop/reap is the sole child lifecycle owner; async stop uses one bounded reactor timer path.

## Update rule

After a successful Step, add only durable facts such as:

```text
- Step 08: g_atpd/atpd_global removed.
- Step 09: public mutable g_atpd_ctx removed; session/XFRM/error ownership moved to subsystem owners.
```

Do not paste Step reports or implementation narratives here.
Aim to keep this file below roughly 100 lines.
