# ATPD eBPF 模块移除与职责收敛方案

## 1. 决策

当前 ATPD 架构中，sing-box 已原生提供并负责 `ebpf-in`。

因此 ATPD 不再负责：

- eBPF capability probe
- BPF syscall 探测
- program/map 加载
- attach/detach
- eBPF datapath 生命周期
- sing-box eBPF 内部状态推断

目标是安全删除 ATPD 中已经重复或失去职责的 eBPF 层，而不是继续增强它。

最终 ownership：

```text
ATPD
├─ sing-box process supervision
├─ Netlink / XFRM
├─ system routing/control
├─ UDS
├─ session
├─ config
└─ Native API observability

sing-box
└─ ebpf-in
   ├─ capability detection
   ├─ BPF program/map lifecycle
   ├─ attach/detach
   ├─ datapath
   └─ eBPF runtime errors
```

核心原则：

> ATPD 不应重复实现 sing-box 已经拥有的 eBPF capability 和 lifecycle。

---

## 2. 为什么应该删除，而不是继续加固

重复 capability probe 会产生两个“真相来源”：

```text
ATPD probe result
vs
sing-box ebpf-in actual startup result
```

两者可能因为：

- 权限
- seccomp
- SELinux
- kernel feature
- memlock
- program type
- map type
- sing-box 自身 fallback

而产生不同结果。

真正决定 datapath 能否工作的只有：

```text
sing-box ebpf-in 实际初始化结果
```

因此 ATPD 自己 probe 的价值有限，反而可能产生假阳性/假阴性。

---

## 3. 本次目标

Codex 应：

1. 全仓审计 ATPD eBPF API 和调用点。
2. 确认 `ebpf.c` / `ebpf_common.c` 是否还有独立于 sing-box 的真实职责。
3. 删除重复 capability probe。
4. 删除假 telemetry。
5. 删除已经无意义的 eBPF runtime config。
6. 清理 include / Makefile / tests / status。
7. 保证 ATPD 不再直接执行 BPF syscall。
8. 保证 sing-box `ebpf-in` 仍然由 sing-box 配置和生命周期完整管理。
9. 保证删除后所有测试通过。
10. 保证不存在兼容性死接口。

---

# 4. 第一阶段：全仓调用图

Codex 不要直接删除文件。

首先搜索：

```text
ebpf_
EBPF_
ENABLE_EBPF
ebpf.c
ebpf_common
ebpf.h
ebpf_common.h
BPF_
bpf(
__NR_bpf
SYS_bpf
RLIMIT_MEMLOCK
```

输出完整表格：

```text
symbol
definition
caller
purpose
still needed?
replacement
delete?
```

---

# 5. 必须重点审计的文件

至少：

```text
src/ebpf.c
src/ebpf_common.c

include/ebpf.h
include/ebpf_common.h

src/config.c
src/config_validator.c
src/status.c
src/atpd_init.c
src/atpd_context.c
src/main.c
src/service.c
src/singbox_api.c

Makefile
tests/
README.md
docs/
examples/
```

并继续全仓 grep，不能只检查上述文件。

---

# 6. `ebpf.c`

如果确认当前只包含：

```text
capability probe
support detection
pseudo telemetry
sing-box process related helper
```

则整个：

```text
src/ebpf.c
```

应删除。

不要保留一个空壳模块。

---

# 7. `ebpf_common.c`

需要单独判断。

如果它只服务 ATPD 自己已经废弃的 eBPF probe/map ABI：

```text
删除
```

如果其中存在真正通用、仍被其他模块使用的 helper：

```text
移动到正确 owner
```

不要仅因为名字叫 common 就保留。

---

# 8. `ebpf.h`

如果所有 API 都失去调用者：

```text
删除
```

不要保留：

```text
ebpf_probe()
ebpf_is_supported()
ebpf_get_stats()
```

作为“以后可能有用”的死 API。

---

# 9. `ebpf_common.h`

重点检查是否暴露：

```text
BPF map key/value struct
sing-box internal eBPF ABI
program/map constants
```

如果 ATPD 不再直接访问 sing-box BPF map：

全部删除。

ATPD 不应绑定 sing-box `ebpf-in` 的内部 map ABI。

---

# 10. ATPD 不再调用 BPF syscall

最终全仓应满足：

