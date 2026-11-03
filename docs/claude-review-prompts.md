# Claude Review & Verification Prompts

This document contains a standardized prompt for peer review and architecture auditing of the ATPD (Advanced Transparent Proxy Daemon) Rust codebase by Claude or external systems-engineering reviewers.

---

## Comprehensive Architecture & Safety Audit Prompt

Use this prompt to have Claude perform an in-depth systems audit of the Rust codebase against Android constraints and systems programming invariants.

```markdown
You are a Principal Rust Systems Engineer and Linux Kernel/Android Expert. Please perform an in-depth code audit and peer review of the Rust implementation of ATPD (Advanced Transparent Proxy Daemon) on the `main` branch.

### Background & Architecture Invariants:
1. Target Platform: Android arm64 only (bionic, NDK r30, minimum API 24, every LOAD segment aligned to 16 KB; rooted via KernelSU/APatch/Magisk). Linux arm64 builds exist only for CI tests.
2. Zero Heavy Async: Must not use Tokio/Actix/Hyper. All I/O, signals, and timers are managed by a single-threaded epoll reactor (rustix for epoll/eventfd/clocks, libc for signalfd/sigprocmask). The only other thread is the Native API worker.
3. Dual-Process Model: ATPD acts strictly as a lightweight supervisor/control plane. The dataplane is exclusively owned by the child `sing-box` process running `ebpf-in`. ATPD must NEVER invoke `sys_bpf` / `libc::bpf`.
4. Footprint: the stripped bionic binary is about 0.6 MB. CI's benchmark gate requires baseline RSS <= 3072 kB, RSS growth <= 512 kB, no fd growth and no thread growth. Only 3 dependencies allowed: `rustix`, `libc`, `serde_json`.

### Review Objectives:
Please review the repository focusing on the following critical modules:
1. `src/reactor/`: epoll handling, signalfd integration, signal mask preservation across lifecycle, and monotonic timer precision.
2. `src/supervisor/`: child process spawning via `CommandExt::pre_exec`, non-allocating privilege dropping (`umask`, `setsid`, `setgroups`, `setresgid`, `setresuid`), defense against PID reuse via `/proc/<pid>/stat` starttime checks, circuit-breaker and cooldown retry logic, and sing-box log rotation.
3. `src/netlink/`: Netlink route/link/XFRM socket reading, zero-dependency binary parsing (`nlmsghdr`, `rtattr`), multi-VPN detection (`is_proxy_interface`), VPN bypass rules, and debounce timing.
4. `src/ipc/` & `src/status.rs`: UDS server (std `UnixListener`/`UnixStream`, rustix `SO_PEERCRED` UID verification), socket-probe collision prevention, idle-client pruning, and lock-free status copy-out.
5. `src/nativeapi/`: gRPC-Web HTTP/1.1 framing over raw TCP, signal isolation on the dedicated worker thread, and manual Protobuf varint / length-delimited codecs (`src/nativeapi/proto.rs`).

### Verified facts (do not re-report):
- `SetClashMode` encodes `ClashMode.mode` as protobuf field 3 (tag `0x1a`). Upstream sing-box `daemon/started_service.proto` defines `message ClashMode { string mode = 3; }`, and Pixel smoke tests confirm clash-mode switching works.
- Native API status fields read N/A for about the first second after start, until sing-box opens its API port.

Provide a structured, prioritized report:
- Critical Deficiencies / Bugs
- Architecture & Invariant Conformance
- Unsafe Code & Memory Safety Correctness
- Concrete, Minimal Action Items
```
