# ATPD `status.c` 可观测性与诊断能力优化方案

## 1. 目标

当前 `status.c` 已经能够展示较多运行信息，但它仍然同时承担：

- `/proc` 数据采集
- sing-box Native API 查询
- CPU / memory / temperature 获取
- Netlink / XFRM 状态推断
- eBPF telemetry 展示
- traffic sampling
- UI table rendering
- 字节格式化
- 部分运行时状态判断

随着 ATPD 逐渐从个人脚本演进为长期驻留的 root networking daemon，`status` 不应继续只是一个“漂亮的状态页面”。

本次优化的目标是：

> 将 `atpd status` 升级为 ATPD 正式的 observability / diagnostics interface。

最终应做到：

1. `status` 查询快速、稳定，不因 sing-box Native API 故障阻塞数秒。
2. 状态信息可信，不通过模糊条件推测 Netlink/XFRM 为 ACTIVE。
3. 能直接观察 ATPD 自身 RSS、VmHWM、FD、Threads、Uptime 等资源状态。
4. 查询操作尽量只读，不因为执行 `atpd status` 改变采样状态。
5. 采集逻辑与渲染逻辑分离。
6. 为测试、Android UI 和自动诊断提供稳定 JSON 输出。
7. 后续新增指标时不继续膨胀单个 `status.c`。

---

# 2. 当前主要问题

## P0-1：Native API 查询位于 status 热路径

当前 `status` 会同步调用 sing-box Native API 获取：

- runtime status
- version
- mode 等信息

相关同步 API 可能包含 retry / sleep。

问题：

```text
atpd status
  ↓
Native API unavailable
  ↓
同步 retry
  ↓
status 延迟达到秒级
```

这意味着一个纯诊断命令会被一个异常组件拖慢。

风险：

- Native API 故障时无法快速诊断
- benchmark latency 被异常状态放大
- Android UI 若调用 status 会卡顿
- 多次 status 并发会放大 IPC/API 压力

### 目标

`atpd status` 本身不应进行长时间 Native API polling。

应优先读取 daemon 已维护的 runtime snapshot。

---

# 3. 推荐总体架构

将当前：

```text
status.c
 ├── collect
 ├── query
 ├── calculate
 ├── side effect
 └── render
```

重构为：

```text
runtime producers
       │
       ▼
status snapshot
       │
 ┌─────┴─────┐
 ▼           ▼
human UI    JSON
```

推荐拆分：

```text
src/
├── status.c
├── status_collect.c
├── status_render.c
├── status_json.c
└── status_resource.c

include/
├── status.h
└── status_internal.h
```

若不希望一次增加太多文件，可第一阶段至少实现：

```text
status.c
status_collect.c
```

然后后续再拆 renderer。

---

# 4. 引入统一 `status_snapshot_t`

建议增加统一结构：

```c
typedef struct {
    uint64_t collected_at_ms;

    atpd_status_info_t atpd;
    singbox_status_info_t singbox;
    network_status_info_t network;
    ebpf_status_info_t ebpf;
    supervisor_status_info_t supervisor;
    system_status_info_t system;

    health_status_info_t health;
} status_snapshot_t;
```

核心原则：

> 所有 renderer 只能读取 snapshot，不自行执行复杂查询。

---

# 5. ATPD 自身状态

当前 status 重点展示 sing-box，但 ATPD 自身的资源观测不足。

新增：

```c
typedef struct {
    pid_t pid;

    uint64_t uptime_sec;

    long rss_kb;
    long hwm_kb;
    long vm_size_kb;

    int fd_count;
    int thread_count;

    int uds_clients;
    int reactor_fd_count;
    int timer_count;

    int running;
} atpd_status_info_t;
```

第一阶段至少必须提供：

```text
PID
Uptime
RSS
Peak RSS / VmHWM
FD count
Threads
```

### 推荐 human output