```text
grep BPF_MAP_CREATE
grep BPF_PROG_LOAD
grep __NR_bpf
grep SYS_bpf
```

ATPD 自有源码中：

```text
0 个实际调用
```

vendored/third-party 代码另行注明。

---

# 11. 删除 `RLIMIT_MEMLOCK` 副作用

如果当前 ATPD eBPF probe 会：

```text
setrlimit(RLIMIT_MEMLOCK)
```

删除该逻辑。

ATPD 不应该为了探测 sing-box 的能力修改自身进程 resource limit。

sing-box 自己需要什么资源限制，应由：

```text
sing-box
service spawn policy
platform
```

负责。

---

# 12. eBPF capability 不再是 ATPD startup prerequisite

删除类似：

```text
ATPD startup
→ ebpf_probe()
→ unsupported
→ ATPD degraded/fail
```

ATPD 的职责是监督 sing-box。

真正的判断应该是：

```text
sing-box starts
→ ebpf-in initializes
→ sing-box reports success/failure
```

---

# 13. `atpd_init.c`

删除 ATPD eBPF probe/init 阶段。

启动流程不再包含：

```text
ebpf_init
ebpf_probe
ebpf capability validation
```

除非全仓审计证明其中某项并非为 sing-box `ebpf-in` 服务。

---

# 14. `cleanup.c`

删除：

```text
ebpf_cleanup
```

如果 ATPD 本身不拥有任何 BPF object。

原则：

> 不拥有的资源，不负责 cleanup。

---

# 15. `atpd_context`

如果 context 中存在：

```text
ebpf_supported
ebpf_ready
ebpf_fd
ebpf_stats
```

且都来自 ATPD 自己的旧 probe：

删除。

不要保留失真的 runtime state。

---

# 16. 配置：`ENABLE_EBPF`

必须判断它到底控制谁。

如果：

```text
ENABLE_EBPF
```

只是控制 ATPD 自己旧 probe：

```text
删除
```

如果它实际上用于生成/控制 sing-box `ebpf-in` 配置：

不要直接删除配置能力，而应该重命名/迁移到真正语义。

例如：

```text
SINGBOX_EBPF_IN_ENABLED
```

但只有 ATPD 确实负责生成 sing-box config 时才需要。

---

# 17. 不要维护两套 enable switch

避免：

```text
ATPD ENABLE_EBPF=1
sing-box ebpf-in disabled
```

或者反过来。

最终应只有一个 authoritative source：

```text
sing-box configuration
```

---

# 18. Config reload

之前 `config_apply_deltas()` 中存在类似：

```text
if ebpf enabled
→ ebpf_probe()
```

删除。

配置 reload 不再调用 ATPD eBPF capability probe。

---

# 19. Config validator

如果 ATPD config 中不再存在 eBPF字段：

删除对应 validator。

如果字段只是透传/生成 sing-box config：

validator 应验证配置语义，而不是执行 kernel probe。

保持：

```text
validation = pure
```

---

# 20. Status

删除 ATPD 自己生成的：

```text
eBPF supported
eBPF active connections
eBPF map state
```

如果这些值并不来自 sing-box authoritative runtime。

尤其不要继续展示假 telemetry。

---

# 21. `active_conns` 必须审计

如果当前所谓：

```text
eBPF active_conns
```

实际上通过：

```text
/proc/<sing-box>/fd
```

统计 sing-box FD：

必须删除或重新命名。

FD 数：

```text
!=
eBPF active connections
```

这种指标不能继续出现在 production status。

---

# 22. 如果需要 sing-box connection 数

应该使用：

```text
Native API SubscribeStatus
```

已有：

```text
connections
```

telemetry。

因此：

```text
sing-box connection count
```

应来自：

```text
singbox_api_snapshot
```

而不是 eBPF 模块。

---

# 23. 如果需要 sing-box 内存

同样：

```text
Native API status memory
```

不要从 eBPF 模块旁路推断。

---

# 24. eBPF runtime health 应该从哪里来

优先级：

```text
1. sing-box Native API 明确提供 ebpf-in health
2. sing-box structured log/error
3. service readiness/failure
4. 不显示细粒度 eBPF health
```

不要为了 status 好看重新加入 ATPD BPF syscall。

---

# 25. 如果 Native API 暂时没有 ebpf-in 专用状态

那 status 可以诚实显示：

