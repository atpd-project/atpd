# ATPD Rust Rewrite: Architectural Specification & Implementation Plan

## 1. Executive Summary & Core Objective

The goal of this project is to rewrite the **ATPD (Advanced Transparent Proxy Daemon)** control plane from its current C11 implementation into modern, idiomatic **Rust**.

ATPD is an ultra-lightweight, zero-firewall privileged control daemon designed for modern Android devices (GKI 5.10+, KernelSU / APatch / Magisk) and Linux systems. It supervises `sing-box`'s in-kernel eBPF socket capture (`ebpf-in`), interacts with sing-box's Native API via gRPC-Web, performs real-time Netlink / XFRM multi-VPN tunnel sensing (Cloudflare WARP, WireGuard, Tailscale, Google VPN), and provides sub-millisecond Fast-Path Unix Domain Socket (UDS) status queries.

### Why Rust?
- **Deterministic Memory Safety**: Eliminates C-level risks (use-after-free, dangling pointers, buffer overflows, and file descriptor leaks) without introducing garbage collection pauses or runtime bloat.
- **Identical Footprint to C**: Unlike Go (which has a 15–30 MB RSS baseline due to runtime GC and heap structures), a properly architected Rust implementation can maintain an idle RSS baseline of **< 4.0 MB** and a stripped binary size of **< 2.5 MB**.
- **First-Class State Modeling**: Expressive algebraic data types (`enum`), pattern matching, and RAII make complex network state machines (`IDLE` ⇄ `PREDICTING` ⇄ `READY` ⇄ `TEARDOWN`) completely verifiable at compile time.

---

## 2. Architectural Invariants & Non-Negotiables

Any Rust implementation **must strictly preserve** the established ATPD architectural invariants:

1. **Independent Dual-Process Model**:
   - ATPD is the independent privileged routing and supervision control daemon.
   - `sing-box` is an independent child/worker process.
   - **Hard Rule**: Do **not** embed sing-box as an in-process library (`libbox`) or merge them into a single process. If sing-box crashes, panics, or hits an eBPF verifier issue, ATPD must remain alive to maintain routing safety and trigger automated circuit breaker / self-healing recovery.

2. **Dataplane Ownership Boundary**:
   - `sing-box` exclusively owns the eBPF dataplane (`ebpf-in`, maps, TC filters, socket programs).
   - ATPD contains **zero** `sys_bpf` calls and must not probe or duplicate eBPF map lifecycles.

3. **Desired State vs. Observed State Separation**:
   - Configuration represents *desired state only*. It must never be mutated or used as a runtime state bucket.
   - Runtime readiness, tunnel observation, metrics, and IPC state belong to dedicated subsystem owners.

4. **Single-Threaded Reactor Event Loop Paradigm**:
   - To avoid POSIX multithreading hazards on Linux/Android (such as per-thread credential / capability dropping with `setresuid` / `capset`), ATPD's core event loop should remain strictly single-threaded (or pin privileged tasks predictably). Exception: the Native API refresh worker (`src/api.c`) stays a separate thread in Rust, as in C11, so blocking HTTP calls (1 s timeouts) never stall the reactor or status queries. It performs network I/O only (no credential or capability changes), starts with all signals blocked, and hands results to the reactor via a channel plus the reactor `Waker`.

5. **Copy-Out Bounded Snapshots for Observability**:
   - Status rendering and UDS IPC queries must consume generation-stamped, stack-allocated, copy-out snapshots.
   - Status and CLI handlers must **never** synchronously poll Netlink, the filesystem, or network sockets during render.

6. **Binary Naming & Packaging**:
   - The compiled release artifact must remain named `atpd`. Never append implementation identifiers like `-rust` to production binaries.

---

## 3. Target SLOs & Resource Budgets

| Metric | C11 Baseline (Current) | Rust Target SLO | Hard Ceiling (Fail Gate) |
| :--- | :--- | :--- | :--- |
| **Stripped Binary Size** | < 160 KB | **< 2.5 MB** | **< 3.5 MB** |
| **Runtime Resident Set Size (RSS)** | ~2.3 MB | **< 4.0 MB** | **< 6.0 MB** |
| **CLI Status Query Latency** | ~6.1 ms | **< 8.0 ms** | **< 12.0 ms** |
| **Netlink Flap Event Burst (30x)** | < 500 ms | **< 400 ms** | **< 600 ms** |
| **Background Busy Polling** | 0 loops | **0 loops** | 0 loops |
| **Memory Page Alignment** | 16 KB compatible | **16 KB aligned** | Android 15+ compatible |

