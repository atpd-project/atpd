# ATPD Go 重构实施计划

## 1. 项目背景

项目：`atpd-project/atpd`

当前主线基础：`ebpf-native-api`

当前 ATPD 主要使用 C 实现，负责 Android/Linux root 环境下的系统级网络控制，包括但不限于：

- Netlink
- XFRM
- 系统路由 / policy routing
- eBPF / iptables / ip rule 编排
- UDS 控制接口
- sing-box 子进程生命周期管理
- Native API 交互
- 网络状态感知
- 自愈与恢复逻辑

sing-box 本身使用 Go 实现。

本计划的目标不是把 sing-box 嵌入 `libbox`，也不是把 ATPD 改造成普通 Android `VpnService` wrapper。

**目标架构仍然是：ATPD 作为独立 root control-plane daemon，sing-box 作为独立 worker/data-plane 进程。**

---

# 2. 重构目标

将 ATPD 从 C 重构为 Go，重点获得：

- 更安全的长期 daemon 生命周期管理
- 更简单的异步状态机
- 更可靠的 child-process supervision
- 更低的 FD / heap ownership 出错概率
- 更自然的 gRPC / Native API 集成
- 更容易维护的 Netlink/XFRM watcher
- 更好的测试能力
- 更快的后续开发速度
- 与 sing-box Go 生态更一致

但必须保留：

- root daemon 能力
- 系统级路由控制能力
- Netlink/XFRM
- eBPF / iptables / policy routing
- sing-box 独立进程
- ATPD 对 sing-box 的 supervisor 能力
- sing-box crash 后 ATPD 仍可独立恢复

---

# 3. 明确禁止的架构变化

本次 Go 重构禁止：

- 将 ATPD 变成 `libbox` wrapper
- 将 sing-box 嵌入 ATPD 同一进程
- 依赖 Android `VpnService` 替代 root 系统路由控制
- 删除 ATPD 对系统路由、XFRM、Netlink 的直接控制
- 为了重构而改变现有网络行为
- 在功能未完全对齐前删除 C 版
- 一次性全量替换生产代码
- 为了让测试通过而降低现有恢复能力

---

# 4. 推荐最终架构

```text
                 Android / Linux Kernel
                         │
          ┌──────────────┼──────────────┐
          │              │              │
       Netlink          XFRM        eBPF / Route
          │              │              │
          └──────────────┼──────────────┘
                         ▼
                      ATPD-Go
                  Root Control Plane
                         │
                gRPC over Unix Socket
                         │
                         ▼
                      sing-box
                   Worker / Data Plane
```

关键原则：

- ATPD-Go 和 sing-box 保持独立进程
- ATPD-Go 负责系统层网络控制
- sing-box 负责代理核心
- 两者通过明确 Native API / gRPC 边界交互
- 优先 Unix Domain Socket
- 不使用 gRPC-Web
- 不直接依赖 sing-box 内部非稳定 package 作为主控制面

---

# 5. 为什么继续保留双进程

必须保留以下故障隔离能力：

```text
sing-box crash
    ↓
ATPD-Go 仍存活
    ↓
检测退出
    ↓
清理 / 恢复必要状态
    ↓
重新启动 sing-box
```

反过来也必须保证：

```text
ATPD-Go restart
    ↓
可以重新识别/接管当前 sing-box
或安全重启 sing-box
```

不能因为双方都是 Go 就强行合并进程。

---

# 6. Go 版本建议

优先使用当前稳定 Go 工具链。

构建时记录：

```text
Go version
Git commit
ATPD version
Build timestamp
```

提供：

```bash
atpd --version
```

例如：

```text
atpd 0.9.0-dev
commit abcdef1
go1.xx.x
```

具体 Go 版本以实施时项目 CI 和 Android 交叉编译兼容性为准。

---

# 7. 推荐目录结构

建议新增：

