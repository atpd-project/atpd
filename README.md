# ATP -- Advanced Transparent Proxy

Ultra-lightweight, zero-firewall transparent proxy daemon for rooted Android devices (GKI 5.10+, Pixel). `atpd` supervises sing-box's **In-Kernel eBPF Socket Interception**, reads its **Native API**, answers CLI queries over **Fast-Path UDS IPC**, and senses **secondary VPN tunnels** (WARP `tun0`, WireGuard, Tailscale, Google VPN `ipsec0`) from a single-threaded Rust epoll reactor.

---

[![CI](https://github.com/atpd-project/atpd/actions/workflows/rust-ci.yml/badge.svg)](https://github.com/atpd-project/atpd/actions/workflows/rust-ci.yml)
[![Release](https://img.shields.io/github/v/release/atpd-project/atpd?include_prereleases)](https://github.com/atpd-project/atpd/releases)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Platform: Android arm64](https://img.shields.io/badge/Platform-Android%20arm64-green.svg)](https://www.android.com/)
[![Rust](https://img.shields.io/badge/Rust-stable-orange.svg)](https://www.rust-lang.org/)

---

## ⚡ Key Highlights

| Feature | Description |
|---|---|
| **Zero Firewall Rules (0 iptables)** | No `iptables`, `ip6tables`, `ipset`, or policy routing tables. Zero Netfilter overhead and zero `netd` conflict. |
| **In-Kernel eBPF Interception** | Traffic is captured in-kernel by sing-box's eBPF inbound (`ebpf-in`). sing-box owns the eBPF programs and maps; `atpd` never calls `bpf()`. |
| **Native API & Runtime Telemetry** | Uses sing-box `services: [{"type": "api"}]` for health checks and the `SubscribeStatus` gRPC-Web stream for memory, goroutines, connections and traffic. |
| **Fast-Path UDS IPC** | UNIX domain socket (`run/atpd.sock`, mode 0600, peer UID checked) for near-instant `atpd status`. |
| **Multi-VPN Tunnel Sensing** | Netlink route/XFRM events detect **Cloudflare WARP (`tun0` / `warp0`)**, **WireGuard (`wg0`)**, **Tailscale (`tailscale0`)** and **Google VPN (`ipsec0` / `xfrm0`)**, and switch the Clash mode while a VPN is up. |
| **Ultra-Lean Footprint** | ~0.6 MB stripped binary, ~2.4 MB RSS, 2 threads, 3 dependencies (`rustix`, `libc`, `serde_json`), no async runtime, no busy polling. |
| **Resilient Core Lifecycle** | sing-box supervisor with exponential backoff, circuit breaker with cooldown retry, health checks, PID-reuse protection and log rotation. |

---

## 🏗️ Architecture Overview

```text
┌─────────────────────────────────────────────────────────────┐
│                    ATPD (Control Plane)                     │
├─────────────────────────────────────────────────────────────┤
│  1. Core Supervisor & Reactor                               │
│     • Single-threaded epoll reactor (signalfd, timers)      │
│     • sing-box child lifecycle, backoff & circuit breaker   │
│                                                             │
│  2. Network & Tunnel Sensing                                │
│     • Netlink XFRM SA & route event listener                │
│     • WARP / WireGuard / Tailscale / IPsec detection        │
│                                                             │
│  3. Native API & Runtime Telemetry                          │
│     • Auto-detects services: [type: api] in config.json     │
│     • SubscribeStatus + Clash mode RPCs on a worker thread  │
│                                                             │
│  4. Fast-Path UDS IPC                                       │
│     • In-memory status snapshot served over run/atpd.sock   │
└─────────────────────────────────────────────────────────────┘
                               ▲
                               │ Lifecycle & Supervision / API
                               ▼
┌─────────────────────────────────────────────────────────────┐
│                sing-box (eBPF-In Inbound)                   │
├─────────────────────────────────────────────────────────────┤
│  • services: [{"type": "api", "listen_port": 9080}]         │
│  • cgroup socket capture, include/exclude_package policies  │
└─────────────────────────────────────────────────────────────┘
                               ▲
                               │ In-Kernel Socket Capture
                               ▼
┌─────────────────────────────────────────────────────────────┐
│                     Android Kernel (GKI)                    │
└─────────────────────────────────────────────────────────────┘
```

---

## 📱 Supported Devices & Requirements

- **Target**: Android arm64 (bionic, minimum API 24, 16 KB page aligned), e.g. Google Pixel
- **Kernel**: Android GKI 5.10+, 5.15+, 6.1+, 6.6+, 6.12+
- **Root**: KernelSU, APatch, or Magisk
- **Kernel eBPF features** (used by sing-box): `cgroup_sock_addr`, `sched_cls`, `lpm_trie`, `hash`, `array`, `lru_hash`

---

## 📁 Directory Layout

`atpd` derives its root from its own binary path (`/proc/self/exe`); the standard Android root is `/data/adb/atp`:

```text
/data/adb/atp/
├── atpd                 ── ATP daemon
├── atp.conf             ── ATP settings (optional)
├── bin/
│   └── sing-box         ── Proxy core binary
├── config.json          ── sing-box configuration (Native API)
├── sing-box.log         ── sing-box log (rotated at 10 MB, 3 copies kept)
└── run/                 ── Runtime directory (auto-created)
    ├── atpd.pid         ── atpd PID
    ├── atpd.sock        ── UDS command socket (0600)
    ├── atp.log          ── atpd log
    └── sing-box.pid     ── sing-box PID
```

---

## 🚀 Quick Start

### 1. Installation

Download `atpd-<version>-android-arm64.tar.gz` from [Releases](https://github.com/atpd-project/atpd/releases) and verify it against `sha256sums.txt`. Then, as root:

```bash
mkdir -p /data/adb/atp/bin /data/adb/atp/run
tar -xzf atpd-*-android-arm64.tar.gz -C /data/adb/atp ./atpd
cp /path/to/sing-box /data/adb/atp/bin/sing-box
cp /path/to/config.json /data/adb/atp/config.json
chmod 755 /data/adb/atp/atpd /data/adb/atp/bin/sing-box
```

### 2. CLI Commands

```bash
/data/adb/atp/atpd start     # Start daemon and sing-box (daemonizes)
/data/adb/atp/atpd status    # Show daemon, VPN tunnel, and traffic status
/data/adb/atp/atpd restart   # Restart daemon and sing-box
/data/adb/atp/atpd reload    # Reload configuration without restart (SIGHUP)
/data/adb/atp/atpd stop      # Stop daemon and sing-box
/data/adb/atp/atpd check     # Validate configuration and exit
```

Options: `-c FILE` config, `-p FILE` PID file, `-f` foreground, `-V` verbose, `-q` quiet, `-n` no color, `-v` version.

### 3. Boot Service

The boot script from the C version works unchanged with the Rust binary. Copy [`service.d/atpd_service.sh`](https://github.com/atpd-project/atpd/blob/archive/c-main/service.d/atpd_service.sh) to `/data/adb/service.d/` and `chmod 755` it.

---

## ⚙️ Configuration Reference

### `atp.conf` (optional)

```ini
# sing-box user and group
CORE_USER_GROUP="root:net_admin"

LOG_TIMESTAMP=1

# Native API connection (auto-detected from config.json if not set)
# API_HOST="127.0.0.1"
# API_PORT=9080
# API_SECRET=""

# Supervisor
SERVICE_START_TIMEOUT=30
SERVICE_STOP_TIMEOUT=10
SERVICE_MAX_FAILURES=5
SERVICE_CIRCUIT_THRESHOLD=5
SERVICE_CIRCUIT_COOLDOWN=60
SERVICE_HEALTH_CHECK_INTERVAL=5000

# Switch Clash mode while a VPN is up
VPN_AUTO_MODE=1
VPN_TARGET_MODE="Google VPN"
VPN_FALLBACK_MODE="Rule"
# VPN_BYPASS_ROUTES="100.64.0.0/10 fd7a:115c:a1e0::/48 192.168.88.0/24"
# VPN_BYPASS_PREF=12400
```

Older key names (`WORK_DIR`, `CLASH_SECRET`, `VPN_AUTO_CLASH_MODE`, `VPN_CLASH_MODE`, `VPN_DEFAULT_MODE`) are still accepted.

### `config.json` (sing-box)

```json
{
  "log": { "level": "info", "timestamp": true },
  "services": [
    { "type": "api", "tag": "api-service", "listen": "127.0.0.1", "listen_port": 9080 }
  ],
  "experimental": { "clash_api": {} },
  "inbounds": [
    {
      "type": "ebpf",
      "tag": "ebpf-in",
      "cgroup_enabled": true,
      "cgroup_path": "/sys/fs/cgroup",
      "network": ["tcp", "udp"]
    }
  ],
  "route": { "auto_detect_interface": true }
}
```

`services[].type: "api"` exposes the Native API that `atpd` uses for health checks, status and Clash mode control. The empty `experimental.clash_api` object enables sing-box's mode engine. On Android, `include_package` / `exclude_package` can be added to the eBPF inbound.

When a supported VPN interface (`ipsecN`, `tunN`, `wgN`, `warpN`, `tailscaleN`) comes up, `atpd` checks that `VPN_TARGET_MODE` is one of sing-box's modes, saves the current mode and switches to the target. When the VPN goes down, it restores the saved mode (or `VPN_FALLBACK_MODE`).

---

## 🛠️ Building & Testing

```bash
# Android release binary (needs the Android NDK linker)
export CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER=/path/to/ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang
cargo build --release --target aarch64-linux-android

# Host checks
cargo fmt --check
cargo clippy --all-targets -- -D warnings
cargo test
```

CI also runs the tests under ASan and TSan, and runs the C version's integration and benchmark harnesses (from [`archive/c-dev`](https://github.com/atpd-project/atpd/tree/archive/c-dev/tests)) against the Rust binary. Device testing: [`docs/agy-pixel-test.md`](docs/agy-pixel-test.md).

The C implementation is archived on the [`archive/c-main`](https://github.com/atpd-project/atpd/tree/archive/c-main) and [`archive/c-dev`](https://github.com/atpd-project/atpd/tree/archive/c-dev) branches.

---

## 📜 License

GPL-3.0 License.

## 🙏 Acknowledgments

- **[sing-box]** by [SagerNet](https://github.com/SagerNet) — The universal proxy core providing native eBPF inbound and Native API capabilities.
- **[AndroidTProxyShell]** by [CHIZI-0618](https://github.com/CHIZI-0618) — Transparent proxying patterns on Android.
- **[atp4pixel]** by [yapixel](https://github.com/yapixel/atp4pixel) — Pure eBPF architecture reference for Android GKI kernels.