```text
ATPD DAEMON
  State              RUNNING
  PID                1234
  Uptime             2d 06:31:10
  RSS                2.38 MB
  Peak RSS           2.81 MB
  FDs                11
  Threads            1
```

---

# 6. ATPD RSS / VmHWM 采集

优先读取：

```text
/proc/self/status
```

字段：

```text
VmRSS
VmHWM
VmSize
Threads
```

不要执行：

```text
ps
awk
grep
```

应直接 C 解析 `/proc/self/status`。

建议 API：

```c
int status_read_self_proc(atpd_status_info_t *out);
```

要求：

- 单次打开
- 单次读取/逐行解析
- 任一非关键字段失败时不导致整个 status 失败
- 未获取字段使用 unknown sentinel

例如：

```c
#define STATUS_VALUE_UNKNOWN (-1)
```

---

# 7. FD Count

读取：

```text
/proc/self/fd
```

使用：

```text
opendir
readdir
closedir
```

排除：

```text
.
..
```

注意：

打开 `/proc/self/fd` 本身会临时产生一个 FD。

计数逻辑必须考虑这一点。

建议：

```c
int status_get_fd_count(void);
```

如无法得到完全稳定值，至少保证误差行为明确。

---

# 8. Uptime

不要通过外部命令获取。

优先：

```text
CLOCK_MONOTONIC
```

ATPD startup 时保存：

```c
uint64_t daemon_started_monotonic_ms;
```

status：

```text
now - startup
```

如果当前 context 已保存 startup timestamp，应直接复用。

---

# 9. Native API 改成 runtime cache

新增缓存：

```c
typedef struct {
    uint64_t updated_at_ms;

    int ready;

    char version[64];
    char clash_mode[32];

    uint64_t memory_bytes;
    int goroutines;
    int connections_in;
    int connections_out;

    int64_t uplink_bps;
    int64_t downlink_bps;

    int64_t uplink_total;
    int64_t downlink_total;

    int last_error;
} singbox_runtime_cache_t;
```

更新方：

```text
Native API monitoring path
```

而不是：

```text
status command
```

---

# 10. Snapshot Freshness

status 必须知道缓存是否新鲜。

增加：

```text
updated_at_ms
```

并计算：

```text
snapshot_age_ms
```

建议显示：

```text
Native API         READY
Snapshot Age       218 ms
```

超过阈值：

```text
Native API         DEGRADED
Snapshot Age       8.2 s
```

建议初始阈值：

```text
fresh     < 3 s
stale     3–10 s
degraded  > 10 s
```

最终数值根据 Native API subscribe/update 周期调整。

---

# 11. 禁止 status 同步长时间 retry

`status` 调用链中：

```text
任何同步 API
任何 socket operation
任何 proc read
```

都不应允许秒级阻塞。

建议 status 单次 budget：

```text
< 100 ms normal
< 250 ms degraded
```

其中大部分正常请求应：

```text
< 20 ms
```

如果因历史兼容必须保留 fallback query：

```text
timeout <= 100 ms
retry = 0
```

但推荐最终完全移除。

---

# 12. 修复 Netlink / XFRM 假 ACTIVE

当前不能通过：

```text
daemon running
或 netlink fd >= 0
```

推断：

```text
Netlink ACTIVE
XFRM ACTIVE
```

两者必须分别提供真实状态。

建议接口：

```c
int netlink_is_registered(void);
int netlink_get_fd(void);

int xfrm_is_registered(void);
int xfrm_get_fd(void);
```

更好：

```c
typedef enum {
    MONITOR_STOPPED,
    MONITOR_ACTIVE,
    MONITOR_RETRYING,
    MONITOR_DEGRADED,
    MONITOR_FAILED
} monitor_state_t;
```

并提供：

```c
monitor_state_t netlink_get_state(void);
monitor_state_t xfrm_get_state(void);
```

---

# 13. 显示 Monitor Last Error

建议记录：

