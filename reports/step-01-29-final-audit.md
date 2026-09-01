# ATPD Step 1–29 Final Audit — Post-Remediation

## Verdict

`PASS_WITH_MINOR_FINDINGS`

Pre-RC remediation 已关闭原 Final Audit 的 4 项 MAJOR。当前无 CRITICAL、无
MAJOR；仅保留一项由执行环境 capability 导致的 MINOR 验证缺口。

## Scope and checkpoint

- 审计范围仍仅限 Step 1–29 最终源码和本次 Pre-RC remediation。
- 未读取、审计或执行 Step 30；普通 preflight 仅自动打印过 Step 30 manifest
  路径，该文件未被打开。
- `.rework-state` 未推进：

```text
last_completed_step=29
current_step=30
status=ready
```

- 未重新机械审计 29 个 Step；仅复核原 4 项 MAJOR 的根因、受影响调用链及跨
  Step architecture invariants。

## Remediation closure

### MAJOR-01 — CLOSED: custom-config control lifecycle

- `src/cli.c` 不再拒绝 `-c/--config` 与 `stop`、`status`、`reload` 组合。
- `tests/test_cli.c` 固定 custom-config control grammar，并继续验证 trailing
  argument 和 command-specific mode rejection。
- `src/main.c` 由 daemon orchestration 单独保存启动时选定的绝对 config source；
  相对路径在 daemonize/chdir 前解析，但不解析 symlink target。
- `config_reload()` 由调用方显式传入 source path，不再从 desired-state
  `DATA_DIR` 猜测 `${DATA_DIR}/atp.conf`。
- benchmark/stress 的 start/status/reload/restart/stop 全部使用同一个显式
  config source，cleanup 不再落回默认 PID/socket path。

定点运行验证：

```text
atpd -c <valid-custom-config> status
→ parser accepts the command

atpd -c <valid-custom-config> stop/reload
→ reaches runtime PID discovery; no “only valid with” parser rejection
```

### MAJOR-02 — CLOSED in implementation: resource lifecycle gates

`tests/benchmark_atpd.sh` 现在：

- 以真实 custom config 启动并要求 UDS `RUNNING` readiness；
- status query 任一失败即 hard failure，不再使用 `|| true` 隐藏 parser/runtime
  failure；
- CSV 包含 RSS、VmHWM、VmSize、PSS、FD 和 Threads，并保存在 sandbox 外；
- 记录 baseline/status/netlink/recovery time series；
- 对 baseline RSS、recovery RSS growth、FD growth、thread growth 和 RSS
  least-squares slope 执行 hard gates；
- cleanup 只信任 sandbox PID file，且在发送 fallback signal 前校验
  `/proc/$pid/exe`。

`tests/stress_atpd_resources.sh` 现在覆盖：

- concurrent status storm；
- reload loop，ATPD PID 必须不变且 UDS 持续可用；
- TCP connection/session churn；
- restart loop，旧 ATPD PID 必须退出，新 ATPD/sing-box/UDS/inbound 必须 ready；
- sing-box `SIGKILL` failure injection 与 supervisor recovery；
- Netlink storm；
- recovery RSS slope、RSS/FD/thread hard growth gates；
- test-owned process identity validation、sandbox/interface cleanup 和持久 CSV artifact。

两个脚本在正式 workload 前执行可恢复的 `CAP_NET_ADMIN` probe。缺少 capability
时明确输出 `NOT PASS` 并执行 `exit 77`，不会产生虚假 PASS。

### MAJOR-03 — CLOSED: authoritative snapshot and read-only rendering

- `service_get_snapshot()` 与 `netlink_get_status_snapshot()` 暴露 owner-owned
  read-only snapshot/accessor。
- `status_collect_snapshot()` 一次性收集 ATPD/sing-box process resources、
  service state、Netlink/XFRM owner state、VPN owner snapshot、traffic totals 和
  system temperature。
- `atpd_rss_kb` 与 `atpd_hwm_kb` 已实际填充并渲染。
- sing-box PID 不再通过 process-name discovery fallback 猜测。
- `src/status_render.c` 只消费 `const status_snapshot_t`；不执行 `/proc`、filesystem、
  socket、netlink、service 或 process query。
