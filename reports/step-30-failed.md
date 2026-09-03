# Step 30 RC/Stable Validation

Result: `BLOCKED`

Step 30 Android clean smoke run succeeded from zero baseline, but the Step 30 RC/stable gate is blocked pending real-device VPN/traffic validation, explicit ASan execution proof, native Linux/CI TSan, and GitHub CI/release gates.

## Valid Evidence Retained

- **Real-Device Android Smoke (Pixel 7 Pro `29271FDH300EJK`)**:
  - Native API authoritative fields: PASS (live child state published: Goroutines 69, Version 1.14.0-rc.1-4895c512, Clash Mode Rule, FCM Push Sensing ACTIVE).
  - reload churn `10/10`: PASS (ATPD PID 5140 unchanged, authoritative status valid, 2080 session alive).
  - restart churn `10/10`: PASS (distinct new PIDs verified on each cycle, old PIDs reaped, authoritative status restored).
  - crash recovery `5/5`: PASS (stale status published as N/A on SIGKILL, child reaped and respawned under backoff, ATPD PID preserved, authoritative telemetry restored).
  - Netlink storm `20/20`: PASS (s30n1..s30n20 dummy interface add/up/del, Netlink listener active).
  - Resource checks: PASS (`fd_delta=0` <= 1, `thread_delta=0` <= 0, `rss_delta=16KB` <= 512KB, `rss_slope=0.000 KB/min` <= 64KB/min).
  - Cleanup and restoration: PASS (`/data/local/tmp/atpd-step30` deleted, module restored under KernelSU BusyBox ash with `ASH_STANDALONE=1`).
  - Read-only restoration verification (`RESTORATION_VERIFIED`): exactly 1 watchdog, 1 sentinel, 1 sing-box; ports 9080/9090, abstract socket, and wlan0 tc filter active; 30s stability verified; zero residue.
  - Raw evidence: `C:\Users\EricZhang\ATPD-device-backups\20260902-085850-Pixel_7_Pro-29271FDH300EJK\step30-run-20260903-103600\`
- **Release Build/Test & UBSan**:
  - `make clean && make -j2 && make test`: PASS.
  - UBSan build and full suite: PASS.
  - Shell script syntax (`bash -n tests/*.sh scripts/*.sh`): PASS.
  - Architecture invariant search: PASS (zero forbidden hits).

## Missing Evidence / Remaining Gates

1. **Google VPN ON/OFF Real-Device Transition**:
   - Verification of ATPD VPN interface observation, Clash Mode switching on connection, and clean restoration on disconnection without stale state retention.
2. **Real Datapath Verification (TCP + DNS/UDP)**:
   - Genuine TCP and DNS/UDP traffic verification through `ebpf-in` to confirm real end-to-end routing.
3. **Verified ASan Execution**:
   - Proof that `-fsanitize=address` was actively enabled during execution and verified with an intentional violation probe or sanitizer banner.
4. **Native Linux / CI TSan**:
   - Full TSan execution in a native Linux environment (WSL memory mapping limitation prevents local TSan run).
5. **GitHub CI / Build Matrix**:
   - Full cross-compilation and CI build matrix validation on GitHub Actions.
6. **Final Release Gate**:
   - Final release validation on resulting HEAD after all evidence is assembled.

All time-based soak testing (1-hour / 24-hour) remains classified as `MANUAL POST-RC VALIDATION` outside the automated Step 30 gate.