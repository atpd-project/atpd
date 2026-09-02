# Step 30 RC/stable validation

Result: `BLOCKED`

Step 30 was started from the verified Step 29 checkpoint. No product source,
tests, scripts, or harness files were modified.

## Validation performed

- `git verify-commit HEAD`: PASS for signed commit `27738f79e6c65e73999c7c1f1d85bf9f530c59d8`.
- `./scripts/codex-preflight.sh --resume`: PASS before validation.
- Release `make clean && make -j2 && make test`: PASS.
- UBSan build and `make test`: PASS.
- ASan debug build: PASS; representative invalid-configuration probe and C unit
  tests passed without sanitizer diagnostics. The full shell configuration
  validation loop blocked when repeatedly invoking the ASan ATPD binary, so
  ASan integration coverage is incomplete rather than marked PASS.
- TSan build: PASS; runtime execution is unsupported in this WSL environment,
  failing with `ThreadSanitizer: unexpected memory mapping`.
- `bash -n tests/*.sh scripts/*.sh`: PASS.
- Architecture invariant searches for ATPD eBPF ownership, god globals,
  umbrella headers, and global UI sinks: no forbidden hits.
- `docs/android-device-test.md` documents T01-T09 scenarios, but no `adb`
  executable or Android device is available in this environment.

## Hard stop

The required RC/stable evidence cannot be completed here:

- No real Android device or `adb` is available for the Android recovery,
  transition, and platform matrix (Android 12+, GKI/device, Magisk, KernelSU,
  APatch).
- No executable 24-hour real-device soak/release gate is available.
- TSan runtime is unavailable, and ASan integration coverage remains blocked by
  the existing shell-loop execution behavior.

The previously completed privileged Step 29 benchmark/stress evidence remains
valid, but it does not substitute for Step 30 Android matrix and soak evidence.
No Step 30 PASS or RC/stable claim is made.

## Android device attempt — 2026-09-02

The real-device gate was attempted on the single connected Google Pixel 7 Pro
(`29271FDH300EJK`, Android 16 / SDK 36, arm64-v8a, KernelSU root). The verified
pre-test backup was rechecked before isolation:

- archive: `atpd-device-backup.tar` (306,720,768 bytes);
- SHA-256: `bce4cf8e927a0eeb654f9f093c773f1b06820d3ec6140ad6beb31f22b766d237`;
- restoration inventory: 804/804 entries and 790/790 regular-file hashes.

The old module watchdog, sentinel, sing-box, listeners, abstract socket, and
`wlan0` BPF filters were isolated successfully. No new sing-box was installed:
the test root reused `/data/adb/atp/bin/sing-box` 1.14.0-rc.1-4895c512
(`with_ebpf`). HEAD `4c6641fb7c32bf9b109c49e27ab89b7f41011e1a` was built in a clean detached
worktree with the pinned Zig 0.15.2 toolchain as a static arm64 musl binary;
the deployed binary reported `1.0.0-dev+g4c6641fb7c32` and matched SHA-256
`1c458d659459fd3708e2ef617f4003cc9af721d3ba3e6a5b98b2bf05d7b6be8a`.

### Product blocker

Android smoke failed at the first startup/authoritative-snapshot gate. ATPD
became `RUNNING`, but every supervised sing-box child exited with code 127:

```text
Service: spawned sing-box
Service: unknown user 'root'
Service: sing-box exited with code 127
```

Consequently there was no `sing-box.pid`, ports 9080/9090/2080 were not
listening, and Goroutines, Version, Clash Mode, and FCM Push Sensing could not
become authoritative. This is a real Android product defect in service child
credential resolution; it is not recorded as an environment PASS and was not
worked around. The requested reload 10, restart 10, crash recovery 5, Netlink
20, datapath/session/resource gates, and 1-hour soak were not run after this
startup hard stop.

### Cleanup and restoration

Complete ATPD/device/log/dmesg evidence was captured before cleanup. The test
daemon was stopped and `/data/local/tmp/atpd-step30` was removed. The backup
archive was pushed back, re-verified on-device with the recorded SHA-256, and
extracted over its original paths. Eight critical static files (ATPD/ATP
scripts, configs, service entry, and the existing sing-box) matched their
backup hashes 8/8. The original sing-box command line, Native API 9080, Clash
API 9090, `@sing-box-ebpf-shared-47`, and `wlan0` `sb_share_in`/`sb_share_out`
attachments were restored.

The module watchdog could not be safely restored from an interactive `su`
launch: its `pkill -0 -f` resolves to Android Toybox `pkill`, which rejects
signal 0 and caused a sentinel fork storm. No wrapper or script modification
was introduced. The storm was stopped; the device was left stable with one
original sing-box and one original sentinel, but with the module watchdog
stopped. This restoration-process gap is explicit and requires review before
another Android attempt.

Evidence directory:

`C:\Users\EricZhang\ATPD-device-backups\20260902-085850-Pixel_7_Pro-29271FDH300EJK\step30-validation-20260902-121757`

Checkpoint remains `last_completed_step=29`, `current_step=30`,
`status=blocked`. No Step 30 PASS is claimed.
