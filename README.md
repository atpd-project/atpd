# ATP (Advanced Transparent Proxy)

High-performance transparent proxy daemon for Android with TPROXY/REDIRECT support, VPN mode switching, and automatic self-healing.

## Features

- **Dual Mode**: TPROXY and REDIRECT modes with auto-detection
- **IPv4/IPv6 Support**: Independent control for both stacks
- **DNS Hijacking**: TPROXY or REDIRECT based DNS interception
- **China IP Bypass**: GeoIP-based ipset with atomic updates
- **Per-App Proxy**: Blacklist/whitelist by Android UID
- **MAC Filtering**: Per-device proxy control for hotspots
- **VPN Mode**: Automatic detection and switching for Google VPN (ipsec*)
- **Self-Healing**: Detects and repairs rule drift from netd
- **Service Monitoring**: Automatic restart of sing-box with cooldown
- **Clash API**: Mode synchronization with sing-box/Clash
- **Performance**: conntrack optimization, BBR TCP stack tuning

## Requirements

- Android 8.0+ (API 27+)
- Root access (Magisk or KernelSU)
- Kernel support: TPROXY, IPSET, CONNTRACK

## Installation

### From Source

```bash
# Build
make

# Install to Android device
make install-android

# Or manual installation
adb push build/bin/atpd /data/adb/atp/bin/
adb shell chmod 755 /data/adb/atp/bin/atpd

Using Install Script

# Push install script and run
adb push scripts/install.sh /data/local/tmp/
adb shell su -c "sh /data/local/tmp/install.sh"

Configuration

Edit /data/adb/atp/atp.conf:

# Proxy ports
PROXY_TCP_PORT=1536
PROXY_UDP_PORT=1536

# Mode: 0=auto, 1=tproxy, 2=redirect, 3=enhance
PROXY_MODE=0

# IPv6 support
PROXY_IPV6=0

# DNS hijacking
DNS_HIJACK_ENABLE=1
DNS_PORT=1053

# Routing marks
MARK_VALUE=20
TABLE_ID=2025

Usage
Commands
# Start daemon
atpd start

# Stop daemon
atpd stop

# Restart daemon
atpd restart

# Check status
atpd status

# Update GeoIP database
atpd update-geoip

# Reload configuration
atpd reload

# Dry run (simulate without changes)
atpd start --dry-run

# Run in foreground (with verbose logging)
atpd start --foreground --verbose

Options
Option	Description
-d, --config-dir DIR	Set configuration directory
-n, --dry-run	Simulate operations (no changes)
-v, --verbose	Enable verbose logging
-q, --quiet	Suppress non-error output
-f, --foreground	Run in foreground (don't daemonize)
-y, --syslog	Log to syslog
-o, --log-file FILE	Write logs to FILE
-h, --help	Show help message
-V, --version	Show version

Directory Structure

/data/adb/atp/
├── bin/
│   └── atpd                 # Main daemon
├── run/
│   ├── atp.log              # ATP log file
│   ├── atpd.pid             # Daemon PID file
│   ├── sing-box.log         # sing-box log
│   └── sing-box.pid         # sing-box PID file
├── rules/
│   ├── cn.zone              # China IPv4 CIDRs
│   └── cn_ipv6.zone         # China IPv6 CIDRs
├── sing-box/
│   └── config.json          # sing-box configuration
└── atp.conf                 # Main configuration

Architecture

atpd (daemon)
├── epoll event loop
├── netlink interface monitor
├── iptables/ip6tables manager
├── ipset manager (GeoIP)
├── routing policy manager
├── service monitor (sing-box)
├── Clash API client
└── command queue (serialized execution)

How It Works

Standard Mode (No VPN)

Traffic → PREROUTING → ATP_PRE_0 → TPROXY → sing-box

VPN Mode (ipsec+ detected)
Traffic → PREROUTING → XFRM_BYPASS → ATP_PRE_0 → TPROXY → sing-box
                ↓
         ESP/UDP 4500/500 bypassed

Atomic Rule Updates
Current: PREROUTING → ATP_PRE_0

Update:
1. Build rules in ATP_PRE_1
2. Atomic switch: PREROUTING → ATP_PRE_1
3. Clear ATP_PRE_0

Building for Android

Using NDK

export NDK=/path/to/android-ndk
export TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64
export CC=$TOOLCHAIN/bin/aarch64-linux-android21-clang

make CC=$CC

Dependencies
libcurl (for GeoIP downloads)

pthread (built-in)

cJSON (included in source)

Troubleshooting
Check if running

atpd status
ps -A | grep atpd

View logs
cat /data/adb/atp/run/atp.log
tail -f /data/adb/atp/run/atp.log


Check iptables rules


iptables -t mangle -L | grep ATP
ip6tables -t mangle -L | grep ATP


Check ipset

ipset list cnip
ipset list cnip6

Force GeoIP update
atpd update-geoip

License
GPL v3

Author
ATP Project


Version
1.0.0