---

## 4. Tech Stack & Crate Discipline (Avoiding the "Async Rust Bloat")

A major failure mode when rewriting lightweight daemons in Rust is pulling in massive, monolithic crates (`tokio` multi-thread full + `tonic` + `reqwest` + `hyper` + `actix`), resulting in a 20 MB binary with 300+ dependencies and slow compile times.

To uphold the **Ponytail (Lazy Senior Dev) Execution Contract**, ATPD-Rust adopts a **strict minimal-dependency tier**:

```text
Recommended Crate Tier:
├── mio (v1.0)                 ── Lightweight single-threaded epoll event loop
├── rustix (v0.38)             ── Safe, lightweight POSIX/Linux syscall wrapper
├── prost (v0.13)              ── Minimal zero-allocation Protocol Buffers parser
├── serde / serde_json         ── Strict JSON parser for config and UDS payloads
└── netlink-packet-route       ── Strongly-typed Netlink route/link message codecs
```

### Decision (2026-11-03): `libc` instead of `mio` / `rustix`
The C reactor relies on `signalfd`, `eventfd`, and the supervisor on `setresuid`/`initgroups`/`setsid`; neither `mio` nor `rustix` covers all of these. ATPD-Rust therefore depends only on `libc` (plus `serde_json`), with a hand-written single-threaded epoll reactor exposing a poll-style `Event` API.

### Alternative Considered:
- **`tokio` (single-threaded `current_thread` flavor)**:
  - *Feasible* if scoped with `default-features = false, features = ["rt", "net", "time", "signal", "macros", "sync"]`.
  - *Trade-off*: Adds ~1.2 MB to binary size compared to raw `mio` / `rustix`, but simplifies asynchronous timer and channel programming. Either approach is acceptable as long as it adheres to the < 2.5 MB binary budget.

---

## 5. Subsystem Architecture & Rust Mappings

```text
                                Linux / Android Kernel
                                          │
                  ┌───────────────────────┼───────────────────────┐
                  │ Netlink (RTM_*)       │ XFRM Events           │ Process Signals
                  ▼                       ▼                       ▼
            ┌───────────┐           ┌───────────┐           ┌───────────┐
            │  Netlink  │           │   XFRM    │           │ Signal    │
            │  Watcher  │           │  Watcher  │           │ Handler   │
            └─────┬─────┘           └─────┬─────┘           └─────┬─────┘
                  │                       │                       │
                  └───────────────┬───────┴───────────────────────┘
                                  ▼
                    ┌───────────────────────────┐
                    │  Core Event Loop Engine   │  (Single-threaded Epoll)
                    │   (Mio / Tokio Current)   │
                    └─────────────┬─────────────┘
                                  │
         ┌────────────────────────┼────────────────────────┐
         ▼                        ▼                        ▼
┌─────────────────┐      ┌─────────────────┐      ┌─────────────────┐
│ Child Process   │      │ Native API      │      │ Fast-Path UDS   │
│ Supervisor      │      │ gRPC-Web Client │      │ IPC Server      │
│ (Backoff & CB)  │      │ (Status Stream) │      │ (CLI Queries)   │
└────────┬────────┘      └────────┬────────┘      └────────┬────────┘
         │                        │                        │
         ▼ fork/exec              ▼ HTTP/1.1 gRPC-Web      ▼ atpd.sock
   ┌───────────┐            ┌───────────┐            ┌───────────┐
   │ sing-box  │            │ sing-box  │            │ atpd CLI  │
   │ (Worker)  │            │ API (9080)│            │ (Client)  │
   └───────────┘            └───────────┘            └───────────┘
```

### 5.1. Process Supervisor (`supervisor`)
- **Child Execution**: Use `std::process::Command` with `std::os::unix::process::CommandExt`.
- **Pre-Exec Hook**: Use `unsafe { command.pre_exec(...) }` to drop credentials to `root:net_admin` (`AID_NET_ADMIN=3005` on Android) and set up process groups (`setpgid`).
- **PID-Reuse Protection**: Validate the child's `starttime` from `/proc/[pid]/stat` upon startup to prevent PID reuse race conditions.
- **Startup Handshake**: Pass a synchronization pipe to ensure deterministic startup; the parent waits for an explicit handshake signal before declaring success.
- **Circuit Breaker & Backoff**: Maintain a sliding-window failure counter, exponential backoff (e.g., 2s, 4s, 8s, up to cooldown), and clean teardown via `SIGTERM` followed by a bounded `SIGKILL` escalation.

