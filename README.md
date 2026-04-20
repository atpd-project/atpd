# ATP -- Advanced Transparent Proxy

High-performance transparent proxy daemon for Android with TPROXY/REDIRECT support, VPN mode switching, and automatic self-healing.

---

[![Build Status](https://github.com/atpd-project/atpd/actions/workflows/build.yml/badge.svg)](https://github.com/atpd-project/atpd/actions)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Platform: Android](https://img.shields.io/badge/Platform-Android-green.svg)](https://www.android.com/)

---

## Features

| Feature | Description |
|--------|-------------|
| **Dual mode** | TPROXY and REDIRECT with auto-detection |
| **IPv4/IPv6** | Independent control for both stacks |
| **DNS hijacking** | TPROXY or REDIRECT based DNS interception |
| **China IP bypass** | GeoIP-based ipset with atomic updates |
| **Per-app proxy** | Blacklist/whitelist by Android UID |
| **MAC filtering** | Per-device proxy control for hotspots |
| **VPN mode** | Auto-detection and switching for Google VPN(`ipsec`) |
| **Self-healing** | Detects and repairs rule drift from netd |
| **Service monitor** | Auto-restart of sing-box with cooldown |
| **Clash API** | Mode synchronization with sing-box/Clash |
| **Performance** | conntrack optimization, BBR TCP stack tuning |

## Requirements

- Android 8.0+ (API 27+)
- Root access (Magisk, KernelSU, or APatch)
- Kernel with `TPROXY`, `IPSET`, and `CONNTRACK` support

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
cp atp.conf.example /data/adb/atp/atp.conf

# Start daemon
/data/adb/atp/bin/atpd start

# Check status
/data/adb/atp/bin/atpd status

# Stop daemon
/data/adb/atp/bin/atpd stop
```

## Configuration

@TP looks for `atp.conf` in the **same directory as the `atpd` binary** by default. Use `-c` to specify a custom path.

Example `atp.conf`:

```ini
# Proxy ports
PROXY_TCP_PORT=1536
PROXY_UDP_PORT=1536

# Mode: 0=auto  1=tproxy  2=redirect  3=enhance
PROXY_MODE=3

# IPv6 support
PROXY_IPV6=0

# DNS hijacking
DNS_HIJACK_ENABLE=1
DNS_PORT=1053

# Routing marks
MARK_VALUE=20
TABLE_ID=150

# API
API_HOST=127.0.0.1
API_PORT=9090
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
|``status` | Show runtime status |
| `reload` | Reload configuration |
| `check` | Check configuration syntax and validity |
| `update-geoip` | Update GeoIP database |

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
atpd status                       # Show status
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
├── rules/
│   ├── cn.zone             # China IPv4 CIDR│   └── cn_ipv6.zone        # China IPV6 CIDR
├── sing-box/
│   └── config.json         # sing-box configuration
└── atp.conf                # Main configuration
```

## Troubleshooting

**Check if running**

```bash
atpd status
ps -A | grep atpd
```

**View logs**

```bash
cat /data/adb/atp/run/atp.log
tail -f /data/adb/atp/run/atp.log
```

**Inspect iptables rules**

```bash
iptables -t mangle -L | grep ATP
ip6tables -t mangle -L | grep ATP
```

**Force GeoIP update**

```bash
atpd update-geoip
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

- [**AndroidTProxyShell**] by [CHIZI-0618](https://github.com/CHIZI-0618) -- The original shell script that inspired this project's architecture, TPROXY implementation, and comprehensive feature set.
- ***sing-box**] by [SagerNet](https://github.com/SagerNet) -- The powerful universal proxy core that powers ATP's underlying traffic handling and Clash API integration.
- ***DeepSeek**](https://www.deepseek.com/) -- AI-driven development assistance that accelerated the creation of this project's C language implementation.

Their excellent work made ATP possible.