```text
last_error
last_error_time
retry_count
```

human output：

```text
MONITORS
  Netlink            ACTIVE
  XFRM               DEGRADED
  XFRM Last Error    epoll_ctl: ENOMEM
  XFRM Retry         3
```

这样 status 才能用于定位 reactor registration / kernel socket 问题。

---

# 14. Supervisor 状态

新增独立区域：

```text
SUPERVISOR
```

字段建议：

```text
sing-box child PID
running state
restart count
last restart time
last exit code
last signal
last error
stop state
restart/backoff state
```

数据必须来自 `service.c` / supervisor 的真实内部状态。

不要通过：

```text
pgrep
/proc basename substring
```

重新猜。

---

# 15. 推荐 Supervisor Snapshot

```c
typedef struct {
    pid_t child_pid;

    int running;
    uint64_t restart_count;

    int last_exit_code;
    int last_signal;

    uint64_t last_start_ms;
    uint64_t last_exit_ms;

    char last_error[128];
} supervisor_status_info_t;
```

---

# 16. eBPF 状态语义修正

必须审查：

```text
active_conns
```

到底表示：

```text
connections
socket entries
tracked FDs
map entries
```

不要显示成含义不准确的：

```text
xxx sing-box FDs
```

字段名必须跟数据语义一致。

建议：

```text
Active Connections
Tracked Sockets
Map Entries
```

三者按实际 telemetry 选择。

---

# 17. eBPF Health

如果可获得，应增加：

```text
Kernel Support
Program Loaded
Program Attached
Maps Ready
Last Event
Last Error
```

推荐：

```text
eBPF
  Kernel Support      YES
  Program             ATTACHED
  Maps                HEALTHY
  Active Connections  27
```

不要只显示一个泛化的 runtime signal。

---

# 18. 去除 traffic sampling 副作用

当前若 `atpd status` 会：

```text
read traffic
read previous sample
calculate speed
write sample state file
```

应逐步移除。

原则：

> status query 应尽量 read-only / idempotent。

执行：

```bash
atpd status
atpd status
atpd status
```

不应改变采样历史。

---

# 19. Traffic 应由 daemon 持续维护

建议将 traffic sampling 改为：

```text
daemon telemetry timer
    ↓
collect counters
    ↓
calculate delta
    ↓
cache
```

status：

```text
只读取当前 cache
```

如不希望长期 timer，可以在已有 Native API SubscribeStatus / runtime telemetry 更新路径一起维护。

---

# 20. 统一 API Context 使用

当前如果存在：

```c
api_ctx_t *status_api = api ? api : &g_api_ctx;
```

则后续：

```text
status
version
mode
health
```

必须全部使用同一个 `status_api`。

不能部分 fallback 到 global，部分在 `api == NULL` 时直接 unknown。

但最终目标仍是：

> renderer 不直接拿 API context。

---

# 21. status Collector

建议接口：

```c
int status_collect_snapshot(
    atpd_context_t *ctx,
    status_snapshot_t *out
);
```

要求：

- 初始化整个 snapshot
- 所有字段都有默认 unknown/unavailable
- 某个子模块失败不使整体失败
- 收集过程不修改业务状态
- 非必要不得进行网络阻塞

---

# 22. Partial Failure 设计

例如：

```text
/proc/self/status OK
Native API cache stale
XFRM failed
eBPF unavailable
```

最终仍然输出完整 status：

```text
ATPD              HEALTHY
Native API        DEGRADED
XFRM              FAILED
eBPF              UNAVAILABLE
Overall           DEGRADED
```

不能因为一个模块异常：

```text
status command failed
```

导致最需要诊断的时候什么都看不到。

---

# 23. Health Aggregation

增加：

```c
typedef enum {
    HEALTH_OK,
    HEALTH_DEGRADED,
    HEALTH_FAILED,
    HEALTH_UNKNOWN
} health_state_t;
```

