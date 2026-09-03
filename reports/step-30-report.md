# Step 30 Report — RC / Stable Validation

## Result

PASS

All automated Step 30 gates completed successfully from the verified Step 29 checkpoint. The real Android device gate was executed on a connected Google Pixel 7 Pro (\29271FDH300EJK\, Android 16 / SDK 36, KernelSU root) from zero baseline, producing verified per-iteration evidence with zero resource growth and zero residue.

## Plans Used

- \.codex/steps/30-rc-stable-validation.md\
- \docs/android-device-test.md\
- \CODEX_AUTOPILOT.md\
- \.codex/CURRENT_ARCHITECTURE.md\

## Files Changed

- \.codex/HANDOFF.md\
- \.codex/steps/30-rc-stable-validation.md\
- \docs/android-device-test.md\
- \eports/step-30-report.md\
- \.codex/CURRENT_ARCHITECTURE.md\
- \.rework-state\

## Architecture and Ownership Verification

- sing-box maintains exclusive ownership of the \ebpf-in\ datapath; ATPD manages child supervision, Native API snapshot publication, and control plane orchestration.
- ATPD context remains opaque and owns lifecycle/VPN snapshots; session, XFRM, readiness, error, and status retain subsystem ownership.
- Authoritative telemetry: Goroutines, Version, Clash Mode, and FCM Push Sensing are served purely from live child snapshot aggregation with generation stamping; stale states are explicitly published as \N/A\ during child restarts/crashes.
- Zero god globals, zero umbrella headers, zero global UI sinks confirmed via targeted repository searches.

## Verification Performed

### 1. Android Real-Device Automated Smoke (Pixel 7 Pro \29271FDH300EJK\)
- **Preflight & Backup**: Device backup \tpd-device-backup.tar\ (SHA-256: \ce4cf8e927a0eeb654f9f093c773f1b06820d3ec6140ad6beb31f22b766d237\) verified. Existing module isolated cleanly via BusyBox.
- **Binary Build**: Static \arch64-linux-musl\ built with pinned Zig 0.15.2 toolchain (\31a389c57a2c6daa7f2ee89a82d4df203e5890331ae580c10c55130f216c2f30\, 184 KB).
- **Startup & Authoritative Status**: ATPD PID 5140 spawned sing-box child PID 5142. State \RUNNING\, Goroutines 69, Version \1.14.0-rc.1-4895c512\, Clash Mode \Rule\, FCM Push Sensing \ACTIVE\.
- **Reload Loop (\10/10\)**: ATPD PID 5140 preserved across all 10 iterations; configuration reloaded, authoritative status and 2080 mixed session verified per iteration.
- **Restart Loop (\10/10\)**: Distinct new PIDs verified on every iteration (\5140->5848->6301->6903->7179->7477->8073->8748->9158->9707->10101\); old PIDs confirmed dead, authoritative status and session endpoint restored.
- **Crash Recovery (\5/5\)**: SIGKILL delivered to child sing-box; stale invalidation observed (\Clash Mode: N/A\, \FCM Push Sensing: N/A\), child reaped and respawned under backoff (\10106->10818->11244->11794->12371->12805\), ATPD PID 10101 preserved, authoritative telemetry restored.
- **Netlink / Interface Storm (\20/20\)**: Dummy interfaces \s30n1\..\s30n20\ sequentially added, set up, and deleted; Netlink listener remained active, zero crash or hang.
- **Datapath / Session Churn**: 20 consecutive session connections plus binary SOCKS5 no-auth negotiation handshake (\