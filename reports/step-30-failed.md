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