```text
sing-box        RUNNING
Native API      HEALTHY
```

而不要编造：

```text
eBPF             ACTIVE
```

如果没有 authoritative signal：

```text
不显示
```

比假状态更好。

---

# 26. Service ownership

如果 sing-box 因 `ebpf-in` 初始化失败而退出：

```text
service supervisor
```

负责：

```text
child exit detection
restart/backoff
circuit breaker
last error
```

不是 `ebpf.c`。

---

# 27. Native API ownership

如果 sing-box 正常运行并能提供 datapath telemetry：

```text
singbox_api.c
```

负责缓存。

不是 `ebpf.c`。

---

# 28. Netlink/XFRM ownership

系统 VPN/network state：

```text
netlink.c
```

负责。

不要因为删除 `ebpf.c` 就把这些逻辑搬进 eBPF替代模块。

---

# 29. 不创建新的 `ebpf_manager.c`

本次目标是删除重复层。

不要删除：

```text
ebpf.c
```

后又创建：

```text
ebpf_manager.c
ebpf_monitor.c
ebpf_probe.c
```

这违背架构目标。

---

# 30. `ebpf_is_pure_mode()` 等历史接口

如果存在：

```text
永远 true
永远 false
固定值
```

的兼容 API：

全仓检查 caller。

无 caller：

```text
删除
```

有 caller：

```text
把 caller 改成当前真实 architecture
```

然后删除 API。

---

# 31. Build system

从：

```text
Makefile
```

删除：

```text
src/ebpf.c
src/ebpf_common.c
```

如果文件确认删除。

同时检查：

```text
object lists
dependencies
header install
test targets
```

---

# 32. Compiler 验证

删除后必须：

```text
make clean
make
```

并开启：

```text
-Wall
-Wextra
-Werror
```

目标：

```text
0 implicit declaration
0 unused compatibility wrapper
0 stale include
```

---

# 33. Tests

删除仅验证 ATPD 自己 BPF probe 的测试。

不要为了“保持测试数量”保留无意义测试。

---

# 34. 替换成 architecture tests

真正应该测试：

```text
sing-box ebpf-in config存在
↓
ATPD启动 sing-box
↓
service health正常
↓
Native API正常
```

以及：

```text
sing-box ebpf-in失败
↓
ATPD能正确观察 child failure
↓
restart/backoff/circuit breaker正常
```

---

# 35. 不要 mock ATPD 自己的 BPF support

既然 ATPD不再拥有 eBPF：

不需要：

```text
mock BPF_MAP_CREATE
mock BPF_PROG_LOAD
```

这属于 sing-box 自己的测试范围。

---

# 36. Test：无 BPF syscall

CI 可增加静态检查：

```bash
grep -R "BPF_MAP_CREATE\|BPF_PROG_LOAD\|SYS_bpf\|__NR_bpf" src include
```

预期：

```text
无 ATPD-owned eBPF syscall
```

如果有合法例外：

明确 allowlist。

---

# 37. Test：无 eBPF旧 API

CI 搜索：

```text
ebpf_probe
ebpf_cleanup
ebpf_is_supported
ebpf_is_pure_mode
```

删除后应：

```text
0 references
```

---

# 38. Test：status 不再输出假指标

验证：

```text
ATPD status
```

不包含：

```text
eBPF active_conns
```

除非数据来自 authoritative sing-box API 且字段重新定义。

---

# 39. Test：config兼容

如果旧配置存在：

```text
ENABLE_EBPF=1
```

必须决定迁移策略。

### 如果项目还未 Stable

推荐：

```text
直接删除
unknown key WARN
```

并更新 example/docs。

### 如果需要兼容旧用户

可以临时：

```text
accept but deprecated/ignored
```

打印一次：

```text
ENABLE_EBPF is deprecated; sing-box owns ebpf-in
```

不要长期保留。

---

# 40. 由于当前目标是未来主分支

`ebpf-native-api` 将成为新的 baseline。

如果项目尚未承诺稳定 ABI/config compatibility：

推荐直接清理旧 eBPF配置和 API。

不要为了历史个人版本长期背兼容债务。

---

# 41. README

更新 architecture。

不要写成：

```text
ATPD eBPF interception engine
```

如果真正 owner 是 sing-box。

建议：

