# ATPD Architecture Invariants

Authoritative architectural invariants, subsystem ownership boundaries, and design constraints for ATPD.

---

## 1. System Identity and Dataplane Ownership

* **Daemon Role**: ATPD is an independent privileged routing and control daemon; sing-box is an independent child/worker process.
* **Dataplane Ownership**: sing-box exclusively owns the `ebpf-in` dataplane. ATPD must not duplicate sing-box eBPF program/map/probe lifecycles and contains no `sys_bpf` calls or ATPD-owned eBPF probing.
* **Single Authority**: Every runtime resource and state has exactly one authoritative owner.
* **No Global Containers**: No god global or singleton runtime containers (`g_atpd`, `atpd_global`); `atpd_context` is opaque and restricted to lifecycle/VPN snapshots.
* **No Umbrella Headers**: No umbrella headers (e.g. `common.h`, `base.h`, `all.h`, or legacy `atp.h` umbrella). Subsystems declare and include only their explicit dependencies.
* **Explicit Resource Tracking**: Child PID, FD, timer, reactor registration, and async callback ownership must be explicit and deterministic.

---

## 2. Configuration, Reload & CLI

* **Desired State Only**: Configuration represents desired configuration, not observed runtime state. Runtime readiness, VPN observation, CLI state, and synchronization are owned outside `atp_config_t`.
* **Strict Typed Validation**: Configuration parsing uses a strict typed schema; unknown, malformed, duplicate, invalid-type, and truncating inputs are rejected before candidate acceptance.
* **Transactional Reload**: Reload validates and merges a candidate configuration before commit; active configuration and runtime state are retained on failure, and reload generation numbers advance only after successful apply.
* **Config Source Retention**: Daemon orchestration retains the selected absolute configuration source for reloads.
* **Strict CLI Boundary**: CLI parsing is a pure, strict typed boundary; paths reject truncation, argument conflicts or trailing arguments fail immediately, and CLI types do not depend on logger internals.

---

## 3. Lifecycle, Service & Reactor

* **Deterministic Handshake**: Daemon-mode parents wait for an explicit startup handshake result via pipe before reporting success; failure triggers reverse-order rollback.
* **Service Lifecycle Authority**: The service subsystem is the sole owner of child process stop, reap, and destruction. Async stop uses a single bounded reactor timer path.
* **Safe Shutdown & Teardown**: Teardown is synchronous before context release; main prioritizes shutdown over pending control work and publishes `STOPPED` only after complete teardown.
* **Transactional Reactor**: Reactor initialization and signal handler replacement are transactional; cached FD interests reflect successful kernel epoll registrations.
* **Process Identity**: Process ownership and proc metrics use starttime-aware identity to eliminate PID reuse races.

---

## 4. Status, UI & Observability

* **Owner Snapshot Aggregation**: Status aggregates coherent owner snapshots; it does not own duplicate state or become an alternate state owner.
* **Bounded Status Collection**: Status collection enforces a bounded snapshot boundary; render paths do not synchronously poll Native API, filesystem, sockets, or netlink.
* **Native API Snapshot Boundary**: `api_ctx` alone owns the mutable Native API snapshot; a bounded refresh worker publishes results back through the reactor, and status/UDS/UI consume only generation-stamped copy-out snapshots.
* **Stack-Owned UI Boundary**: UI rendering operates through stack-owned `ui_render_ctx_t` with explicit `FILE *`, centralized ANSI filtering, plain output for UDS requests, and codepoint-safe UTF-8 truncation boundaries.

---

## 5. Networking, Session & Transport

* **Netlink/XFRM Ownership**: Netlink owns XFRM FD registration and publishes only verified attachment state.
* **Session Lifecycles**: Session refcount and state transitions are terminal-safe; registry close-all operates on dynamic snapshots with deferred cleanup.
* **Splice Datapath**: The session subsystem is the production splice datapath owner; standalone splice utilities retry `EINTR` safely.
* **UDS Transport**: UDS owns bounded client state, buffered responses, idle connection timeouts, peer credential verification, and atomic socket-path replacement.
* **Native API Transport**: Reports unavailable or unsupported operations explicitly without fabricating telemetry; process lifecycle remains service-owned.

---

## 6. Diagnostics, Error Model & Quality Gates

* **Diagnostic Ownership**: Diagnostic history is owned by `atpd_error`; getters return copies and logging occurs outside its mutex locks.
* **Typed Result Model**: Generic operation outcomes use typed `atp_result_t`; internal result codes are never leaked as process exit codes or wire status codes.
* **Logger Reliability**: Logger validates emitted levels, synchronizes threshold access, and writes directly to explicit sinks without recursive lock hazards.
* **Resource Regression Gates**: Resource benchmarks and stress tests record memory (RSS/HWM/PSS), FD, and thread counts, enforcing strict growth gates and least-squares slope verification while cleaning only test-owned resources.