```text
cmd/
└── atpd/
    └── main.go

internal/
├── app/
│   ├── app.go
│   └── lifecycle.go
│
├── config/
│   ├── config.go
│   ├── validate.go
│   └── reload.go
│
├── log/
│   └── log.go
│
├── supervisor/
│   ├── supervisor.go
│   ├── process.go
│   ├── restart.go
│   └── health.go
│
├── nativeapi/
│   ├── client.go
│   ├── health.go
│   └── status.go
│
├── ipc/
│   ├── server.go
│   ├── client.go
│   └── protocol.go
│
├── netlink/
│   ├── watcher.go
│   ├── route.go
│   └── link.go
│
├── xfrm/
│   └── watcher.go
│
├── routing/
│   ├── routing.go
│   ├── policy.go
│   └── cleanup.go
│
├── ebpf/
│   ├── loader.go
│   ├── maps.go
│   └── lifecycle.go
│
├── state/
│   ├── state.go
│   └── snapshot.go
│
└── platform/
    ├── linux.go
    └── android.go

tests/
├── integration/
├── stress/
├── soak/
└── parity/
```

如果现有目录结构已有明确约定，不要求机械照搬。

目标是：

- package 职责明确
- platform code 与业务状态机分离
- system side-effect 集中
- 可测试逻辑尽量纯函数化

---

# 8. Main 生命周期设计

`main.go` 只做：

```text
parse args
load config
init logger
init app
run
wait
shutdown
```

核心生命周期集中在：

```text
internal/app
```

推荐：

```go
ctx, cancel := signal.NotifyContext(
    context.Background(),
    syscall.SIGTERM,
    syscall.SIGINT,
)
defer cancel()

if err := app.Run(ctx); err != nil {
    ...
}
```

不要把大量状态逻辑写在 `main.go`。

---

# 9. Context 作为统一生命周期根

所有长期 goroutine 必须挂到根 `context.Context`。

推荐结构：

```text
root context
 ├── IPC server
 ├── Netlink watcher
 ├── XFRM watcher
 ├── sing-box supervisor
 ├── Native API health loop
 └── recovery/state manager
```

shutdown：

```text
cancel root context
 ↓
stop accepting new IPC
 ↓
stop watchers
 ↓
stop/reap sing-box
 ↓
cleanup route/eBPF state
 ↓
wait goroutines
 ↓
exit
```

禁止出现无法停止的后台 goroutine。

---

# 10. Goroutine Ownership 规则

每个 goroutine 必须明确：

- 谁启动
- 谁取消
- 谁等待退出
- 错误返回到哪里
- 是否允许 restart
- 是否可能 duplicate

建议使用：

```text
errgroup.Group
context.Context
sync.WaitGroup
```

不要使用不可追踪的：

```go
go func() { ... }()
```

然后完全不管理其退出。

---

# 11. sing-box Supervisor 重构

这是 Go 重构最优先模块之一。

目标：

```text
Start
Stop
Restart
Wait
Crash detection
Backoff
Health check
PID tracking
```

推荐直接使用：

```text
os/exec
context
time.Timer
```

而不是手工模拟 C 的 `SIGCHLD + waitpid` 状态机。

核心原则：

```go
cmd := exec.Command(...)
cmd.Start()
cmd.Wait()
```

必须保证：

- 每一个 `Start()` 成功的 child 最终都有对应 `Wait()`
- 不允许 zombie
- 不允许旧 child exit 覆盖新 child 状态
- restart 必须有 generation / PID identity
- shutdown 与 crash recovery 不互相 race

建议状态：

```text
Stopped
Starting
Running
Stopping
Backoff
Failed
```

---

# 12. Supervisor Generation ID

为避免旧 process 的 exit event 干扰新 process，建议每次启动生成：

```text
generation uint64
```

例如：

```text
child #41
PID 1001

restart

child #42
PID 1038
```

旧 child #41 的退出结果只能更新 #41。

不能：

```text
old Wait() returns
 ↓
把 current #42 标成 stopped
```

这是必须测试的 race。

---

# 13. Restart Backoff

建议实现有上限的指数退避。

例如：

```text
1s
2s
4s
8s
15s
30s
```

达到稳定运行窗口后 reset。

避免：

```text
sing-box config 错误
 ↓
ATPD 每 10ms 无限重启
 ↓
CPU / log storm
```

同时提供：

```text
restart_count
last_exit_reason
last_start_time
```

供 status 查看。

---

