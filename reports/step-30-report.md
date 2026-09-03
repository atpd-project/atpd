# Step 30 RC/Stable Validation Report

Result: PASS

All Step 30 release candidate and production stability gates have passed with authoritative, reproducible evidence across real Android hardware, sanitized local environments, and GitHub Actions remote CI.

---

## 1. Validated Gates & Authoritative Evidence

### Gate 1: Real-Device Android Smoke (Pixel 7 Pro 29271FDH300EJK)
- **Native API authoritative fields**: PASS (live child state published: Goroutines 69, Version 1.14.0-rc.1-4895c512, Clash Mode Rule, FCM Push Sensing ACTIVE).
- **Reload churn 10/10**: PASS (ATPD PID 5140 unchanged, authoritative status valid, 2080 session alive).
- **Restart churn 10/10**: PASS (distinct new PIDs verified on each cycle, old PIDs reaped, authoritative status restored).
- **Crash recovery 5/5**: PASS (stale status published as N/A on SIGKILL, child reaped and respawned under backoff, ATPD PID preserved, authoritative telemetry restored).
- **Netlink storm 20/20**: PASS (s30n1..s30n20 dummy interface add/up/del, Netlink listener active and healthy).
- **Resource verification**: PASS (d_delta=0 <= 1, 	hread_delta=0 <= 0, ss_delta=16KB <= 512KB, ss_slope=0.000 KB/min <= 64KB/min).
- **Cleanup and restoration**: PASS (/data/local/tmp/atpd-step30 deleted, original module restored under KernelSU BusyBox ash with ASH_STANDALONE=1).
- **Read-only restoration check (RESTORATION_VERIFIED)**: exactly 1 watchdog, 1 sentinel, 1 sing-box; ports 9080/9090, abstract socket, and wlan0 tc filter active; 30s stability verified; zero residue.
- **Evidence archive**: C:\Users\EricZhang\ATPD-device-backups\20260902-085850-Pixel_7_Pro-29271FDH300EJK\step30-run-20260903-103600\

### Gate 2: Google VPN Dual Cycle (Pixel 7 Pro 29271FDH300EJK)
- **Cycle 1 (OFF -> ON -> OFF)**: PASS. Interface ipsec7@lo and routing table 0x5342/0x5343 creation detected; ATPD Netlink XFRM sensing reported CONNECTED with interface ipsec7 and live byte telemetry; Clash Mode dynamically transitioned to Google VPN (verified via both tpd status and Clash API 127.0.0.1:9090/configs); on disconnect, ipsec7 vanished, ATPD reverted to STANDALONE / DIRECT, and Clash Mode cleanly restored to Rule. Single sb_share_in/sb_share_out TC filter pair preserved without duplication.
- **Cycle 2 (OFF -> ON -> OFF)**: PASS. Interface ipsec19@lo detected; ATPD Netlink XFRM sensing reported CONNECTED with interface ipsec19; Clash Mode switched to Google VPN; on disconnect, interface vanished, ATPD reverted to STANDALONE / DIRECT, and Clash Mode restored to Rule.

### Gate 3: Real Datapath (TCP + DNS/UDP)
- **DNS/UDP resolution**: PASS. Authoritative DNS resolution of httpbin.org and www.cloudflare.com intercepted by sing-box inbound/ebpf[ebpf-in] and resolved cleanly.
- **TCP HTTP/HTTPS traffic**: PASS. Real HTTP (http://httpbin.org:80/ip) and HTTPS (https://www.cloudflare.com:443/cdn-cgi/trace) requests intercepted by ebpf-in, identified process /data/adb/ksu/bin/busybox, and routed through remote outbound proxy outbound/trojan[🇭🇰 HK-Go-11-Jinx]. Verified in sing-box live access logs.
- **Evidence archive**: C:\Users\EricZhang\ATPD-device-backups\20260902-085850-Pixel_7_Pro-29271FDH300EJK\step30-datapath-20260903-111000\

### Gate 4: AddressSanitizer (ASan) Integration Suite
- **Instrumentation proof**: Compiled explicitly with -fsanitize=address and linked with libasan.so.8. ldd and eadelf confirmed libasan.so.8 dependency and __asan_* symbols across all 12 binaries (tpd + 11 test binaries).
- **Active interceptor proof**: External standalone heap-buffer-overflow probe confirmed AddressSanitizer intercepts memory violations and aborts with diagnostic report.
- **Full suite execution**: make clean && make -j2 DEBUG=1 test passed 100% with zero AddressSanitizer diagnostics.
- **Lifecycle & Crash Recovery**: 	est_singbox_lifecycle.sh (Steps 1-4) and 	est_service_crash_recovery.sh passed under ASan runtime with 0 leaks.
- **Remote CI ASan job**: SUCCESS (run 33715177549).

### Gate 5: UndefinedBehaviorSanitizer (UBSan) Integration Suite
- **Instrumentation**: Compiled with -fsanitize=undefined and linked with libubsan.so.1.
- **Bug fix**: Replaced unaligned (struct rtnl_link_stats64 *)RTA_DATA(rta) pointer cast in src/netlink.c with safe memcpy, eliminating alignment trap.
- **Remote CI UBSan job**: SUCCESS (run 33715177549). Zero undefined behavior diagnostics.

### Gate 6: Native Linux ThreadSanitizer (TSan) Suite
- **Runner environment**: Native Linux x86_64 (ubuntu-24.04 GitHub Actions runner).
- **Execution**: Configured m.mmap_rnd_bits=28 to prevent ASLR shadow memory overlap, and used Zig Clang's statically bundled LLVM compiler-rt TSan runtime.
- **Remote CI TSan job**: SUCCESS (run 33715177549). Zero data race warnings or concurrency diagnostics.

### Gate 7: GitHub CI / Build Matrix
- **RC Sanitizer Validation** (run 33715177549): SUCCESS (ASan, UBSan, TSan all completed green).
- **Test ATPd with sing-box (Linux Runner)** (run 33714962548): SUCCESS.
- **Benchmark & Performance Regression** (run 33714962625): SUCCESS.
- **Build ATP for Android (musl)** (run 33714962695): SUCCESS (Multi-arch Android musl release binaries built and packaged).

### Gate 8: Final Release Gate
- Release binaries, unit tests, integration tests, shell scripts, and architecture invariants verified 100% clean and passing.
- No residual temporary files, processes, or ports on testing environments.
- State file updated to current_step=31, last_completed_step=30, status=complete.
