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

## Remaining automated gates

Step 30 remains blocked pending:

- A clean, from-zero Android smoke run with raw per-iteration evidence.
- TSan runtime is unavailable, and ASan integration coverage remains blocked by
  the existing shell-loop execution behavior.
- Native Linux/CI sanitizer execution and the final CI/build matrix rerun.
- The final release gate after all automated evidence is complete.

The previously completed privileged Step 29 benchmark/stress evidence remains
valid, but it does not substitute for the remaining Step 30 automated evidence.
No Step 30 PASS or RC/stable claim is made.

All time-based soak testing, including 1-hour and 24-hour runs, is classified as
`MANUAL POST-RC VALIDATION`. It is performed separately by the operator and is
not part of the automated Step 30 PASS gate.

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
stopped. This historical restoration-process gap was closed before the resume
attempt below by launching the original service under KernelSU BusyBox ash with
`ASH_STANDALONE=1`.

Evidence directory:

`C:\Users\EricZhang\ATPD-device-backups\20260902-085850-Pixel_7_Pro-29271FDH300EJK\step30-validation-20260902-121757`

Checkpoint remains `last_completed_step=29`, `current_step=30`,
`status=blocked`. No Step 30 PASS is claimed.

## Android resume and pause — 2026-09-02

Signed commit `8de80df1a24482b402febcd13c415f842aafd26f` fixed Android
root credential resolution and was deployed as a static arm64 build. Initial
startup demonstrated that the existing sing-box could start and publish real
Goroutines, Version, Clash Mode, and FCM Push Sensing values, but the workload
result is not counted: a temporary runner helper overwrote its outer counter,
so only one restart and one crash were executed while its summary claimed the
target counts.

A corrected runner began a second run, but it was interrupted when a stricter
fresh-deployment requirement was issued. No Android smoke result from either
run is reused. The required clean rerun remains startup plus reload `10/10`,
restart `10/10`, crash recovery `5/5`, Netlink/interface `20/20`, authoritative
Native API, datapath/session, FD/thread/RSS, and cleanup/restoration gates, with
raw per-iteration evidence.

Before pausing, the complete temporary test root was pulled to the host evidence
directory and the device deployment was removed. The original module was
started with KernelSU BusyBox standalone semantics. Restoration verification
found exactly one stable watchdog, sentinel, and sing-box; ports 9080/9090,
`@sing-box-ebpf-shared-47`, and `wlan0` `sb_share_in`/`sb_share_out` were active;
no Step 30 process, socket, interface, or device temporary file remained.

Resume evidence:

`C:\Users\EricZhang\ATPD-device-backups\20260902-085850-Pixel_7_Pro-29271FDH300EJK\step30-resume-20260902-160321`