# 14. Native API / gRPC

ATPD-Go 与 sing-box 继续使用正式 Native API。

优先：

```text
gRPC over Unix Domain Socket
```

不使用 gRPC-Web。

要求：

- typed protobuf client
- 明确 dial timeout
- 每次 RPC 明确 timeout
- context cancellation
- reconnect
- health checking
- API unavailable 时不阻塞主 reactor

推荐：

```go
ctx, cancel := context.WithTimeout(parent, 2*time.Second)
defer cancel()
```

具体 timeout 根据实际设备测量调整。

---

# 15. Native API Client 状态

建议维护：

```text
Disconnected
Connecting
Ready
Degraded
```

并记录：

```text
last_success
last_error
consecutive_failures
```

不能把：

```text
gRPC dial 成功
```

简单等同于：

```text
sing-box healthy
```

至少需要一个轻量 health/status RPC。

---

# 16. Native API 断线恢复

必须覆盖：

```text
sing-box restart
socket disappear
socket recreated
temporary RPC timeout
API server late startup
```

ATPD-Go 不能因为一次 API dial 失败就永久进入 broken state。

恢复流程应可重复。

---

# 17. IPC / ATPD CLI

如果当前 `atpd status` 等命令通过 UDS，Go 版应尽量保持兼容。

优先保持：

```text
socket path
commands
output schema
exit code
```

如果必须改变协议：

- 在迁移期提供 compatibility layer
- 文档明确 breaking change
- parity tests 覆盖旧 CLI 行为

---

# 18. Go UDS Server

优先使用：

```text
net.UnixListener
net.UnixConn
```

每个连接：

- deadline
- 最大请求大小
- 最大并发
- 完整 write
- graceful close

建议：

```text
read deadline: 2–5s
write deadline: 2–5s
max clients: 32/64
```

避免 C 版曾经存在的：

```text
idle client FD exhaustion
partial send
EAGAIN handling
```

问题。

---

# 19. 配置模块

建议将配置处理拆成：

```text
Parse
Validate
Diff
Apply
Commit
Rollback
```

避免：

```text
先覆盖 live config
再 apply
失败后处于半更新状态
```

目标：

```text
old config
  ↓
parse new
  ↓
validate
  ↓
prepare
  ↓
apply runtime changes
  ↓
commit
```

失败：

```text
rollback
keep old config
```

---

# 20. Netlink

Go 版不应简单翻译 C callback。

建议重新抽象成：

```text
Netlink watcher
   ↓
normalized event
   ↓
state manager
```

例如：

```go
type LinkEvent struct {
    IfIndex int
    Name    string
    Up      bool
}
```

核心逻辑不应依赖 raw netlink buffer。

---

# 21. XFRM

XFRM watcher 与普通 route/link watcher分开。

要求：

- 初始化失败明确返回
- subscribe 失败可恢复
- fd/socket ownership 清晰
- context cancellation
- no silent registered state

状态至少：

```text
NotStarted
Listening
Retrying
Failed
```

不能出现：

```text
实际 subscribe 失败
但状态显示 listening
```

---

# 22. Routing / Policy Routing

系统路由操作必须集中到单独 package。

不要在：

```text
supervisor
API client
IPC handler
```

里直接散落执行：

```text
ip rule
ip route
iptables
```

建议统一接口：

```go
type Router interface {
    Apply(ctx context.Context, desired State) error
    Cleanup(ctx context.Context) error
    Snapshot(ctx context.Context) (Snapshot, error)
}
```

便于：

- test
- dry-run
- rollback
- parity comparison

---

# 23. Android 平台差异

必须单独考虑：

- toybox
- netd
- SELinux
- root shell
- Magisk
- KernelSU
- APatch
- Android 8–16
- iptables legacy/nft 差异
- kernel XFRM 差异
- vendor kernel

不要假设标准 desktop Linux 行为等同 Android。

---

# 24. eBPF

如果当前 eBPF program 本身用 C/BPF C 编译，不要求全部改 Go。

推荐：

```text
Go = control plane
BPF C = kernel program
```

Go 负责：

```text
load
attach
map access
lifecycle
cleanup
```