- traffic state load/save 文件和 status query 写副作用已删除；重复 status 不再创建
  `traffic_state`。
- 无 authoritative owner snapshot 的 Clash mode/Goroutines/version/FCM 指标明确
  显示 unavailable，不伪造 telemetry。

### MAJOR-04 — CLOSED: explicit UI output boundary

- `ui_render_ctx_t` 是 stack-owned，显式携带 `FILE *`、width、color 和 emoji
  preference。
- UI 不保存 borrowed `FILE *`，无 `g_ui_out`、global no-color 或 global width
  state。
- UDS status 直接向 request-local `open_memstream` 进行 plain rendering，不切换
  process-wide sink。
- renderer isolation test 使用两个独立 memstream，验证内容不串流、plain output
  无 ANSI，并固定 snapshot resource fields。
- UTF-8 truncation 仍保持 codepoint boundary-safe。

## Validation results

| Validation | Result |
|---|---|
| initial `./scripts/codex-preflight.sh` | PASS |
| `make -j2` | PASS |
| `make test` | PASS |
| `bash -n tests/*.sh scripts/*.sh` | PASS |
| `build/tests/test_cli` | PASS |
| `build/tests/test_status_render` | PASS |
| custom-config status/stop/reload parser probes | PASS |
| offline status no-ANSI check | PASS |
| status no traffic-state write check | PASS |
| `git diff --check` | PASS |
| source ownership/boundary invariant searches | PASS |
| privileged resource benchmark | PASS — root run with `CAP_NET_ADMIN`; baseline/status/netlink/recovery gates completed |
| privileged resource stress | PASS — root run; status/reload/restart/session/crash/netlink/recovery/resource gates completed |
| `shellcheck` | N/A — tool unavailable |

The privileged resource commands were rerun as root. The environment has:

```text
uid=0
CapEff=000001ffffffffff
```

The benchmark reported `Result: PASS` with baseline RSS 1748KB, recovery RSS
1820KB, RSS slope 0.000KB/min, and FD/thread growth 0/0. The full stress run
reported `resource stress PASS` after status=5000, reload=100, restart=100,
sessions=1000, singbox_crash=10, Netlink storm=200, and ten recovery samples;
RSS delta was 124KB, RSS slope 0.000KB/min, and FD/thread delta 0/0.

## Findings

### CRITICAL

None.

### MAJOR

None.

### MINOR-01 — CLOSED: privileged resource workload executed

The root rerun exercised the corrected benchmark and stress harnesses, including
status/reload/restart/session churn, ten sing-box crash recoveries, Netlink storm,
recovery sampling, and hard resource gates. Both harnesses passed without residual
ATPD/sing-box processes, sockets, or test interfaces.

### INFO — commit signing environment

GPG signing with key `56BBBCE870EF17D9` was attempted and reached an interactive
passphrase prompt. The unattended session cannot provide the passphrase and did not
modify GPG configuration. The repository's AGENTS.md unattended migration-safe
exception therefore applies: the remediation commit is created with
`--no-gpg-sign`, and its hash is recorded in `.codex/HANDOFF.md`. After the
signing environment recovered, a separate signed attestation commit records
this handoff and the final audit without rewriting the unsigned commit.

## Architecture invariant recheck

- ATPD source still contains no `sys_bpf`, ATPD-owned eBPF probe, program/map
  lifecycle or fake dataplane telemetry.
- sing-box remains sole owner of `ebpf-in`; status wording continues to identify the
  sing-box inbound dataplane.
- config remains desired state only; selected config source is orchestration metadata,
  not a field in `atp_config_t`.
- `atpd_context` remains opaque; no god global or replacement context container was
  introduced.
- no umbrella compatibility header was introduced.
- service/netlink remain authoritative runtime owners; status only collects their
  snapshots.
- UI output, width, color and emoji state are render-call-local.
- no unrelated reactor/session/splice/async/UDS lifecycle ownership was moved.
- Step 30 files and work were not included.

## Final decision

The four original MAJOR findings and the privileged resource execution gap are
closed. The `CAP_NET_ADMIN` environment gap is closed by the root validation.
The code remains at the Step 29 completed / Step 30 ready checkpoint.

Final verdict: `PASS`