### 5.2. Netlink & Multi-VPN Sensing (`netlink`)
- **Socket**: Open non-blocking Netlink sockets (`NETLINK_ROUTE` and `NETLINK_XFRM`) with groups:
  - `RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR | RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE | RTMGRP_IPV4_RULE`
- **Proxy Interface Filter**: Maintain verified prefixes: `tun`, `warp`, `wg`, `tailscale`, `zt`, `zerotier`, `utun`, `vpn`, `ppp`, `ipsec`, `xfrm`.
- **Debounced Transitions**: Network events trigger a debounced timer (500 ms debounce, 1000 ms VPN settle, matching `NETLINK_DEBOUNCE_MS` / `NETLINK_VPN_SETTLE_MS` in `src/netlink.c`). State transitions must strictly follow:
  - `IDLE` (no secondary VPN active)
  - `PREDICTING` (XFRM SA announced before interface is up)
  - `READY` (debounced stable tunnel active)
  - `TEARDOWN` (XFRM SA deleted)
- **Bypass Route Synchronizer**: Atomically inject/remove custom routing policy rules (`ip rule add ... pref 12400`) when secondary VPNs (e.g. Google VPN `ipsec*`) are present.

### 5.3. Native API & gRPC-Web Client (`nativeapi`)
- **Transport**: Persistent or pooled HTTP/1.1 client connecting to `127.0.0.1:9080` (or configured host/port) with a 1-second timeout.
- **Protobuf Payloads**:
  - `SubscribeStatus`: Read streaming gRPC-Web frame for live Go runtime metrics (goroutines, memory, connection counts, uplink/downlink rates and totals).
  - `GetClashModeStatus`: Query available modes (`mode_list`) and current mode.
  - `SetClashMode`: Encode `ClashMode` protobuf message with `mode` as **field 3** (`Tag 0x1A`), per `message ClashMode { string mode = 3; }` in sing-box `daemon/started_service.proto`.
- **Case-Insensitive Mode Resolution**: Provide case-insensitive mapping against the core's reported `mode_list` and dynamically restore saved modes on tunnel disconnection.

### 5.4. Fast-Path UDS IPC & Dashboard Rendering (`ipc`, `ui`)
- **IPC Protocol**: UNIX Domain Socket at `/data/adb/atp/run/atpd.sock` with `0600` permissions.
- **Peer Credential Verification**: Verify client UID via `SO_PEERCRED` (only `root` / `0` authorized).
- **Snapshot Aggregation**: Aggregates coherent, copy-out snapshots across supervisor, Netlink, and Native API modules without blocking the main event loop.
- **UI Terminal Renderer**: Pure string formatter rendering ANSI dashboard tables, with automatic plain-text stripping when stdout is not a TTY or when serving UDS responses.

### 5.5. Configuration & Transactional Reload (`config`)
- **Format**: `atp.conf` (Shell/INI style) + `config.json` (sing-box JSON).
- **Strict Validation**: Typed schema validation rejecting duplicate keys, out-of-range ports, malformed CIDRs, or invalid user/group credentials.
- **Transactional SIGHUP Reload**: Parse candidate configuration into a new struct; validate against system dependencies; replace the active config value only after validation succeeds (single-threaded, so no lock is needed). If validation fails, retain active configuration and log errors deterministically.

---

## 6. Target Directory Layout

```text
atpd-rust/
├── Cargo.toml
├── build.rs                        # Injects Git commit, build time, and version
├── src/
│   ├── main.rs                     # CLI entrypoint, argument parsing, signal bootstrap
│   ├── app.rs                      # Master daemon lifecycle coordinator
│   ├── config/                     # Configuration definitions, parser & validation
│   │   ├── mod.rs
│   │   ├── atp_conf.rs
│   │   └── validator.rs
│   ├── reactor/                    # Event loop abstractions (mio/epoll)
│   │   ├── mod.rs
│   │   └── timer.rs
│   ├── supervisor/                 # sing-box process manager & circuit breaker
│   │   ├── mod.rs
│   │   ├── process.rs
│   │   └── health.rs
│   ├── netlink/                    # Netlink/XFRM socket, parser, & VPN sensing
│   │   ├── mod.rs
│   │   ├── watcher.rs
│   │   └── bypass.rs
│   ├── nativeapi/                  # sing-box gRPC-Web & Protobuf client
│   │   ├── mod.rs
│   │   ├── client.rs
│   │   ├── proto.rs                # Prost-generated or manual light codecs
│   │   └── status.rs
│   ├── ipc/                        # Unix Domain Socket IPC listener & handler
│   │   ├── mod.rs
│   │   └── server.rs
│   ├── ui/                         # Terminal dashboard & ANSI formatting
│   │   └── dashboard.rs
│   └── utils/                      # Process procfs inspection, timezone, paths
│       ├── mod.rs
│       └── proc.rs
└── tests/
    ├── common/
    ├── test_supervisor.rs
    ├── test_netlink_debounce.rs
    ├── test_native_api.rs
    └── test_config_reload.rs
```