最终计算：

```text
Overall Health
```

示例：

```text
Overall            HEALTHY
```

或：

```text
Overall            DEGRADED
Reason             Native API snapshot stale
```

---

# 24. Health 规则建议

初始建议：

## FAILED

以下任一关键组件不可用：

```text
ATPD internal fatal state
required route control unavailable
required eBPF attach failed
sing-box permanently stopped when expected running
```

## DEGRADED

```text
Native API stale
XFRM watcher retrying
sing-box restart recently
optional telemetry unavailable
```

## HEALTHY

关键组件正常，允许 optional metric unknown。

---

# 25. Human Renderer

新增：

```c
void status_render_human(
    const status_snapshot_t *snapshot,
    FILE *out
);
```

renderer：

- 不访问 `/proc`
- 不访问 socket
- 不访问 Native API
- 不更新文件
- 不改变 global state

只负责：

```text
format
colors
tables
units
```

---

# 26. JSON Renderer

新增：

```c
int status_render_json(
    const status_snapshot_t *snapshot,
    FILE *out
);
```

CLI：

```bash
atpd status --json
```

示例：

```json
{
  "health": "healthy",
  "atpd": {
    "state": "running",
    "pid": 1234,
    "uptime_sec": 93210,
    "rss_kb": 2437,
    "hwm_kb": 2811,
    "fd_count": 11,
    "threads": 1
  },
  "singbox": {
    "state": "running",
    "pid": 5678,
    "native_api": "ready",
    "snapshot_age_ms": 218,
    "version": "1.14.x",
    "goroutines": 31
  },
  "network": {
    "netlink": "active",
    "xfrm": "active"
  },
  "ebpf": {
    "state": "attached"
  }
}
```

---

# 27. JSON 稳定性要求

JSON 将作为 machine-readable interface。

因此：

- key 不随 UI 文案变化
- number 必须保持 number
- unknown 使用 `null`
- boolean 使用 JSON boolean
- 不输出 ANSI color
- 不混入日志

例如：

```json
"rss_kb": null
```

而不是：

```json
"rss_kb": "unknown"
```

---

# 28. 可选 `--brief`

后续可增加：

```bash
atpd status --brief
```

输出：

```text
HEALTHY atpd=2.4MB fd=11 singbox=ready xfrm=active ebpf=attached
```

适合：

```text
service scripts
health checks
CI
```

优先级低于 `--json`。

---

# 29. CLI Exit Code

建议定义：

```text
0 = healthy
1 = degraded
2 = failed
```

如果担心破坏旧脚本兼容，第一阶段：

```text
status command 本身成功统一返回 0
```

同时 JSON/human 显示 health。

后续版本再明确 versioned exit code。

不要未经兼容性评估直接改变现有 exit code。

---

# 30. 推荐最终 Human Output

```text
ATP STATUS

ATPD DAEMON
  State              RUNNING
  PID                1234
  Uptime             2d 06:31:10
  RSS                2.38 MB
  Peak RSS           2.81 MB
  FDs                11
  Threads            1

PROXY CORE
  sing-box           RUNNING
  PID                5678
  Version            1.14.x
  Native API         READY
  Snapshot Age       218 ms
  Memory             42.8 MB
  Goroutines         31
  Connections        25 / 27

NETWORK CONTROL
  Netlink            ACTIVE
  XFRM               ACTIVE
  Interface           rmnet_data2
  VPN                 READY
  Clash Mode          Google VPN

eBPF
  Kernel Support      YES
  Program             ATTACHED
  Maps                HEALTHY
  Active Connections  27

SUPERVISOR
  Child               HEALTHY
  Restart Count       0
  Last Restart        never
  Last Exit           none

HEALTH
  Overall             HEALTHY
  Last Error          none
```

---

# 31. `status.c` 文件职责调整

重构后建议：

## `status.c`

只保留 public orchestration：

