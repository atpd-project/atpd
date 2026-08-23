# ATP -- Advanced Transparent Proxy (Pure eBPF Edition)

Ultra-lightweight, zero-firewall transparent proxy daemon for modern Android devices (GKI 5.10+, Pixel, Android 13+) powered by **Native Kernel eBPF Interception**, multi-VPN tunnel sensing (Cloudflare WARP `tun0`, WireGuard, Tailscale, Google VPN `ipsec0`), asynchronous Reactor event loop, and Clash REST API integration.

---

[![Build Status](https://github.com/atpd-project/atpd/actions/workflows/build.yml/badge.svg)](https://github.com/atpd-project/atpd/actions)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Platform: Android](https://img.shields.io/badge/Platform-Android-green.svg)](https://www.android.com/)

---

## ⚡ Key Highlights (Pure eBPF Edition)

| Feature | Description |
|---|---|
| **Zero Firewall Rules (0 iptables)** | 100% free of `iptables`, `ip6tables`, `ipset`, and policy routing table 2025. Zero Netfilter overhead and zero `netd` conflict. |
| **In-Kernel Socket Interception** | Packets intercepted in-kernel via sing-box eBPF inbound (`cgroup/connect4`, `cgroup/connect6`, `cgroup/sendmsg4`, `cgroup/recvmsg4`, TC `sched_cls`). |
| **Multi-VPN Tunnel Sensing** | Real-time Netlink event sensing for secondary VPN tunnels: **Cloudflare WARP (`tun0` / `warp0`)**, **WireGuard (`wg0`)**, **Tailscale (`tailscale0`)**, and **Google VPN (`ipsec0` / `xfrm0`)**. |
| **Ultra-Lightweight Footprint** | Binary size stripped down to **158 KB**; runtime RSS memory baseline **~1.5 MB**; 0 background polling threads. |
| **Sing-box Core Lifecycle** | Non-blocking async process supervisor with exponential backoff, circuit breaker, and health check. |
| **Asynchronous Clash API** | Native REST client for proxy node delay inspection, group mode switching, and traffic metrics. |
| **Persistent Fast Logger** | Non-blocking line-buffered file stream with auto-rotation (10MB) and thread-safe timestamps. |

---

## 🏗️ Pure eBPF Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│                      ATPD (Pure eBPF)                       │
├─────────────────────────────────────────────────────────────┤
│  1. Core Supervisor                                         │
│     • Single-threaded Epoll Reactor event loop              │
│     • sing-box child process lifecycle & circuit breaker    │
│                                                             │
│  2. Network & Tunnel Sensing                                │
│     • Netlink XFRM SA & Route event listener                │
│     • WARP (tun0) / WireGuard / Tailscale detection         │
│     • Real-time RX/TX speed and bandwidth metrics           │
│                                                             │
│  3. Control & Observability                                 │
│     • Fast Unix Domain Socket interface (status/reload/stop)│
│     • Asynchronous Clash REST API client                    │
│     • Non-invasive kernel eBPF capability prober            │
└─────────────────────────────────────────────────────────────┘
                               ▲
                               │ Lifecycle & Supervision
                               ▼
┌─────────────────────────────────────────────────────────────┐
│                sing-box (eBPF-In Inbound)                   │
├─────────────────────────────────────────────────────────────┤
│  • cgroup.bpf.c kernel socket capture                       │
│  • include_package / exclude_package UID policy             │
│  • Direct in-kernel China IP bypass via rule-sets           │
└─────────────────────────────────────────────────────────────┘
```

---

## 📱 Supported Devices & Requirements

- **Target OS**: Android 12+, 13, 14, 15, 16+
- **Kernel Baseline**: Android GKI 5.10+, 5.15+, 6.1+, 6.6+, 6.12+ (Google Pixel, modern Xiaomi, OnePlus, Samsung, etc.)
- **Root Environment**: KernelSU, APatch, or Magisk
- **Kernel Features**: `cgroup_sock_addr`, `sched_cls`, `lpm_trie`, `hash`, `array`, `lru_hash`

---

## 📁 Workspace Directory Structure (Unified & Self-Adaptive)

ATPd automatically derives its root working directory from its own binary path (`/proc/self/exe`), allowing arbitrary installation paths (`/data/adb/atp`, `/data/adb/sing-box`, etc.) with zero hardcoding:

```text
/data/adb/atp/ (or /data/adb/sing-box/)
├── atpd                 ── ATP Daemon (C11 + Pure eBPF Reactor)
├── atp.conf             ── ATP Framework Configuration
├── config.json          ── sing-box Core Configuration
├── cache.db             ── sing-box cache & rule-sets (auto-stored here via -D .)
├── bin/
│   └── sing-box         ── Proxy Core Binary (or at root ./sing-box)
└── run/                 ── Isolated Runtime Directory (auto-created)
    ├── atpd.pid         ── ATPd Daemon PID
    ├── atpd.sock        ── UDS Local Control Socket (0600)
    ├── atp.log          ── ATPd System Log
    ├── sing-box.pid     ── sing-box Process PID
    ├── sing-box.log     ── sing-box Process Log
    └── traffic.state    ── Persistent Traffic Statistics
```

---

## 🚀 Quick Start

### 1. Installation

Deploy `atpd`, `atp.conf`, and `sing-box` directly to your root directory:

```bash
mkdir -p /data/adb/atp/bin /data/adb/atp/run
cp build/bin/atpd /data/adb/atp/
cp examples/atp.conf.example /data/adb/atp/atp.conf
cp /path/to/sing-box /data/adb/atp/bin/sing-box
chmod 755 /data/adb/atp/atpd /data/adb/atp/bin/sing-box
```

### 2. Basic Commands

```bash
# Start daemon and proxy core (auto-daemonize)
/data/adb/atp/atpd start

# Inspect live status, memory, CPU, threads, and VPN sensing
/data/adb/atp/atpd status

# Restart daemon and proxy core
/data/adb/atp/atpd restart

# Hot-reload configuration (SIGHUP)
/data/adb/atp/atpd reload

# Stop daemon and proxy core cleanly
/data/adb/atp/atpd stop

# Probe kernel eBPF capabilities
/data/adb/atp/atpd ebpf probe
```

### 3. Boot Service Setup (`service.d`)

Create `/data/adb/service.d/atpd_service.sh` for automatic boot execution under Magisk, KernelSU, or APatch:

```bash
mkdir -p /data/adb/service.d
cat << 'EOF' > /data/adb/service.d/atpd_service.sh
#!/system/bin/sh
until [ "$(getprop sys.boot_completed)" = "1" ]; do
    sleep 3
done
sleep 2

for candidate in /data/adb/atp/atpd /data/adb/sing-box/atpd; do
    if [ -f "${candidate}" ]; then
        chmod +x "${candidate}"
        "${candidate}" start
        break
    fi
done
exit 0
EOF
chmod 755 /data/adb/service.d/atpd_service.sh
```

---

## 📊 Status Dashboard (`atpd status`)

```text
======================================================================
                     ATP Status (Pure eBPF Edition)
======================================================================
┌────────────────────── PROXY CORE ──────────────────────────┐
│ 🟢 Status          │ sing-box                              │
│ ├─ PID             │ 12480                                 │
│ ├─ Uptime          │ 2h 15m 32s                            │
│ ├─ Memory          │ 24.50 MB                              │
│ ├─ CPU             │ 0.2%                                  │
│ ├─ Threads         │ 14                                    │
│ ├─ FDs             │ 32                                    │
│ └─ Version         │ sing-box 1.14.0-beta.8                │
└────────────────────────────────────────────────────────────┘

┌──────────────────── PURE eBPF ENGINE ──────────────────────┐
│ ├─ Engine Mode     │ Pure eBPF (Zero iptables)             │
│ ├─ Data Path       │ sing-box ebpf inbound                 │
│ ├─ eBPF Kernel     │ AVAILABLE                             │
│ └─ Capabilities    │ cgroup_sock tc lpm_trie lru_hash      │
└────────────────────────────────────────────────────────────┘

┌────────────────── VPN TUNNEL STATUS ───────────────────────┐
│ 🟢 Status          │ CONNECTED                             │
│ ├─ Interface       │ tun0 (Cloudflare WARP / TUN)          │
│ ├─ 📥 Total RX     │ 142.50 MB                             │
│ ├─ 📤 Total TX     │ 38.20 MB                              │
│ ├─ 📈 Avg RX Speed │ 2.45 MB/s                             │
│ └─ 📉 Avg TX Speed │ 480.20 KB/s                           │
└────────────────────────────────────────────────────────────┘

┌─────────────────── REACTOR ENGINE (v2.0) ──────────────────┐
│ State Machine      │ ⚡  READY                              │
│ └─ XFRM Sync       │ LOCKED                                │
└────────────────────────────────────────────────────────────┘
```

---

## ⚙️ Configuration (`atp.conf`)

```ini
# Directory Customization (Optional, auto-detected by default)
# DATA_DIR="/data/adb/atp"
# RUN_DIR="run"

# Performance Mode (TCP BBR & Kernel Scheduler)
PERFORMANCE_MODE=1

# Log timestamping
LOG_TIMESTAMP=1

# Process user and group (Android: root:net_admin, Linux: root:root)
CORE_USER_GROUP="root:net_admin"

# Clash REST API connection (Auto-synced from config.json if available)
API_HOST="127.0.0.1"
API_PORT=9090
CLASH_SECRET=""

# Service supervisor parameters
SERVICE_START_TIMEOUT=30
SERVICE_STOP_TIMEOUT=10
SERVICE_MAX_FAILURES=5
SERVICE_CIRCUIT_COOLDOWN=60
```

---

## 🛠️ Building from Source

```bash
# Clean & Build with Clang (LTO & Strip)
make clean && make

# Run automated CI lifecycle tests (requires sing-box binary)
sudo ./tests/test_singbox_lifecycle.sh
```

---

## 📜 License

GPL-3.0 License.

## 🙏 Acknowledgments

- **[sing-box]** by [SagerNet](https://github.com/SagerNet) — The universal proxy core providing native eBPF inbound and Clash API integration.
- **[AndroidTProxyShell]** by [CHIZI-0618](https://github.com/CHIZI-0618) — The inspiration for transparent proxying on Android.
- **[atp4pixel]** by [yapixel](https://github.com/yapixel/atp4pixel) — The pure eBPF reference architecture for Android GKI kernels.
