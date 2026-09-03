# ATPD Codex Handoff

Repository: `/home/ezhang/atpd`
Branch: `ebpf-native-api`
Product build under validation: `8de80df1a24482b402febcd13c415f842aafd26f`

Checkpoint (must remain unchanged while paused):

```text
last_completed_step=29
current_step=30
status=blocked
```

## Valid evidence retained

- Release build/test PASS and UBSan PASS.
- ASan probe/unit tests PASS; ASan integration is still incomplete.
- Step 29 privileged benchmark and full resource stress PASS: crash recovery
  `10/10`, Netlink `200`, FD/thread delta `0/0`, RSS slope `0.000 KB/min`, no
  residual resources.
- Commit `8de80df1a24482b402febcd13c415f842aafd26f` is GPG-signed by
  `56BBBCE870EF17D9`; its Android root/numeric credential fix passed local
  build/test and produced the deployed arm64 binary.
- Device preflight and backup are valid for Pixel 7 Pro serial
  `29271FDH300EJK`. Backup root:
  `C:\Users\EricZhang\ATPD-device-backups\20260902-085850-Pixel_7_Pro-29271FDH300EJK`.

## Invalidated Android results

Do not reuse either 2026-09-02 workload attempt. The first temporary runner
overwrote its outer counter and executed only one restart and one crash despite
claiming target counts. A corrected run was then interrupted before the newly
required delete-and-fresh-redeploy boundary. No Android smoke gate is credited.
Raw evidence and the temporary runner are preserved under:

`C:\Users\EricZhang\ATPD-device-backups\20260902-085850-Pixel_7_Pro-29271FDH300EJK\step30-resume-20260902-160321`

## Device state at pause

The Step 30 deployment is removed. The original module is restored with exactly
one watchdog, sentinel, and sing-box. Watchdog environment contains
`ASH_STANDALONE=1`; ports 9080/9090, `@sing-box-ebpf-shared-47`, and wlan0
`sb_share_in`/`sb_share_out` are active. PID identity was stable across the
restoration check. No Step 30 process, socket, dummy interface, or device
temporary file remains.

## Remaining automated Step 30 gates

1. Fresh Android deployment and from-zero smoke with raw per-iteration proof:
   startup, authoritative Native API fields, reload `10/10`, restart `10/10`,
   crash recovery `5/5`, Netlink/interface `20/20`, datapath/session,
   FD/thread/RSS, and cleanup/restoration.
2. Complete ASan integration validation.
3. Run TSan in a supported native Linux/CI runtime; WSL TSan is not PASS.
4. Complete the required GitHub CI/build matrix and final release gate on the
   resulting HEAD.

All time-based soak testing is `MANUAL POST-RC VALIDATION`; it is outside the
automated Step 30 PASS gate. No 1-hour, 24-hour, or other soak should be started
by the automated run.

## Exact resume state and commands

Resume only after an explicit new instruction. The paused working tree contains
only the intended Step 30 policy/report/checklist/handoff updates plus the three
pre-existing historical reports. First review and GPG-sign a documentation-only
commit containing `.codex/HANDOFF.md`, `.codex/steps/30-rc-stable-validation.md`,
`.rework-state`, `docs/android-device-test.md`, and
`reports/step-30-failed.md`; do not add the historical reports. Then keep step
numbers unchanged, change `.rework-state` from `status=blocked` to
`status=in_progress`, clear `blocked_reason`, and run:

```sh
./scripts/codex-preflight.sh --resume
```

Before isolation, re-verify serial `29271FDH300EJK`, root, backup hash
`bce4cf8e927a0eeb654f9f093c773f1b06820d3ec6140ad6beb31f22b766d237`,
and the restored 1/1/1 process/port/BPF baseline. Isolate the existing module in
the KernelSU BusyBox environment:

```sh
ASH_STANDALONE=1 /data/adb/ksu/bin/busybox sh /data/adb/atp/atp.sh stop
```

After testing, restore only with BusyBox standalone applet resolution:

```sh
ASH_STANDALONE=1 /data/adb/ksu/bin/busybox nohup \
  /data/adb/ksu/bin/busybox sh /data/adb/service.d/atp4_service.sh \
  >/dev/null 2>&1 &
```

Never use interactive Toybox `pkill` for module restoration.

Preserved historical reports remain untracked and must not be modified or
removed: `reports/step-01-report.md`, `reports/step-02-report.md`, and
`reports/step-03-report.md`.

Unattended unsigned commits retained from earlier work: `bf9c6ef`, `c21f3be`,
`167b256`, `4114564`, `e36b8b9`, `2ba27c9`, `c95a2f2`, `34ca744`, `7788c02`,
`f6cf3b6`, `49a8ad5`, `6a9db29`.