不要为了“全 Go”强行改写已稳定的 eBPF kernel code。

---

# 25. eBPF Loader

建议使用成熟 Go eBPF loader 方案。

实施时根据当前 ATPD BPF 构建方式评估：

```text
cilium/ebpf
libbpfgo
现有 bpftool/generated object
```

优先考虑：

- Android 构建兼容
- 静态依赖
- binary size
- kernel compatibility
- BTF availability

不要只根据开发机 convenience 选择。

---

# 26. State Manager

建议将系统状态集中管理。

例如：

```go
type State struct {
    VPNActive      bool
    XFRMActive     bool
    SingBoxRunning bool
    NativeAPIReady bool
    ActiveIfIndex  int
}
```

然后：

```text
event
 ↓
update desired state
 ↓
reconcile
```

而不是每个 watcher 直接修改系统。

---

# 27. Reconcile 模型

建议逐步引入：

```text
Observed State
Desired State
Reconcile()
```

例如：

```text
Wi-Fi up
VPN active
sing-box healthy
```

计算目标：

```text
which routes
which rules
which eBPF state
```

然后 apply。

这样比 callback 里直接执行 side effect 更容易保证一致性。

---

# 28. 错误分类

不要所有错误都：

```text
log error
return
```

建议区分：

```text
Transient
Permanent
Fatal
```

例如：

Transient：

```text
Native API temporary unavailable
Netlink socket transient error
```

Permanent：

```text
invalid config
unsupported kernel feature
```

Fatal：

```text
cannot initialize required root capability
corrupt internal state
```

对应：

```text
retry
degrade
exit
```

策略不同。

---

# 29. Logging

日志必须结构化且可追踪。

至少包含：

```text
component
event
pid
generation
error
duration
```

例如：

```text
component=supervisor
event=child_exit
pid=1234
generation=42
reason=signal
signal=9
```

方便 Android 真机定位问题。

---

# 30. Metrics / Status

Go 版 status 至少保留并增强：

```text
ATPD uptime
ATPD RSS
FD count
goroutine count
sing-box PID
sing-box restart count
Native API state
Netlink state
XFRM state
current interface
VPN state
last error
```

不要因为 Go 重构而丢掉现有可观测性。

---

# 31. Go 自身资源目标

不能沿用 C 版：

```text
RSS < 3 MB
```

作为硬门槛。

Go 版资源目标应通过实际 prototype 测量后确定。

建议第一阶段：

```text
idle RSS
peak RSS
recovery RSS
goroutine count
FD count
CPU idle
binary size
```

重点不是追求绝对极低，而是：

```text
长期稳定
无持续增长
无泄漏
CPU idle 接近 0
```

---

# 32. 初始建议资源验收

第一阶段可设软目标：

```text
RSS idle <= 15 MB
FD recovery growth <= 1
goroutine recovery growth <= 2
CPU idle <= 0.5%
```

以上仅作为 prototype 初始目标，不应未经测量直接设为最终 release gate。

正式阈值必须根据：

```text
Pixel/AOSP
Xiaomi
Samsung/OnePlus
不同 Android 版本
```

实测制定。

---

# 33. C 版作为 Reference Implementation

Go 重构期间，C 版不能立即删除。

它承担：

```text
Behavior Reference
Regression Oracle
Fallback
```

任何 Go 行为不确定时：

```text
同设备
同配置
同事件
```

比较 C 与 Go 的：

```text
routes
rules
eBPF state
sing-box state
status
recovery behavior
```

---

# 34. Parity Test

新增：

```text
tests/parity/
```

目标：

同一个 scenario：

```text
C ATPD
Go ATPD
```

分别运行。

比较：

```text
exit code
status output
route table
ip rule
iptables/eBPF state
sing-box PID behavior
recovery timing
```

允许内部实现不同。

要求 observable behavior 一致。

---

# 35. Phase 0 — 冻结现有 C 行为

在 Go 开发前先完成：

- 当前 C P0/P1 缺陷修复
- resource stress
- restart/reload tests
- Android soak baseline
- 行为文档

必须记录：

```text
哪些行为是 intended
哪些只是历史偶然行为
```

避免 Go 版复制 C bug。

---

