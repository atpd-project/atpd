# ATP — Advanced Transparent Proxy

High-performance transparent proxy daemon for Android with TPROXY/REDIRECT support, VPN mode switching, and automatic self-healing.

---

[![Build Status](https://github.com/atpd-project/atpd/actions/workflows/build.yml/badge.svg)](https://github.com/atpd-project/atpd/actions)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Platform: Android](https://img.shields.io/badge/Platform-Android-green.svg)](https://www.android.com/)

---

## Known Good Commits

| Commit | Date | Status | Description |
|--------|------|--------|-------------|
| `最新` | 2026-04-19 | ✅ | CLI UX improvements |
| `de11688` | 2026-04-19 | ✅ | Stable base version |

---

## Features

| Feature | Description |
|---|---|
| **Dual mode** | TPROXY and REDIRECT with auto-detection |
| **IPv4/IPv6** | Independent control for both stacks |
| **DNS hijacking** | TPROXY or REDIRECT based DNS interception |
| **China IP bypass** | GeoIP-based ipset with atomic updates |
| **Per-app proxy** | Blacklist/whitelist by Android UID |
| **MAC filtering** | Per-device proxy control for hotspots |
| **VPN mode** | Auto-detection and switching for Google VPN (`ipsec*`) |
| **Self-healing** | Detects and repairs rule drift from netd |
| **Service monitor** | Auto-restart of sing-box with cooldown |
| **Clash API** | Mode synchronization with sing-box/Clash |
| **Performance** | conntrack optimization, BBR TCP stack tuning |

---

## Requirements

- Android 8.0+ (API 27+)
- Root access (Magisk or KernelSU)
- Kernel with `TPROXY`, `IPSET`, and `CONNTRACK` support

---

## Installation

### Build from source

```bash
make
```

### Deploy to device

```bash
# Via Makefile
make install-android

# Or manually
adb push build/bin/atpd /data/adb/atp/bin/
adb shell chmod 755 /data/adb/atp/bin/atpd
```

### Using the install script

```bash
adb push scripts/install.sh /data/local/tmp/
adb shell su -c "sh /data/local/tmp/install.sh"
```

---

## Configuration

Edit `/data/adb/atp/atp.conf`:

```ini
# Proxy ports
PROXY_TCP_PORT=1536
PROXY_UDP_PORT=1536

# Mode: 0=auto  1=tproxy  2=redirect  3=enhance
PROXY_MODE=0

# IPv6 support
PROXY_IPV6=0

# DNS hijacking
DNS_HIJACK_ENABLE=1
DNS_PORT=1053

# Routing marks
MARK_VALUE=20
TABLE_ID=2025
```

---

## Usage

### Commands

```bash
atpd start                        # Start daemon
atpd stop                         # Stop daemon
atpd restart                      # Restart daemon
atpd status                       # Check status
atpd reload                       # Reload configuration
atpd update-geoip                 # Update GeoIP database
atpd start --dry-run              # Simulate without applying changes
atpd start --foreground --verbose # Run in foreground with verbose logging
```

### Options

| Option | Description |
|---|---|
| `-d, --config-dir DIR` | Set configuration directory |
| `-n, --dry-run` | Simulate operations without changes |
| `-v, --verbose` | Enable verbose logging |
| `-q, --quiet` | Suppress non-error output |
| `-f, --foreground` | Run in foreground (don't daemonize) |
| `-y, --syslog` | Log to syslog |
| `-o, --log-file FILE` | Write logs to FILE |
| `-h, --help` | Show help message |
| `-V, --version` | Show version |

---

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
│   ├── cn.zone             # China IPv4 CIDRs
│   └── cn_ipv6.zone        # China IPv6 CIDRs
├── sing-box/
│   └── config.json         # sing-box configuration
└── atp.conf                # Main configuration
```

---

## Architecture

```
atpd (daemon)
├── epoll event loop
├── netlink interface monitor
├── iptables/ip6tables manager
├── ipset manager (GeoIP)
├── routing policy manager
├── service monitor (sing-box)
├── Clash API client
└── command queue (serialized execution)
```

---

## How It Works

### Standard mode

```
Traffic → PREROUTING → ATP_PRE_0 → TPROXY → sing-box
```

### VPN mode (`ipsec*` detected)

```
Traffic → PREROUTING → XFRM_BYPASS → ATP_PRE_0 → TPROXY → sing-box
                ↓
         ESP / UDP 4500 / UDP 500 bypassed
```

### Atomic rule updates

Rule changes are applied with zero traffic interruption:

```
1. Build new ruleset in ATP_PRE_1
2. Atomic switch: PREROUTING → ATP_PRE_1
3. Flush and clear ATP_PRE_0
```

---

## Building for Android

### With NDK directly

```bash
export NDK=/path/to/android-ndk
export TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64
export CC=$TOOLCHAIN/bin/aarch64-linux-android21-clang

make CC=$CC
```

### Dependencies

- `libcurl` — GeoIP database downloads
- `pthread` — built into the NDK
- `cJSON` — bundled in source tree

---

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

**Inspect ipsets**
```bash
ipset list cnip
ipset list cnip6
```

**Force GeoIP update**
```bash
atpd update-geoip
```

---

## License

GPL v3

---

*ATP Project — v1.0.0*