```text
ATPD supervises sing-box and coordinates system networking.
Traffic interception is provided by sing-box ebpf-in.
```

---

# 42. Architecture 文档

推荐：

```text
Android/Linux Kernel
        │
        ├── Netlink/XFRM ← ATPD
        │
        └── eBPF datapath ← sing-box ebpf-in
                            ↑
                            │
ATPD ── process supervision/API ── sing-box
```

清楚区分 ownership。

---

# 43. 不要把“ATPD使用 eBPF”写成“ATPD实现 eBPF”

产品层面可以说：

```text
ATPD + sing-box solution uses eBPF
```

源码架构层面必须说：

```text
sing-box owns eBPF datapath
```

---

# 44. Manifest / examples

检查：

```text
manifest.md
examples/
android/
scripts/
service.d/
```

是否还存在：

```text
ENABLE_EBPF
ebpf probe
BPF capability
ATPD-owned BPF
```

全部同步更新。

---

# 45. Shell scripts

搜索：

```text
bpftool
/sys/fs/bpf
BPF
ebpf
```

区分：

### ATPD runtime dependency

删除。

### debug/diagnose helper

如果只是用户诊断 sing-box eBPF：

可以保留，但命名和说明必须写：

```text
sing-box ebpf-in diagnostics
```

而不是 ATPD eBPF subsystem。

---

# 46. Android packaging

确认没有：

```text
额外 CAP_BPF
额外 memlock setup
ATPD-specific bpffs mount
```

仅为旧 ATPD eBPF模块存在。

如果 sing-box自己仍需要：

不能盲删。

必须确认是谁依赖。

---

# 47. SELinux / root policy

同样：

如果某些规则只允许 ATPD执行：

```text
bpf syscall
```

而 ATPD不再需要：

可删除。

如果 sing-box需要：

规则应作用于正确 process/domain。

---

# 48. Security收益

删除 ATPD BPF syscall后：

ATPD本身的 kernel attack surface进一步减少。

职责更清楚：

```text
ATPD:
control plane

sing-box:
data plane
```

这是好事。

---

# 49. Resource收益

可以删除：

```text
probe socket/syscall
memlock side effect
probe cache
BPF-specific globals
pseudo stats
```

代码和 runtime state都会减少。

---

# 50. 可维护性收益

以后 sing-box升级 ebpf-in：

ATPD不需要同步：

```text
map ABI
program type
feature probe
kernel compatibility matrix
```

这些都由 sing-box upstream负责。

---

# 51. `ebpf_common.c` 特别注意

不要因为里面代码“看起来有用”就留下。

判断标准只有一个：

> ATPD在不理解 sing-box内部 eBPF实现的情况下，是否仍需要这个函数？

如果答案：

```text
no
```

删除。

---

# 52. 不要读取 sing-box pinned BPF maps

除非未来有非常明确的需求。

否则 ATPD不要：

```text
open pinned map
lookup map
walk map
infer sessions
```

因为这会重新形成内部 ABI coupling。

优先 Native API。

---

# 53. 不要依赖 sing-box eBPF object names

同理不要：

```text
grep bpffs object
根据 program name判断 ready
```

这些都是内部实现细节。

---

# 54. `status` 的最终层次

推荐：

```text
ATPD
  Service        RUNNING
  Native API     HEALTHY
  Netlink        ACTIVE
  XFRM           ACTIVE/DEGRADED
  Sessions       N

sing-box
  Version
  Memory
  Goroutines
  Connections
  Traffic
```

如果未来 Native API提供：

```text
ebpf-in health
```

再增加：

```text
  eBPF-in        ACTIVE
```

并明确属于 sing-box。

---

# 55. Config最终层次

ATPD配置只保存 ATPD真正拥有的东西。

sing-box datapath配置：

```text
sing-box config
```

如果 ATPD负责生成 sing-box config：

则 ATPD只做：

```text
configuration orchestration
```

不做 kernel capability implementation。

---

# 56. 推荐 Commit 1

```text
ebpf: audit ownership and remove fake telemetry
```

内容：

- 完整调用图
- 删除 active_conns 等错误指标
- status调整
- 不改变核心 build

---

# 57. Commit 2

```text
ebpf: remove ATPD capability probing
```

内容：

- 删除 BPF syscall
- 删除 memlock probe side effect
- 删除 capability cache
- 删除 init/cleanup hooks

---