# 36. Phase 1 — Go Skeleton

先建立：

```text
Go module
cmd/atpd
config
logging
signal handling
graceful shutdown
build
CI
```

不操作系统路由。

验收：

```text
start
status
shutdown
no goroutine leak
```

---

# 37. Phase 2 — sing-box Supervisor

实现：

```text
Start
Stop
Restart
Crash recovery
PID/state
Backoff
```

先不迁移 Netlink/eBPF。

要求通过：

```text
restart ×100
SIGTERM ×100
SIGKILL ×100
```

无 zombie。

---

# 38. Phase 3 — Native API

加入：

```text
gRPC client
UDS
health
status
reconnect
```

验证：

```text
sing-box starts late
socket disappears
sing-box restart
API timeout
```

都能恢复。

---

# 39. Phase 4 — ATPD IPC / CLI

迁移：

```text
atpd status
reload
restart
stop
```

保持兼容。

加入 UDS stress：

```text
2000 requests
idle clients
slow clients
partial writes
```

---

# 40. Phase 5 — Netlink / XFRM

迁移：

```text
interface event
route event
XFRM event
VPN state
```

与 C 版 parity。

重点：

```text
Wi-Fi ↔ cellular
VPN connect/disconnect
interface flap
```

---

# 41. Phase 6 — Routing

迁移：

```text
ip rule
route
redirect/tproxy behavior
UID rules
hotspot behavior
```

必须使用真机和 namespace 测试。

---

# 42. Phase 7 — eBPF Control Plane

迁移：

```text
load
attach
map
detach
cleanup
recovery
```

不要求重写 BPF C program。

---

# 43. Phase 8 — Self-Healing

迁移：

```text
sing-box crash
Native API loss
route loss
XFRM transition
interface change
```

目标：

Go 版达到或超过 C 版恢复能力。

---

# 44. Phase 9 — Long Soak

至少：

```text
24 hours
```

真机混合场景：

```text
idle
screen on/off
Wi-Fi/5G switch
airplane mode
VPN on/off
hotspot
sing-box kill
ATPD restart
reload
network loss/recovery
```

---

# 45. Failure Injection

Go 版必须支持 test-only failure injection。

模拟：

```text
Native API timeout
child Start failure
child crash
Netlink init failure
XFRM subscribe failure
route apply failure
eBPF attach failure
config apply failure
```

要求：

```text
no resource leak
state remains consistent
retry/rollback correct
```

---

# 46. Go Race Detector

CI 增加：

```bash
go test -race ./...
```

如果 Android cross build 不支持 race，不影响。

至少 Linux host 单元/集成测试必须跑 race。

目标：

```text
0 race detected
```

---

# 47. Go Vet / Static Analysis

至少：

```bash
go vet ./...
```

建议加入：

```text
staticcheck
```

若项目允许。

禁止新增明显：

```text
context leak
copylock
unchecked error
goroutine leak
```

---

# 48. 单元测试

重点测试：

```text
config parse/validate
state reconcile
restart backoff
generation logic
error classification
route diff
status formatting
```

纯逻辑尽量做到高覆盖。

---

# 49. 集成测试

需要真实：

```text
Unix socket
child process
Netlink namespace
route namespace
```

可在 Linux CI 中尽量覆盖。

Android-specific 保留 self-hosted / real-device test。

---

# 50. Android 真机矩阵

正式切换前建议至少覆盖：

```text
Android 10/11
Android 12/13
Android 14
Android 15/16
```

Root 方案至少覆盖两类：

```text
Magisk
KernelSU / APatch
```

设备至少：

```text
AOSP/Pixel 类
Xiaomi
Samsung/OnePlus 中至少一种
```

具体矩阵按现有设备资源调整。

---

# 51. 性能对比

Go 与 C 版需要对比：

```text
startup time
status latency
idle RSS
peak RSS
FD
CPU idle
network event reaction latency
restart recovery time
```

不要求 Go 全面优于 C。

必须保证：

```text
性能没有不可接受 regression
```

---

# 52. Native API 延迟

重点测：

```text
gRPC UDS call latency
```

例如：

```text
2000 status calls
```

记录：