---

## 7. Android Compatibility & Build Configuration

### 7.1. Cargo Profile Optimization (`Cargo.toml`)
```toml
[package]
name = "atpd"
version = "1.0.0"
edition = "2021"

[profile.release]
opt-level = "z"             # Optimize aggressively for binary size
lto = "fat"                 # Full link-time optimization across all crates
codegen-units = 1           # Maximize dead-code elimination & inlining
panic = "abort"             # Remove stack unwinding tables
strip = "symbols"           # Strip all debug symbols and names
overflow-checks = true      # Keep arithmetic safety active
```

### 7.2. 16 KB Page Size Alignment (Android 15+ & GKI 2025/2026)
Modern Android devices require 16 KB ELF segment alignment:
```toml
# .cargo/config.toml
[target.aarch64-linux-android]
rustflags = ["-C", "link-arg=-Wl,-z,max-page-size=16384"]
```

---

## 8. Staged Migration Plan (Dual-Track Parity)

To eliminate deployment risk, the Rust migration will execute in **4 sequential phases**, retaining the validated C11 test suite as the oracle. Shell harnesses (`tests/*.sh`) run against either binary; C unit tests (`tests/*.c`) link the C sources and serve as behavioural references that must be ported to Rust tests:

### Phase 1: Foundation & Pure Modules (Week 1)
- [ ] Establish Cargo workspace and cross-compilation pipeline for `aarch64-linux-android` (production target: Pixel, NDK/bionic).
- [ ] Implement `config` module: parse and validate `atp.conf` and `config.json`.
- [ ] Verify test parity against existing `test_config_validation.sh` and `test_vpn_bypass_config`.

### Phase 2: Core Loop & Supervisor (Week 2)
- [ ] Implement single-threaded Reactor and timer wheel using `mio` / `rustix`.
- [ ] Implement `supervisor`: process spawning, `pre_exec` credentials, PID identity checking, and exponential backoff.
- [ ] Verify test parity against `test_singbox_lifecycle.sh` and `test_service_crash_recovery.sh`.

### Phase 3: Telemetry, API & Sensing (Week 3)
- [ ] Implement Netlink/XFRM multi-VPN sensing with debounced timers.
- [ ] Implement `nativeapi`: gRPC-Web status streaming and `SetClashMode` synchronization.
- [ ] Implement UDS command socket and terminal dashboard rendering.
- [ ] Verify test parity against `test_api_vpn_mode` and `test_netlink`.

### Phase 4: Production Benchmarks & Gate Validation (Week 4)
- [ ] Execute `tests/benchmark_atpd.sh` to confirm RSS memory is **< 4.0 MB** and CLI latency is **< 8.0 ms**.
- [ ] Run 30x Netlink flap stress tests (`test_netlink`) to verify zero FD leaks and sub-500ms settlement.
- [ ] Full regression check across Magisk / KernelSU root environments on real Android devices.

---

## 9. Review Checklist for Reviewers (Claude / Maintainers)

When reviewing PRs or milestones for this rewrite, verify that the submission complies with the following checks:

1. **Dual-Process Integrity**: Does ATPD manage sing-box purely as an external child process? (Reject any in-process embedding).
2. **Zero `sys_bpf`**: Does the Rust codebase contain zero eBPF program loading or map definitions? (Reject eBPF duplication).
3. **No Hidden Multi-Thread Contention**: Is the core reactor free from unnecessary thread pools or mutex deadlocks around Netlink/Reactor dispatch?
4. **Binary Size & Memory Gate**: Does the compiled arm64 binary stay under **2.5 MB**, and does idle RSS stay under **4.0 MB**?
5. **Robust Error Handling**: Does the codebase use typed error enums without any `unwrap()` or `expect()` in production daemon paths?
6. **Protobuf Wire Fidelity**: Is `SetClashMode` encoding verified by a byte-level test to use field 3 (`Tag 0x1A`), matching sing-box `started_service.proto`?