# 58. Commit 3

```text
ebpf: remove obsolete config and compatibility APIs
```

内容：

- ENABLE_EBPF policy
- config validator
- config reload hook
- pure-mode/no-op APIs

---

# 59. Commit 4

```text
ebpf: remove obsolete source and headers
```

如果确认没有剩余职责：

```text
delete src/ebpf.c
delete src/ebpf_common.c
delete include/ebpf.h
delete include/ebpf_common.h
```

并更新 Makefile。

---

# 60. Commit 5

```text
docs: document sing-box ownership of ebpf-in
```

更新：

```text
README
manifest
docs
examples
architecture
```

---

# 61. Commit 6

```text
tests: validate ebpf ownership boundary
```

内容：

- no ATPD BPF syscall
- no stale API
- config migration
- status semantics
- sing-box ebpf-in integration

---

# 62. 删除文件的验收条件

只有满足：

```text
0 external caller
0 config dependency
0 status dependency
0 cleanup dependency
0 build dependency
```

才能删除对应 source/header。

不要因为目标是删除就跳过 dependency audit。

---

# 63. Integration 验收：正常 ebpf-in

真实设备：

```text
sing-box ebpf-in enabled
ATPD starts sing-box
```

验证：

```text
traffic interception正常
ATPD service supervision正常
Native API正常
Netlink/XFRM正常
```

删除 ATPD `ebpf.c` 不应改变 datapath。

---

# 64. Integration 验收：ebpf-in启动失败

故意提供不满足条件的 sing-box配置/环境。

验证：

```text
sing-box失败或报告错误
ATPD service层正确观察
restart/backoff/circuit breaker正确
```

不需要 ATPD提前自己 probe。

---

# 65. Integration 验收：网络切换

```text
Wi-Fi → cellular
cellular → Wi-Fi
VPN/IPsec变化
```

验证：

```text
Netlink/XFRM仍正常
```

证明这些功能没有错误依赖 ATPD eBPF模块。

---

# 66. Integration 验收：status

eBPF模块删除后：

```text
atpd status
```

仍然：

```text
快速
准确
无假 eBPF telemetry
```

---

# 67. Resource验收

删除前后比较：

```text
binary size
baseline RSS
FD
startup syscalls
```

理论上不应恶化。

最好有小幅下降。

---

# 68. 最终 grep 验收

Codex最终报告：

```text
grep results
```

确认：

```text
BPF_MAP_CREATE
BPF_PROG_LOAD
SYS_bpf
__NR_bpf
ebpf_probe
ebpf_cleanup
```

在 ATPD-owned runtime源码中没有残留。

---

# 69. 最终 ownership invariant

```text
I1:
ATPD does not load or attach BPF programs.

I2:
ATPD does not inspect sing-box internal BPF maps.

I3:
ATPD does not independently decide whether sing-box ebpf-in is kernel-compatible.

I4:
sing-box owns ebpf-in lifecycle and capability detection.

I5:
ATPD observes sing-box through service lifecycle and Native API.

I6:
Netlink/XFRM remain ATPD-owned system control/observation mechanisms.

I7:
No fake eBPF telemetry remains.

I8:
No obsolete eBPF config reports successful runtime application.
```

---

# 70. 最终目标结构

删除前：

```text
ATPD
├─ ebpf.c
├─ ebpf_common.c
├─ Netlink/XFRM
├─ service
└─ singbox_api

sing-box
└─ ebpf-in
```

删除后：

```text
ATPD
├─ Netlink/XFRM
├─ service
├─ session
├─ UDS
├─ config
└─ singbox_api
        │
        ▼
     sing-box
        │
        ▼
     ebpf-in
        │
        ▼
      kernel
```

职责边界更加清楚。

---

# 71. 最终结论

既然当前 sing-box 已经原生负责 `ebpf-in`，ATPD继续维护独立 eBPF capability/probe层没有必要。

正确方向不是继续修补 `ebpf.c`，而是：

> 安全删除重复 ownership，让 ATPD保持 control plane，让 sing-box完整拥有 eBPF datapath。

Codex实施时应先完成全仓 dependency audit，再逐步删除。

如果审计确认 `ebpf.c` / `ebpf_common.c` 已没有任何独立于 sing-box 的必要职责，最终应直接删除这两个模块，而不是留下兼容空壳。
