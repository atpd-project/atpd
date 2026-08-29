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

None yet.

## Update rule

After a successful Step, add only durable facts such as:

```text
- Step 07: ATPD-owned eBPF files/sys_bpf probing removed; sing-box is sole ebpf-in owner.
- Step 08: g_atpd/atpd_global removed.
- Step 09: public mutable g_atpd_ctx removed; session/XFRM/error ownership moved to subsystem owners.
```

Do not paste Step reports or implementation narratives here.
Aim to keep this file below roughly 100 lines.
