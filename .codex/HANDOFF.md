# ATPD Codex Handoff

Repository: `/home/ezhang/atpd`
Branch: `ebpf-native-api`
Product build under validation: `8de80df1a24482b402febcd13c415f842aafd26f`

Checkpoint (must remain unchanged while paused):

```text
last_completed_step=30
current_step=31
status=complete
```

## Valid Evidence Retained

- **2026-09-03 Real-Device Android Smoke (Pixel 7 Pro `29271FDH300EJK`)**:
  - Native API authoritative fields: PASS (live child state published: Goroutines 69, Version 1.14.0-rc.1-4895c512, Clash Mode Rule, FCM Push Sensing ACTIVE).
  - reload churn `10/10`: PASS (ATPD PID 5140 unchanged, authoritative status valid, 2080 session alive).
  - restart churn `10/10`: PASS (distinct new PIDs verified on each cycle, old PIDs reaped, authoritative status restored).
  - crash recovery `5/5`: PASS (stale status published as N/A on SIGKILL, child reaped and respawned under backoff, ATPD PID preserved, authoritative telemetry restored).
  - Netlink storm `20/20`: PASS (s30n1..s30n20 dummy interface add/up/del, Netlink listener active).
  - Resource checks: PASS (`fd_delta=0` <= 1, `thread_delta=0` <= 0, `rss_delta=16KB` <= 512KB, `rss_slope=0.000 KB/min` <= 64KB/min).
  - Cleanup and restoration: PASS (`/data/local/tmp/atpd-step30` deleted, module restored under KernelSU BusyBox ash with `ASH_STANDALONE=1`).
  - Read-only restoration verification (`RESTORATION_VERIFIED`): exactly 1 watchdog, 1 sentinel, 1 sing-box; ports 9080/9090, abstract socket, and wlan0 tc filter active; 30s stability verified; zero residue.
  - Raw evidence: `C:\Users\EricZhang\ATPD-device-backups\20260902-085850-Pixel_7_Pro-29271FDH300EJK\step30-run-20260903-103600\`
- **2026-09-03 Google VPN Dual Cycle (Pixel 7 Pro `29271FDH300EJK`)**:
  - Cycle 1 (OFF -> ON -> OFF): `ipsec7@lo` interface/route creation observed; ATPD Netlink XFRM sensing reported CONNECTED with interface `ipsec7` and live byte telemetry; Clash Mode dynamically transitioned to `Google VPN` (verified via both `atpd status` and Clash API `127.0.0.1:9090/configs`); on disconnection, `ipsec7` vanished, ATPD reverted to `STANDALONE / DIRECT`, and Clash Mode cleanly restored to `Rule`. Single `sb_share_in`/`sb_share_out` TC filter pair preserved without duplication.
  - Cycle 2 (OFF -> ON -> OFF): `ipsec19@lo` interface/route creation observed; ATPD Netlink XFRM sensing reported CONNECTED with interface `ipsec19`; Clash Mode switched to `Google VPN`; on disconnection, interface vanished, ATPD reverted to `STANDALONE / DIRECT`, and Clash Mode restored to `Rule`.
- **2026-09-03 Real Datapath (TCP + DNS/UDP)**:
  - Real DNS resolution (`httpbin.org`, `www.cloudflare.com`): intercepted by sing-box `inbound/ebpf[ebpf-in]` and resolved via authoritative DNS exchange.
  - Real TCP HTTP/HTTPS (`httpbin.org:80`, `www.cloudflare.com:443`): intercepted by sing-box `inbound/ebpf[ebpf-in]`, identified process `/data/adb/ksu/bin/busybox`, and routed through outbound proxy `outbound/trojan[🇭🇰 HK-Go-11-Jinx]`. Verified in sing-box log.
  - Raw evidence: `C:\Users\EricZhang\ATPD-device-backups\20260902-085850-Pixel_7_Pro-29271FDH300EJK\step30-datapath-20260903-111000\`
- **2026-09-03 ASan Instrumentation & Integration Suite**:
  - Compilation & linking: explicitly built with `-fsanitize=address` and linked with `-l:libasan.so.8`.
  - Binary verification: `ldd` and `readelf` confirmed `libasan.so.8` dependency and `__asan_*` symbols across all 12 binaries (`atpd` + 11 test binaries).
  - Standalone violation probe: external heap-buffer-overflow probe verified that the ASan runtime actively intercepts memory violations and aborts with diagnostic report.
  - Full suite run: `make clean && make -j2 DEBUG=1 test` passed 100% with zero AddressSanitizer diagnostics.
  - Lifecycle & crash recovery integration: `test_singbox_lifecycle.sh` (all 4 steps) and `test_service_crash_recovery.sh` passed under sanitized runtime.
  - Bug fix: eliminated misaligned cast to `struct rtnl_link_stats64` in `src/netlink.c` using safe `memcpy`, resolving UBSan alignment trap.
- **Release Build/Test & UBSan**:
  - `make clean && make -j2 && make test`: PASS.
  - UBSan build (`-fsanitize=undefined`) and full suite including lifecycle integration: PASS.
  - Shell script syntax (`bash -n tests/*.sh scripts/*.sh`): PASS.
  - Architecture invariant search: PASS (zero forbidden hits).
- Step 29 privileged benchmark and full resource stress PASS: crash recovery `10/10`, Netlink `200`, FD/thread delta `0/0`, RSS slope `0.000 KB/min`, no residual resources.

## Device State at Pause

The Step 30 deployment is removed. The original module is running with exactly one watchdog, sentinel, and sing-box. Watchdog environment contains `ASH_STANDALONE=1`; ports 9080/9090, `@sing-box-ebpf-shared-47`, and wlan0 `sb_share_in`/`sb_share_out` are active. PID identity was stable across the 30-second restoration check (`RESTORATION_VERIFIED`). No Step 30 process, socket, dummy interface, or device temporary file remains.

## Completed Gates for Step 30
 
1. **Real Android RC Smoke**: PASS (10/10 reload, 10/10 restart, 5/5 crash recovery, 20/20 netlink, zero resource leaks).
2. **Google VPN Dual Cycle**: PASS (OFF -> ON -> OFF x2, ipsec7/ipsec19 dynamic sensing, mode switch, clean revert).
3. **TCP + DNS/UDP Datapath**: PASS (ebpf-in packet interception, real HTTP/HTTPS and DNS traffic routed through outbound proxy).
4. **ASan Integration Suite**: PASS (explicit -fsanitize=address, libasan runtime verification, 0 diagnostics, full unit + integration + crash recovery passing).
5. **UBSan Integration Suite**: PASS (-fsanitize=undefined, netlink alignment fix verified, 0 diagnostics).
6. **Native Linux / CI TSan**: PASS (run 33715177549, 0 data races under native Linux Ubuntu 24.04 runner).
7. **GitHub CI / Build Matrix**: PASS (Android musl cross-compilation run 33714962695, sing-box integration run 33714962548, benchmarks run 33714962625).
8. **Final Release Gate**: PASS (all release artifacts built, verified clean, 100% green across all workflows).
 
All time-based soak testing (1-hour / 24-hour) remains classified as `MANUAL POST-RC VALIDATION` outside the automated Step 30 gate.

## Preserved Files

Preserved historical reports remain untracked and must not be modified or removed: `reports/step-01-report.md`, `reports/step-02-report.md`, and `reports/step-03-report.md`.