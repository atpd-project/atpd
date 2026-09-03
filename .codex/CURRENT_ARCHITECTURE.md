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
- Step 14: Native API transport reports unavailable/unsupported operations explicitly; process lifecycle remains service-owned.
- Step 15: API context is a thin control facade with no duplicated transport configuration or reactor ownership.
- Step 16: Netlink owns XFRM FD registration and only publishes successful attachment state.
- Step 17: Session refcount/state transitions are terminal-safe; registry close-all uses a dynamic snapshot and deferred GC.
- Step 18: Session remains the production splice datapath owner; standalone splice helpers are utility-only and retry EINTR safely.
- Step 19: Async validation owns one child-reap path and drains validator output before completing callbacks.
- Step 20: UDS owns bounded client state, buffered responses, idle cleanup, peer checks, and safe socket-path replacement.
- Step 21: Diagnostic history is owned by `atpd_error`; getters return copies and logging occurs outside its mutex.
- Step 22: Logger validates emitted levels, synchronizes threshold access, and exposes only implemented sinks.
- Step 23: Utility command execution is bounded and shell-free; process ownership and proc metrics use starttime-aware identity.
- Step 24: CLI parsing is a pure, strict typed boundary; paths reject truncation, conflicts/trailing arguments fail, and CLI types do not depend on logger internals.
- Step 25: Status collection has a bounded owner-snapshot boundary; render paths do not synchronously poll Native API or invent eBPF metrics.
- Pre-RC Step 25/26 correction: `api_ctx` alone owns the mutable Native API snapshot; a bounded refresh worker publishes results back through the reactor, and status/UDS/UI consume only the generation-stamped copy-out snapshot.
- Step 26: UI rendering centralizes ANSI filtering, keeps UDS status plain, and truncates UTF-8 at codepoint boundaries.
- Step 27: CLI and UI implementation units no longer depend on the legacy `atp.h` umbrella; no replacement umbrella header exists.
- Step 28: Lifecycle failure paths explicitly handle timer/FD registration failures and retain child ownership until reap.
- Step 29: Resource regression scripts record memory/FD/thread baseline and recovery, enforce growth gates, and clean only test-owned processes/resources.
- Pre-RC remediation: daemon orchestration retains the selected config source for reload; status renders only an immutable owner snapshot through a stack-owned UI context, and resource gates preserve metrics while validating test-owned process identity.

## Update rule

After a successful Step, add only durable facts such as:

```text
- Step 08: g_atpd/atpd_global removed.
- Step 09: public mutable g_atpd_ctx removed; session/XFRM/error ownership moved to subsystem owners.
```

Do not paste Step reports or implementation narratives here.
Aim to keep this file below roughly 100 lines.
