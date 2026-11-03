# Pixel smoke test of the Rust atpd (instructions for agy on Windows)

You are testing a new build of `atpd` (a root daemon) on the user's Pixel phone over USB.
The on-phone script does all the testing and **always puts the original C `atpd` back** when it ends.
Your job: download the build, copy two files to the phone, run one command, collect the report.
Use PowerShell. Do not improvise steps that change the phone beyond what is written here.

**Start only when the user has told you the Rust build is ready for device testing.** Earlier builds are placeholders that cannot run the daemon.

## Rules
- Only run the commands below. Do not edit files on the phone by hand, except the manual restore in step 7.
- Never edit `pixel-smoke.sh`, `/data/adb/atp/atp.conf` or `/data/adb/atp/config.json`.
- Never set `KEEP_RUST=1` unless the user explicitly asks to keep the Rust build installed.
- If a step's check fails, stop and report the exact command and its full output. Do not retry with different flags.
- The phone will lose its proxy for roughly 3–5 minutes while the test runs. Tell the user before step 5.

## 1. Preconditions
```powershell
adb version
gh auth status
adb devices
```
Check: `adb devices` lists exactly one device in state `device`. If it says `unauthorized`, ask the user to accept the USB debugging prompt on the phone, then run it again.

```powershell
adb shell su -c id
```
Check: the output contains `uid=0`. If the phone shows a root (Magisk/KernelSU) prompt, ask the user to grant it, then run it again.

## 2. Download the latest successful build
```powershell
$dir = Join-Path $env:TEMP "atpd-rs"
Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
$run = gh run list -R atpd-project/atpd -b main -w "ATPD Rust CI" -s success -L 1 --json databaseId,headSha | ConvertFrom-Json
$run
gh run download $run[0].databaseId -R atpd-project/atpd -n atpd-bionic-arm64 -D $dir
Get-ChildItem $dir
```
Check: `$dir` contains `atpd` and `pixel-smoke.sh`. Write down `headSha` for the report.

## 3. Copy to the phone
```powershell
adb push (Join-Path $dir "atpd") /data/local/tmp/atpd-rust
adb push (Join-Path $dir "pixel-smoke.sh") /data/local/tmp/pixel-smoke.sh
adb shell su -c "chmod 755 /data/local/tmp/atpd-rust"
adb shell su -c "/data/local/tmp/atpd-rust version"
```
Check: the last command prints an `atpd` version line (not `not executable` or `exec format error`).

## 4. Confirm the phone's current state (before)
```powershell
adb shell su -c "/data/adb/atp/atpd -n status"
adb shell su -c "ls -l /data/adb/atp/atpd.c-backup"
```
Check: the status shows `RUNNING / healthy`, and `atpd.c-backup` does **not** exist (`No such file`).
If the backup file exists, a previous run did not finish: go to step 7 first.

## 5. Run the test
Tell the user the proxy will be down for a few minutes, then run:
```powershell
adb shell su -c "sh /data/local/tmp/pixel-smoke.sh /data/local/tmp/atpd-rust"
```
It takes about 3 minutes. Each line starts with `PASS`, `FAIL`, `INFO`, `SKIP` or `RESULT`.

**VPN test (only if the user is at the phone and agrees):** add ` --vpn` after `atpd-rust` inside the quotes. The script then prints `ACTION` lines. Immediately tell the user what each one asks (turn the VPN on, then off). The script waits up to 120 seconds for each.

## 6. Collect results and confirm the phone was restored (after)
```powershell
adb pull /data/local/tmp/atpd-smoke-report.txt $dir
adb shell su -c "/data/adb/atp/atpd version"
adb shell su -c "/data/adb/atp/atpd -n status"
adb shell su -c "ls -l /data/adb/atp/atpd.c-backup"
```
Check: the report's last lines contain `INFO  restored C atpd` and a `RESULT pass=N fail=M` line. The version is the C one again (same as the `C atpd:` line at the top of the report), status shows `RUNNING / healthy`, and `atpd.c-backup` no longer exists.

## 7. Manual restore (only if the run was interrupted)
Use this only if `/data/adb/atp/atpd.c-backup` still exists after a run, for example because USB disconnected or the window was closed.
```powershell
adb shell su -c "/data/adb/atp/atpd stop"
adb shell su -c "cp /data/adb/atp/atpd.c-backup /data/adb/atp/atpd && chmod 755 /data/adb/atp/atpd && rm /data/adb/atp/atpd.c-backup"
adb shell su -c "/data/adb/atp/atpd start"
adb shell su -c "/data/adb/atp/atpd -n status"
```
Check: the status shows `RUNNING / healthy`. If it does not, stop and tell the user right away, because their proxy is down.

## 8. Report back to the user
Send:
1. The `headSha` from step 2.
2. The `RESULT pass=N fail=M` line.
3. Every `FAIL` line, word for word.
4. The full contents of `atpd-smoke-report.txt` (path: `$dir\atpd-smoke-report.txt`).
5. Whether step 6 confirmed the phone is back on the C atpd and healthy.
6. Any command that failed outside the script, with its full output.

Do not interpret failures or suggest fixes. Report them exactly.