```text
p50
p95
p99
```

避免只看平均值。

---

# 53. Shutdown 验收

收到 SIGTERM：

```text
stop new IPC
cancel watchers
stop sing-box
reap child
cleanup routes/eBPF
close sockets
exit
```

必须设置总体 shutdown deadline。

例如：

```text
5–10 秒
```

具体通过真机确定。

超时进入明确 forced cleanup。

---

# 54. Panic Policy

长期 root daemon 不应因为普通可恢复错误 panic。

panic 只用于：

```text
programming invariant violation
```

建议 top-level recovery 记录 stack，但是否继续运行要谨慎。

不要用 recover 掩盖 corruption。

---

# 55. File Descriptor 管理

虽然 Go 有 GC，也不能依赖 GC 自动关闭 FD。

必须显式：

```go
defer conn.Close()
defer file.Close()
```

长期 resource 必须 ownership 明确。

测试继续监控：

```text
/proc/PID/fd
```

---

# 56. Goroutine Leak 测试

每轮：

```text
start
stress
recovery
```

记录：

```text
runtime.NumGoroutine()
```

例如：

```text
baseline 12
peak 30
recovery 13
```

不能每轮：

```text
+1
+1
+1
```

持续增长。

---

# 57. Heap / GC

建议 resource test 记录：

```text
runtime.MemStats
HeapAlloc
HeapSys
HeapObjects
NumGC
```

同时记录系统：

```text
RSS
PSS
VmHWM
```

不要只看 Go heap。

---

# 58. pprof

建议 debug/test build 支持：

```text
pprof
```

但 production 默认不要暴露 TCP debug port。

如果需要 production diagnostics：

优先：

```text
localhost / UDS
显式开启
权限保护
```

---

# 59. Build Tags

建议区分：

```text
linux
android
testing
```

平台代码使用：

```go
//go:build linux
```

等方式隔离。

避免大量 runtime.GOOS if/else 散落。

---

# 60. Cross Compile

必须验证：

```text
GOOS=linux
GOARCH=arm64
```

以及项目实际支持 ABI。

Android root binary 本质上运行于 Linux kernel userspace，但 libc/syscall/loader 兼容仍需真机验证。

如果使用 CGO：

必须明确说明原因。

优先：

```text
CGO_ENABLED=0
```

除非 eBPF / native dependency 强制要求。

---

# 61. CGO 原则

如果可以纯 Go：

```text
优先 pure Go
```

CGO 会增加：

- cross compile 复杂度
- Android ABI 风险
- toolchain 依赖
- crash surface

如果必须 CGO，必须单独评审。

---

# 62. Dependency 原则

Go 重构不要引入大量第三方库。

核心优先标准库。

第三方重点只考虑：

```text
gRPC/protobuf
eBPF
Netlink
```

每个 dependency 要评估：

```text
维护状态
license
Android compatibility
binary size
API stability
```

---

# 63. 不建议直接 import sing-box internal package

除非正式公开 API 明确允许，否则不要建立：

```text
ATPD-Go → sing-box/internal/*
```

这样的强依赖。

风险：

```text
sing-box upgrade
 ↓
ATPD compile break
```

优先使用：

```text
protobuf / gRPC API
```

作为稳定边界。

---

# 64. 版本策略

Go 重构期间建议：

```text
0.9.x-dev
```

不要直接宣布 `2.0.0`。

推荐：

```text
0.9.0-go.alpha.1
0.9.0-go.beta.1
1.0.0-rc.1
1.0.0
```

如果项目最终决定 Go 版是第一个正式公开稳定版：

```text
v1.0.0
```

完全合理。

---

# 65. Branch 策略

不要立即覆盖当前主线。

建议：

```text
main                 C 稳定基线
rewrite/go           Go 重构集成分支
feat/go-*            临时模块开发
```

每个模块完成：

```text
feat/go-supervisor
 ↓
PR
 ↓
rewrite/go
```

Go 版达到切换标准后：

```text
rewrite/go
 ↓
main
```

然后再删除旧 C implementation。

---

# 66. C 版删除条件

以下全部满足前禁止删除：

