# Claude Review & Verification Prompts

This document contains standardized prompts designed for peer review, architecture auditing, and bug fixing by Claude or external systems-engineering reviewers for the ATPD (Advanced Transparent Proxy Daemon) Rust codebase (`v1.0.1-alpha.1`).

---

## 1. Comprehensive Architecture & Safety Audit Prompt

Use this prompt to have Claude perform an in-depth systems audit of the Rust codebase against Android 15+ constraints and systems programming invariants.

```markdown
You are a Principal Rust Systems Engineer and Linux Kernel/Android Expert. Please perform an in-depth code audit and peer review on the newly merged Rust implementation of the ATPD (Advanced Transparent Proxy Daemon) repository (tag: v1.0.1-alpha.1).

### Background & Architecture Invariants:
1. Target Platforms: Linux (x86_64/arm64) and Android 12-16+ (GKI 5.10+, KernelSU/APatch/Magisk, 16KB page size aligned).
2. Zero Heavy Async: Must not use Tokio/Actix/Hyper. All I/O, signals, and timers are managed via a single-threaded epoll reactor using rustix and libc.
3. Dual-Process Model: ATPD acts strictly as a lightweight supervisor/control plane. The dataplane is exclusively owned by the child `sing-box` process running `ebpf-in`. ATPD must NEVER invoke `sys_bpf` / `libc::bpf`.
4. Footprint: Stripped binary < 2.5 MB, idle RSS < 4.0 MB. Only 3 dependencies allowed: `rustix`, `libc`, `serde_json`.

### Review Objectives:
Please review the repository focusing on the following critical modules:
1. `src/reactor/`: epoll handling, signalfd integration, signal mask preservation across lifecycle, and monotonic timer precision.
2. `src/supervisor/`: child process spawning via `CommandExt::pre_exec`, non-allocating privilege dropping (`umask`, `setsid`, `setgroups`, `setresgid`, `setresuid`), defense against PID reuse via `/proc/<pid>/stat` starttime checks, and circuit-breaker logic.
3. `src/netlink/`: Netlink route/link/XFRM socket reading, zero-dependency binary parsing (`nlmsghdr`, `rtattr`), multi-VPN detection (`is_proxy_interface`), and debounce timing.
4. `src/ipc/` & `src/status.rs`: UDS server, `SO_PEERCRED` UID verification, socket-probe collision prevention, and lock-free status copy-out.
5. `src/nativeapi/`: gRPC-Web HTTP/1.1 framing over raw TCP, signal isolation on the dedicated worker thread, and manual Protobuf varint / length-delimited codecs (`src/nativeapi/proto.rs`).

### Specific Check:
Examine `encode_set_clash_mode` in `src/nativeapi/proto.rs` against upstream sing-box `daemon.StartedService/SetClashMode` protobuf definitions (`message ClashMode { string mode = 1; }`). Verify whether encoding only Field 3 (`0x1a`) causes mode-switch failures on standard sing-box, and evaluate our proposed fix of emitting both Field 1 (`0x0a`) and Field 3 (`0x1a`) for dual-compatibility.

Provide a structured, prioritized report:
- Critical Deficiencies / Bugs
- Architecture & Invariant Conformance
- Unsafe Code & Memory Safety Correctness
- Concrete, Minimal Action Items
```

---

## 2. Targeted Bugfix & Unit Test Prompt (SetClashMode Protobuf Tag)

Use this prompt to guide Claude or an autonomous agent to implement the fix for the Protobuf field mismatch in `src/nativeapi/proto.rs` and update unit tests.

```markdown
Please fix a critical Protobuf tag encoding defect in the sing-box Native API client within `atpd`.

### Problem Statement:
In `src/nativeapi/proto.rs`, the function `encode_set_clash_mode` currently encodes only Protobuf Field 3 (`tag = 0x1A = (3 << 3) | 2`):
```rust
out.push(0x1a);
encode_varint(mode_bytes.len() as u64, &mut out);
out.extend_from_slice(mode_bytes);
```
However, standard upstream sing-box defines `ClashMode` as:
```protobuf
message ClashMode {
    string mode = 1;
}
```
Standard sing-box expects Field 1 (`tag = 0x0A = (1 << 3) | 2`). Because only Field 3 is sent, sing-box ignores it as an unknown field and sets `mode = ""`, failing to synchronize Clash mode when VPN starts or stops.

### Requirements:
1. Update `encode_set_clash_mode` in `src/nativeapi/proto.rs`:
   - Emit Field 1 (`0x0A`, length-delimited) containing `mode`.
   - In addition, emit Field 3 (`0x1A`, length-delimited) containing `mode` for backward compatibility with legacy forks. (Proto3 decoders safely ignore extra fields).
   - Update docstrings and comments accordingly.
2. Update unit test `test_set_clash_mode_request_body_exact_bytes` in `src/nativeapi/tests.rs`:
   - Update the expected byte array to verify both Field 1 (`[0x0a, len, ...mode...]`) and Field 3 (`[0x1a, len, ...mode...]`).
3. Maintain zero new dependencies and ensure all assertions pass.
```
