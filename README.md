# ATP -- Advanced Transparent Proxy

High-performance Android daemon for a sing-box eBPF inbound, VPN-aware mode switching, and automatic policy repair.

---

[![Build Status](https://github.com/atpd-project/atpd/actions/workflows/build.yml/badge.svg)](https://github.com/atpd-project/atpd/actions)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Platform: Android](https://img.shields.io/badge/Platform-Android-green.svg)](https://www.android.com/)

---

## Features

| Feature | Description |
|--------|-------------|
| **eBPF backend** | Supervises a sing-box native eBPF inbound |
| **Google VPN** | Detects XFRM/ipsec handovers and selects `Google VPN` mode |
| **Other VPNs** | Reports TUN/WireGuard VPNs such as Cloudflare WARP separately |
| **Direct Wi-Fi** | Selects `Direct` on a configured SSID and restores the prior policy |
| **Hotspot routing** | Binds tethered clients to Google VPN with IPv4/IPv6 policy rules |
| **Self-healing** | Detects and repairs Android netd policy-rule drift |
| **Service monitor** | Auto-restart of sing-box with cooldown |
| **Clash API** | Mode synchronization with sing-box/Clash |
| **Runtime status** | Reports monitors, state machines, traffic, temperature, and core health |

## Requirements

- Android 8.0+ (API 27+)
- Root access (Magisk, KernelSU, or APatch)
- A sing-box build and kernel that support the configured eBPF inbound

## Installation

### Download pre-built binary

Visit [GitHub Releases](https://github.com/atpd-project/atpd/releases) to download the latest `atpd` binary for your architecture.

### Install via adb

```bash
adb push atpd /data/adb/atp/bin/
adb shell chmod 755 /data/adb/atp/bin/atpd
```

### Or build from source

```bash
git clone https://github.com/atpd-project/atpd.git
cd atpd
make
```

## Quick Start

```bash
# Create default config (optional)
mkdir -p /data/adb/atp
cp examples/atp.conf.example /data/adb/atp/atp.conf

# Start daemon
/data/adb/atp/bin/atpd start

# Check status
/data/adb/atp/bin/atpd status

# Query or control the sing-box core
/data/adb/atp/bin/atpd core status
/data/adb/atp/bin/atpd core restart

# Stop daemon
/data/adb/atp/bin/atpd stop
```

## Configuration

ATPd reads `/data/adb/atp/atp.conf` by default. Use `-c` to specify a custom path.

Example `atp.conf`:

```ini
ATP_DATA=/data/adb/atp
NETWORK_BACKEND=ebpf
CORE_USER_GROUP=root:net_admin
API_HOST=127.0.0.1
API_PORT=9090
USER_CLASH_MODE=Rule
DIRECT_WIFI_SSID="Home Wi-Fi"
```

## Usage

```bash
atpd [options] command
```

### Commands

| Command | Description |
|--------|-------------|
| `start` | Start daemon |
| `stop` | Stop daemon |
| `restart` | Restart daemon |
| `status` | Show ATPd, network policy, and state-machine status |
| `core status` | Show sing-box process and API status |
| `core start` | Start sing-box without restarting ATPd |
| `core stop` | Stop sing-box without stopping ATPd |
| `core restart` | Restart sing-box without restarting ATPd |
| `reload` | Reload configuration |
| `check` | Check configuration syntax and validity |

### Options

| Option | Description |
|----------|-------------|
| `-c, --config FILE` | Specify configuration file |
| `-t` | Test configuration and exit (same as `check`) |
| `-f, --foreground` | Run in foreground |
| `-v, --verbose` | Verbose output |
| `-q, --quiet` | Quiet output |
| `--force` | Skip confirmation for dangerous operations |
| `--no-color` | Disable colored output |
| `-h, --help` | Show help |
| `-V, --version` | Show version |

### Examples

```bash
atpd status                       # Show daemon and network policy status
atpd core status                  # Show sing-box core status
atpd core start                   # Start sing-box without restarting ATPd
atpd core stop                    # Stop sing-box without stopping ATPd
atpd core restart                 # Restart sing-box without restarting ATPd
atpd -c atp.conf start            # Start with custom config
atpd -f -v start                  # Start in foreground with verbose log
atpd -t                           # Test configuration
atpd stop --force                 # Stop without confirmation
```

## Versioning

- **Stable releases**: Tagged commits (e.g., `v1.0.0`).
- **Development builds**: Format `dev-<short-commit>` (e.g., `dev-abc1234`).

For a list of verified working commits, see [Issue #1](https://github.com/atpd-project/atpd/issues/1).

## Directory Structure

```
/data/adb/atp/
├── bin/
│   └── atpd                # Main daemon
├── run/
│   ├── atp.log             # ATP log file
│   ├── atpd.pid            # Daemon PID file
│   ├── sing-box.log        # sing-box log
│   └── sing-box.pid        # sing-box PID file
├── sing-box/
│   └── config.json         # sing-box configuration
└── atp.conf                # Main configuration
```

## Troubleshooting

**Check if running**

```bash
atpd status
atpd core status
ps -A | grep atpd
```

**View logs**

```bash
cat /data/adb/atp/run/atp.log
tail -f /data/adb/atp/run/atp.log
```

**Inspect managed hotspot policy rules**

```bash
ip rule show pref 100
ip -6 rule show pref 100
```

## Development

### Environment Setup

A consistent environment across PC and Android is recommended:

- **PC**: WSL2 (Debian) with Git, GitHub CLI, and GPG signing.
- **Android**: Termux with Git, GitHub CLI, and GPG.

### Common Commands

| Task | Command |
|-------|--------|
| Pull latest | `git pull origin dev` |
| Commit | `git add . && git ci -m "message"` |
| Push | `git push origin dev` |
| Clean workspace | `git reset --hard HEAD && git clean -fd` |

## License

GPL v3

## 🙏 Acknowledgments

ATP is built upon the shoulders of giants. Special thanks to:

- **[AndroidTProxyShell]** by [CHIZI-0618](https://github.com/CHIZI-0618) — The original shell script that inspired this project's architecture, TPROXY implementation, and comprehensive feature set.
- **[sing-box]** by [SagerNet](https://github.com/SagerNet) — The powerful universal proxy core that powers ATP's underlying traffic handling and Clash API integration.
- **[DeepSeek](https://www.deepseek.com/)** — AI-driven development assistance that accelerated the creation of this project's C language implementation.

Their excellent work made ATP possible.

---
🚀 **Project:** ATP -- Advanced Transparent Proxy
🛡️ **Status:** Verified Commit Flow Enabled
👤 **Identity:** DeepSeek (Author) / debiansid (Committer)