```text
功能 parity
resource tests
race detector
24h soak
real-device matrix
upgrade test
rollback/failure test
Native API recovery
routing parity
eBPF parity
```

并且至少保留一个 tag：

```text
archive/c-atpd-final
```

---

# 67. 最终切换 Gate

Go 版成为 `main` 前必须全部 PASS：

### Functional

- CLI/API parity
- route behavior parity
- VPN/XFRM parity
- eBPF parity
- sing-box supervision parity

### Stability

- restart ×100
- forced SIGKILL ×100
- reload ×100
- Netlink storm
- UDS storm
- Native API reconnect storm

### Resources

- 无 FD 持续增长
- 无 goroutine 持续增长
- RSS 稳定
- CPU idle 可接受

### Tooling

- `go test ./...`
- `go test -race ./...`
- `go vet ./...`
- staticcheck（若启用）

### Android

- 多版本真机
- 24h soak
- Wi-Fi/5G/VPN/热点
- Root 环境验证

---

# 68. Rollback Plan

Go 版首次 release 必须能回滚到 C 版。

不能修改持久状态格式到不可逆。

如果修改：

```text
config
pid file
state file
socket path
```

必须有 migration 设计。

---

# 69. 推荐 Commit 顺序

## Commit 1

```text
Go module + skeleton + CI
```

## Commit 2

```text
config/logging/lifecycle
```

## Commit 3

```text
sing-box supervisor
```

## Commit 4

```text
Native API gRPC/UDS client
```

## Commit 5

```text
ATPD IPC/CLI
```

## Commit 6

```text
Netlink/XFRM
```

## Commit 7

```text
routing/policy routing
```

## Commit 8

```text
eBPF control plane
```

## Commit 9

```text
self-healing
```

## Commit 10

```text
parity/stress/soak
```

---

# 70. Codex 实施要求

Codex 开始前必须先阅读：

```text
README.md
Makefile
versions.env
src/
include/
tests/
.github/workflows/
docs/
```

并输出当前 C 模块映射。

例如：

```text
src/service.c
→ internal/supervisor

src/reactor.c
→ context/goroutine/runtime model

src/uds.c
→ internal/ipc

src/netlink.c
→ internal/netlink + internal/xfrm
```

不要机械逐文件翻译。

目标是按职责重新设计。

---

# 71. 禁止机械 C→Go 翻译

例如 C：

```text
epoll callback
timer callback
global state
```

不要一比一翻译成：

```text
goroutine callback
timer callback
global state
```

Go 版应使用：

```text
context
channels
typed state
explicit ownership
reconcile
```

重新建模。

---

# 72. 迁移原则

每个模块遵循：

```text
理解当前行为
 ↓
写 parity test
 ↓
实现 Go
 ↓
跑 C/Go 对比
 ↓
stress
 ↓
merge
```

不是：

```text
先删 C
再猜行为
```

---

# 73. 最终 Codex 交付报告

每个 Phase 完成后输出：

1. 实现模块
2. 与 C 版映射
3. 行为差异
4. 新增 dependency
5. 单元测试结果
6. race test
7. stress result
8. resource result
9. Android result
10. 尚未迁移模块
11. 已知风险
12. 下一阶段建议

最终切换前额外输出：

```text
C vs Go parity matrix
resource comparison
24h soak summary
device matrix
rollback plan
release recommendation
```

---

# 74. 最终设计原则

ATPD-Go 的目标不是“因为 sing-box 是 Go，所以 ATPD 也用 Go”。

真正目标是：

> 使用 Go 降低 ATPD 作为长期 root networking daemon 的生命周期复杂度，同时继续保持 ATPD 与 sing-box 的进程隔离，以及 ATPD 对 Android/Linux 系统路由、Netlink、XFRM 和 eBPF 的完整控制权。

最终目标架构：

```text
ATPD-Go
  = root system controller
  = network state reconciler
  = sing-box supervisor
  = Native API client

sing-box
  = independent proxy engine
  = data plane

IPC
  = typed gRPC
  = Unix Domain Socket
```

只有在 Go 版完成：

```text
功能等价
资源稳定
真机验证
异常恢复
长期 soak
```

之后，才允许替换 C 版成为正式主线。