```text
parse status mode
collect snapshot
select renderer
```

目标控制在：

```text
< 200–300 lines
```

## `status_collect.c`

负责：

```text
ATPD
supervisor
network
eBPF
Native API cache
system
```

## `status_resource.c`

负责：

```text
/proc/self/status
/proc/self/fd
resource formatting helpers
```

## `status_render.c`

负责 human output。

## `status_json.c`

负责 JSON。

---

# 32. 不要机械重构

禁止：

```text
把 status.c 677 行切成几个文件
但数据流完全不变
```

真正目标是：

```text
data collection
   ↓
snapshot
   ↓
renderer
```

必须建立这一层边界。

---

# 33. 性能目标

正常：

```text
atpd status
```

目标：

```text
p50 < 10 ms
p95 < 25 ms
p99 < 50 ms
```

这是方向性目标，需要基于 CI/Android 真机结果调整。

异常场景：

```text
Native API unavailable
```

status 仍应：

```text
< 100–250 ms
```

绝不能因为 retry 阻塞数秒。

---

# 34. Status Stress Test

新增：

```text
tests/test_status_stress.sh
```

建议：

```bash
atpd status × 5000
```

检查：

```text
all responses valid
FD no growth
RSS no sustained growth
latency stable
no crash
```

可与现有 resource benchmark 合并。

---

# 35. Native API Failure Test

场景：

```text
start ATPD
start sing-box
verify status
kill/disable Native API
run status ×100
```

要求：

```text
status returns quickly
Native API = DEGRADED
other fields still available
no FD growth
no daemon crash
```

---

# 36. XFRM 状态测试

模拟：

```text
XFRM registration success
XFRM reactor_add_fd failure
XFRM socket failure
XFRM retry
```

status 必须分别显示：

```text
ACTIVE
FAILED
DEGRADED/RETRYING
```

不能统一 ACTIVE。

---

# 37. `/proc` Parser Test

为：

```text
VmRSS
VmHWM
VmSize
Threads
```

写 parser 单元测试。

测试：

```text
normal file
missing field
malformed number
very large value
empty file
```

不要让 status parser 因单行异常 crash。

---

# 38. JSON Test

新增 golden / semantic test。

检查：

```text
valid JSON
required fields present
unknown -> null
number remains number
health enum valid
no ANSI
```

如果项目已有 yyjson，可优先复用现有 yyjson。

避免手工大量：

```c
printf("{\"...")
```

拼接 JSON。

---

# 39. No Side Effect Test

执行：

```text
snapshot before
atpd status ×100
snapshot after
```

确保 status 不改变：

```text
traffic sample state
route
eBPF
supervisor
config
Native API state
```

允许：

```text
diagnostic counters
```

发生变化，但必须明确。

---

# 40. Resource Regression

status stress 后：

```text
RSS growth <= existing resource threshold
FD growth <= 1
Threads growth = 0
```

并结合：

```text
VmHWM
```

记录峰值。

---

# 41. Error Handling

所有 status helper 禁止：

```text
exit()
abort()
assert(external data)
```

诊断代码必须遵循：

```text
best effort
```

例如：

```c
if (read_rss(...) != 0) {
    snapshot->atpd.rss_kb = STATUS_VALUE_UNKNOWN;
}
```

而不是使整个命令失败。

---

# 42. 安全性

status 中如果展示：

```text
socket path
configuration
route information
process command line
```

不得泄露：

```text
credential
token
password
secret
private key
```

JSON 更容易被自动上传到 bug report，因此尤其需要过滤敏感数据。

---

# 43. 第一阶段实施范围

建议第一 PR 不追求全部完成。

## Phase 1 / P0

必须完成：

1. 引入 `status_snapshot_t`
2. 增加 ATPD PID / uptime / RSS / VmHWM / FD / Threads
3. Netlink/XFRM 独立真实状态
4. status 不再进行秒级 Native API retry
5. 修正 API context fallback 不一致
6. 修正 eBPF active_conns 标签语义
7. 增加 basic status stress test

---

# 44. 第二阶段

## Phase 2 / P1

完成：

1. Native API runtime cache
2. cache freshness
3. supervisor status
4. traffic sampling 移出 status query
5. health aggregation
6. collector/renderer 分离

---

# 45. 第三阶段

## Phase 3 / P2

完成：

1. `--json`
2. `--brief`
3. JSON tests
4. Android UI / automation 稳定接口
5. further diagnostics

---

# 46. 推荐 Commit 顺序

## Commit 1

```text
status: add unified snapshot model
```

## Commit 2

```text
status: expose ATPD resource metrics
```

## Commit 3

```text
status: report real Netlink/XFRM state
```

## Commit 4

```text
status: remove blocking Native API queries
```

## Commit 5

```text
status: move traffic sampling outside query path
```

## Commit 6

```text
status: split collector and human renderer
```

## Commit 7

```text
status: add JSON renderer
```

## Commit 8

```text
tests: add status stress and failure tests
```

---

# 47. Codex 开始前需要检查

Codex 在修改前必须阅读：

```text
src/status.c
include/status.h
src/singbox_api.c
include/singbox_api.h
src/netlink.c
include/netlink.h
src/service.c
include/service.h
src/reactor.c
src/ebpf.c
include/ebpf.h
src/uds.c
tests/benchmark_atpd.sh
```

并先输出：

```text
当前 status 数据来源映射
```

格式：

```text
字段
→ 来源函数
→ 是否阻塞
→ 是否有 side effect
→ 是否可信
→ 是否适合 cache
```

先理解再修改。

---

# 48. Codex 必须重点审计的调用

搜索：

```text
api_get_status_sync
api_get_version_sync
reactor_add_fd
netlink_get_fd
xfrm
active_conns
/proc
traffic
status_show
```

确认：

- status 调用链是否还有 sleep/retry
- 是否存在隐式文件写入
- 是否存在状态推断而非真实 state
- 是否存在重复 API query
- 是否存在动态 allocation 未释放

---

# 49. Acceptance Criteria

优化完成后必须满足：

## Responsiveness

```text
Native API down 时 status 不再等待数秒
```

## ATPD Resources

显示：

```text
PID
Uptime
RSS
VmHWM
FD
Threads
```

## Monitor Accuracy

```text
Netlink
XFRM
```

必须独立真实报告。

## Read-only

重复执行 status 不改变业务状态。

## Architecture

renderer 不执行 Native API / `/proc` / socket query。

## Stability

```text
status ×5000
```

无：

```text
FD leak
RSS 持续增长
crash
hang
```

## Compatibility

现有人类可读输出核心信息不应无理由消失。

## JSON

若 Phase 3 实施：

```text
atpd status --json
```

必须输出稳定合法 JSON。

---

# 50. 最终设计原则

`status` 的定位应该从：

> “用户执行时临时去查询各种东西并画一张表”

升级为：

> “读取 ATPD 已知的统一运行状态快照，并以 human 或 machine-readable 形式输出。”

最终数据流：

```text
ATPD runtime
  │
  ├── supervisor state
  ├── Native API cache
  ├── Netlink/XFRM state
  ├── eBPF telemetry
  ├── traffic telemetry
  └── self resource metrics
          │
          ▼
    status_snapshot_t
          │
     ┌────┴────┐
     ▼         ▼
   Human      JSON
```

这样可以同时获得：

- 更低 status latency
- 更可信的诊断结果
- 更好的故障定位能力
- 更好的 benchmark 稳定性
- 更好的 Android UI 接口
- 更容易做自动化测试
- 更容易在未来扩展 ATPD observability

最重要的一点：

> `atpd status` 必须在系统最异常的时候仍然快速、完整、可信地返回。

这应成为本次重构的最高优先级。
