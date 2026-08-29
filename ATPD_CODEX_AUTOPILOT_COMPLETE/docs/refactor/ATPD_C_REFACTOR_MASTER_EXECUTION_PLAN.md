# ATPD C 重构总控实施路线图

> 适用分支：`ebpf-native-api`  
> 用途：作为 Codex 实施 ATPD C 版本重构的唯一总控施工顺序。  
> 原则：**一次只实施一个 Step；完成、测试、提交后，再进入下一 Step。**
>
> 本文档不替代各专项 MD。本文档负责规定：
>
> - 专项 MD 的执行顺序
> - 每一步的目标与边界
> - 哪些 MD 应一起执行
> - 哪些 MD 已废弃
> - 建议 Git commit
> - 每一步给 Codex 的标准指令
> - 阶段验收条件
>
> 未来 Go 重写不属于本路线图。`ATPD_GO_REWRITE_PLAN.md` 独立保留，不参与当前 C 版本施工。

---

# 0. 总原则

## 0.1 不要一次把所有 MD 交给 Codex

禁止：

```text
把 20+ 个 MD 一次性全部提交给 Codex，
然后要求“按照这些方案全部重构”。
```

原因：

```text
上下文过大
ownership 边界容易混乱
Codex 容易提前实施后续阶段
难以定位 regression
难以 review
难以 rollback
```

正确方式：

```text
Step 1
→ 编译
→ 测试
→ review
→ commit

Step 2
→ 编译
→ 测试
→ review
→ commit

...
```

---

# 0.2 一个 Step 原则上对应一个 Codex session / PR

推荐：

```text
1 Step
=
1 个明确目标
+
1~3 个相关 MD
+
若干小 commits
+
完整验证
```

只有明确标注“联合执行”的 MD 才一起给 Codex。

---

# 0.3 禁止 Codex 提前实施后续阶段

如果当前 Step 发现后续问题：

```text
记录 TODO
```

而不是：

```text
顺手把后面 5 个模块一起改掉。
```

否则依赖顺序会失效。

---

# 0.4 每一步开始前必须重新检查当前源码

MD 是设计/审查方案，不是机械 patch。

Codex 必须：

```text
重新读取当前 ebpf-native-api 分支
重新 grep callsites
确认代码是否已因前面 Step 改变
再实施当前 MD
```

不要机械依赖 MD 中旧行号。

---

# 0.5 每一步完成后必须停止

Codex 完成当前 Step 后：

```text
报告结果
停止
等待下一 Step
```

不要自动继续下一份 MD。

---

# 1. 总体施工阶段

整个 C 重构分为 8 个大阶段：

```text
PHASE A
基础类型 / Version / Header

        ↓

PHASE B
Config Model / Validation / Transaction

        ↓

PHASE C
删除 ATPD-owned eBPF

        ↓

PHASE D
Global / Context / Lifecycle

        ↓

PHASE E
Reactor / Service / Native API / Netlink

        ↓

PHASE F
Session / Splice / Async / UDS

        ↓

PHASE G
Error / Logger / Utils / CLI / Status / UI

        ↓

PHASE H
全仓 Stability / Resource / Release
```

---

# PHASE A — 基础模型

目标：

```text
先稳定最底层公共语义，
避免后续模块继续依赖错误的公共定义。
```

---

# STEP 1 — 返回码模型

## 提交给 Codex

```text
ATPD_RESULT_ERROR_MODEL_REFACTOR_PLAN.md
```

## 目标

整理：

```text
atp_error_t
ATP_ERR_*
atp_strerror()
```

明确：

```text
函数返回状态
≠
diagnostic event
≠
errno
≠
process exit code
≠
UDS/API protocol code
```

## 重点

删除：

```text
ATP_ERR_EBPF
```

逐步消除：

```text
ATP_ERR_SERVICE
ATP_ERR_NETLINK
```

改成错误性质：

```text
TIMEOUT
IO
PERM
NOENT
CONFIG
INVAL
...
```

## 推荐方向

```text
atp_error_t
→ atp_result_t

atp_strerror()
→ atp_result_string()
```

可以分阶段完成，不要求一次大 rename。

## 必须检查

```text
ATP_ERR_* 是否被 UDS/API 序列化
ATP_ERR_* 是否直接从 main 返回
ATP_ERR_* 是否被脚本依赖 numeric value
```

## 建议 commit

```text
refactor(result): clarify internal result model
```

## 验收

```text
ATP_ERR_EBPF 不存在
内部负数 result 不直接作为 process exit code
result 与 atpd_error diagnostics 职责明确
```

---

# STEP 2 — Version 单一来源

## 提交给 Codex

```text
ATPD_VERSION_SINGLE_SOURCE_RELEASE_PLAN.md
```

## 目标

解决当前多个版本来源：

```text
include/version.h
Makefile
versions.env
include/atp.h
```

## 建立

```text
/VERSION
```

作为唯一产品版本来源。

例如：

```text
0.9.0
1.0.0-rc.1
1.0.0
```

## Build metadata

构建时生成：

```text
product version
git commit
dirty
full version
```

## 删除

```text
ATP_VERSION_MAJOR
ATP_VERSION_MINOR
ATP_VERSION_PATCH
ATP_BUILD_TIME __TIME__
Makefile hardcoded VERSION
versions.env 中产品 ATP_VERSION
```

## 保证

```text
CLI
UDS
status
version command
```

全部读取同一个 version API。

## 建议 commit

```text
refactor(version): establish single version source
```

## 验收

```text
只有一个 product version source
无 __TIME__
无 fake major/minor/patch
no-git source tarball 可构建
release tag 与 VERSION 可校验
```

---

# STEP 3 — Core Header 第一轮清理

## 提交给 Codex

```text
ATPD_CORE_HEADER_OWNERSHIP_CLEANUP_PLAN.md
```

## 注意

这份 MD **分两次执行**。

当前 Step 3 只做：

```text
第一轮 ownership cleanup
```

最终删除 `atp.h` 放到 Step 27。

## 本轮目标

处理：

```text
_FORTIFY_SOURCE
version macros
EBPF_PROBE_*
MODE_EBPF
cfg_* compatibility macros
module-specific constants
```

## `_FORTIFY_SOURCE`

从 header 移到：

```text
Makefile / compiler flags
```

## 删除 compatibility macros

例如：

```text
cfg_api_port
cfg_service_args
cfg_config_mutex
```

调用点改为明确访问。

## 模块常量回 owner

例如：

```text
service timeout
API retry
netlink timeout
command buffer
```

不再放万能 core header。

## 当前不要强求

```text
立刻删除 atp.h
```

因为后面模块仍可能依赖 umbrella includes。

## 建议 commit

```text
refactor(headers): reduce core umbrella ownership
```

## 验收

```text
_FORTIFY_SOURCE 由 build flags 控制
旧版本宏移除
eBPF core constants 开始移除
cfg_* compatibility macros 消失
```

---

# PHASE B — Config 地基

这是整个后续重构最重要的基础阶段之一。

顺序必须：

```text
Config Model
→ Validator
→ Transaction
```

不能反过来。

---

# STEP 4 — Config Model 纯化

## 提交给 Codex

```text
ATPD_CONFIG_MODEL_IMMUTABILITY_REFACTOR_PLAN.md
```

## 目标

让：

```text
atp_config_t
```

成为真正的：

```text
纯配置 value
immutable candidate
```

## 从 config 移除 CLI 状态

```text
foreground
verbose
no_color
dry_run
```

这些归：

```text
CLI/startup options
```

## 从 config 移除 runtime state

```text
current_vpn_iface
ebpf.ready
```

## 删除

```text
ebpf_config_t
```

## 移动

```text
restart_delay
```

进入：

```text
service_config_t
```

## 最重要

删除：

```text
pthread_mutex_t mutex
```

从 `atp_config_t`。

锁应该保护：

```text
config store
```

而不是每一个 config value。

## 最终目标

合法：

```c
atp_config_t a;
atp_config_t b = a;
```

无需：

```text
mutex init
mutex destroy
special copy helper
```

## 建议 commit

```text
refactor(config): make configuration value immutable
```

## 验收

```text
config 无 mutex
config 无 runtime readiness
config 无 observed VPN iface
config 无 CLI presentation flags
candidate 可 stack create/copy/discard
```

---

# STEP 5 — Config Parser / Validator 严格化

## 提交给 Codex

```text
ATPD_CONFIG_VALIDATOR_STRICTNESS_HARDENING_PLAN.md
```

## 目标

让 config validator 真正成为安全门。

## 必修问题

当前：

```text
config_validate_key()
```

未真正接入 loader。

必须解决：

```text
unknown key 静默忽略
```

## Parser 必须 key-first

禁止：

```text
先看 value 能不能 parse int
再决定 key 类型
```

必须：

```text
lookup key
→ 确定 expected type
→ strict parse
```

## 必须拒绝

```text
API_PORT=abc
SERVICE_START_TIMEOUT=30x
VPN_AUTO_MODE=2
未知 key
duplicate key
canonical + alias conflict
超长字符串
malformed line
未闭合 quote
```

## 禁止

```text
字符串 silent truncation
非法数字 fallback default
```

## Validator

逐步改为：

```text
pure
const candidate
structured diagnostics
no logger side effect
```

## 建议 commit

```text
fix(config): enforce strict typed validation
```

## 验收

```text
unknown key 必失败
invalid integer 必失败
bool 只接受定义值
oversized string 必失败
duplicate key 可检测
secret 不进入错误日志
```

---

# STEP 6 — Transactional Reload

## 提交给 Codex

```text
ATPD_CONFIG_TRANSACTIONAL_RELOAD_PLAN.md
```

## 前置条件

必须完成：

```text
Step 4
Step 5
```

## 最终流程

```text
defaults
↓
parse
↓
merge external source
↓
normalize
↓
validate final candidate
↓
prepare subsystem deltas
↓
commit generation
```

## 失败原则

任何 candidate/prepare 失败：

```text
active config 不变
runtime 不变
daemon 保持 RUNNING
```

## 修复

当前使用：

```text
value == default
```

判断用户是否显式配置的问题。

建立：

```text
presence metadata
```

## Generation

建立明确：

```text
config generation
```

而不是 fake snapshot/rollback。

## 删除 fake API

如果无真实语义：

```text
config_reload_atomic()
config_rollback()
config_set_mode()
旧 backup snapshot
```

## 建议 commit

```text
refactor(config): implement transactional reload
```

## 验收

```text
invalid reload 不影响 active runtime
final merged candidate 必须重新 validate
显式 default value 不会被误判为 unset
generation 只在成功 commit 后增长
```

---

# PHASE C — 删除 ATPD-owned eBPF

---

# STEP 7 — eBPF Module Removal

## 提交给 Codex

```text
ATPD_EBPF_MODULE_REMOVAL_PLAN.md
```

## Architecture invariant

```text
sing-box
owns
ebpf-in dataplane
```

ATPD 不再：

```text
probe BPF
sys_bpf
load maps/programs
attach/detach
维护 ebpf ready
伪造 eBPF telemetry
```

## 删除候选

```text
src/ebpf.c
src/ebpf_common.c
include/ebpf.h
相关 duplicated ABI structs
ENABLE_EBPF
MODE_EBPF
EBPF_PROBE_*
cfg->ebpf
```

先做 callsite audit，再删除。

## Telemetry

如果需要：

```text
ebpf-in status
```

从：

```text
sing-box Native API / service health
```

获取。

## 禁止

创建：

```text
ebpf_manager.c
```

重新包装旧逻辑。

## 建议 commit

```text
refactor(ebpf): remove ATPD-owned dataplane management
```

## 验收

grep：

```text
sys_bpf
__NR_bpf
SYS_bpf
BPF_
RLIMIT_MEMLOCK
ebpf_probe
```

确认 ATPD 不再拥有 dataplane。

---

# PHASE D — Global / Context / Lifecycle

---

# STEP 8 — 删除 atpd_global

## 提交给 Codex

```text
ATPD_GLOBAL_STATE_ELIMINATION_PLAN.md
```

## 目标

删除：

```text
atpd_global_t
g_atpd
global macro aliases
```

## 顺序

```text
remove duplicate aliases
↓
usage map
↓
config owner private
↓
API detach from global config
↓
service/reactor private ownership
↓
signal flags rename
↓
delete atpd_global.c/h
```

## 禁止

把全部字段搬进：

```text
atpd_context
```

这会制造：

```text
atpd_global v2
```

## 建议 commit

```text
refactor(core): eliminate global runtime container
```

## 验收

```text
atpd_global.c/h 删除
无 g_atpd
无 global aliases
各 subsystem 有明确 owner
```

---

# STEP 9 — Context Ownership 收敛

## 本 Step 同时提交两个 MD

```text
ATPD_CONTEXT_STATE_OWNERSHIP_REFACTOR_PLAN.md

ATPD_CONTEXT_PUBLIC_BOUNDARY_REFACTOR_PLAN.md
```

## 说明

后者是前者的补充/强化。

告诉 Codex：

```text
把两份方案合并理解，
不要机械执行两遍。
```

## 删除 context 中

```text
eBPF lifecycle
session registry
XFRM fd
duplicate last_error
duplicate stats
duplicate component readiness
```

## 修复

```text
reload 后 uptime reset
```

## Uptime

改：

```text
CLOCK_MONOTONIC
one-shot daemon start
```

## Session

context 不再：

```text
遍历 session list
destroy sessions
free registry node
```

统一：

```text
session_manager_close_all()
```

## 重要 bug

修复当前 VPN killswitch：

```text
session 可能被重复 destroy
```

## 最终

隐藏：

```c
extern atpd_context_t g_atpd_ctx;
```

外部读取：

```text
immutable runtime snapshot
```

## 建议 commit

```text
refactor(context): shrink runtime context ownership
```

## 验收

```text
context 不再是 god object
无 session registry
无 eBPF state
无 duplicate error/stats
reload 不重置 uptime
g_atpd_ctx 不公开
```

---

# STEP 10 — Init / Shutdown / Rollback

## 提交给 Codex

```text
ATPD_INIT_SHUTDOWN_ROLLBACK_HARDENING_PLAN.md
```

## 目标

建立确定性的：

```text
startup phase stack
shutdown reverse order
rollback
```

## 修复

```text
context init twice
config load twice
service rollback UAF
reactor create failure仍返回成功
signal watch failure被忽略
netlink add_fd failure被忽略
reactor destruction ordering
STOPPED 发布过早
API init failure被忽略
```

## 删除

如果仍 no-op：

```text
cleanup.c
```

## Service rollback

不能：

```text
async stop
→ free service
```

## 建议 commit

```text
refactor(lifecycle): make startup rollback deterministic
```

## 验收

```text
任意 startup phase failure
→ 已完成 phase 按逆序 rollback
→ 无 UAF
→ 正确非零失败
```

---

# STEP 11 — main.c 瘦身

## 提交给 Codex

```text
ATPD_MAIN_LIFECYCLE_ORCHESTRATION_REFACTOR_PLAN.md
```

## 前置

必须完成 Step 10。

## 修复

```text
daemonize false success
help/version依赖 config
reload failure设置 FAILED
main复制 service stop internals
PID identity弱
restart忽略 stop failure
STOPPED过早
```

## Main 最终职责

```text
parse CLI
startup orchestration
run reactor
handle lifecycle requests
shutdown orchestration
exit mapping
```

## 目标规模

大致：

```text
150–300 LOC
```

不是硬指标。

## 建议 commit

```text
refactor(main): reduce daemon lifecycle orchestration
```

## 验收

```text
main 不管理 service internals
main 不拥有 config transaction internals
daemon parent不会在 child startup失败时假报成功
```

---

# PHASE E — Runtime Core

---

# STEP 12 — Reactor

## 提交给 Codex

```text
ATPD_REACTOR_STABILITY_HARDENING_PLAN.md
```

## 为什么现在做

后续：

```text
service
session
async_validate
UDS
```

都依赖 reactor。

## 重点

```text
FD ownership
add/modify/delete
callback lifecycle
timer ownership
signal watcher
destroy ordering
error propagation
```

## 建议 commit

```text
fix(reactor): harden event lifecycle and ownership
```

## 验收

```text
reactor create failure可传播
timer无泄漏
callback removal安全
destroy后无悬空 callback/fd
```

---

# STEP 13 — Service Supervisor

## 提交给 Codex

```text
ATPD_SERVICE_C_REFACTOR_PLAN.md
```

## 注意

不要使用：

```text
ATPD_SERVICE_SUPERVISOR_OPTIMIZATION_PLAN.md
```

它已废弃。

## 目标

service 成为真正 child supervisor owner。

## Ownership

```text
spawn
PID
starttime/generation
reap
stop
restart
health
circuit breaker
```

全部 service 自己负责。

## 禁止

```text
get_pid_by_name
kill_all_by_name
main复制 stop internals
```

## 拆分

按职责拆，不按行数机械拆。

## 建议 commit

```text
refactor(service): establish child supervisor ownership
```

## 验收

```text
owned child一定被 reap
PID reuse有防护
stop/restart只有一个 authoritative implementation
main不操作 child internals
```

---

# STEP 14 — sing-box Native API

## 提交给 Codex

```text
ATPD_SINGBOX_NATIVE_API_RELIABILITY_PLAN.md
```

## 目标

建立 transport owner：

```text
connect
reconnect
timeout
subscription
status cache
API health
```

## 关键

status 等 consumer：

```text
读 cached snapshot
```

而不是每次同步 RPC。

## 建议 commit

```text
refactor(singbox-api): harden native API transport
```

## 验收

```text
Native API unavailable 不阻塞 daemon
status path不做长同步等待
reconnect状态明确
snapshot有 generation/time
```

---

# STEP 15 — API Control Boundary

## 提交给 Codex

```text
ATPD_API_CONTROL_BOUNDARY_REFACTOR_PLAN.md
```

## 前置

Step 14。

## 删除

```text
status/version 20×100ms sleep retry
fake async facade
duplicate base_url/secret/timeout
g_config直接读取
```

## VPN policy

config commit：

```text
copy policy
```

runtime：

```text
observation
→ desired mode
→ reconcile
```

## 建议 commit

```text
refactor(api): make control API a thin facade
```

## 验收

```text
API facade不拥有 transport
API callback不读 global config
API unavailable时 desired state不丢
```

---

# STEP 16 — Netlink / XFRM

## 提交给 Codex

```text
ATPD_NETLINK_XFRM_STABILITY_HARDENING_PLAN.md
```

## 目标

Netlink owner：

```text
fd
registration
XFRM parse
VPN observation
debounce
snapshot
```

## Context

不再保存：

```text
xfrm_fd
current_vpn_iface
```

## 建议 commit

```text
refactor(netlink): own XFRM observation lifecycle
```

## 验收

```text
注册失败不会伪装成功
VPN snapshot跨字段一致
FD ownership唯一
```

---

# PHASE F — Session / Data Path / IPC

---

# STEP 17 — Session Lifecycle

## 提交给 Codex

```text
ATPD_SESSION_LIFECYCLE_OWNERSHIP_HARDENING_PLAN.md
```

## 目标

Session manager自己拥有：

```text
registry
create
closing
destroy request
GC
metrics
```

## 删除

```text
context session registry
register_to_ctx
unregister_from_ctx
```

## VPN teardown

只调用：

```text
session_manager_close_all(reason)
```

一次。

## 建议 commit

```text
refactor(session): centralize session lifecycle ownership
```

## 验收

```text
无 double destroy
无 registry UAF
close-all支持 >256 sessions
GC ownership明确
```

---

# STEP 18 — Splice Consolidation

## 提交给 Codex

```text
ATPD_SPLICE_DATAPATH_CONSOLIDATION_PLAN.md
```

## 先 audit

确认：

```text
standalone splice.c/h
```

是否已经与：

```text
session.c internal splice
```

重复。

如果生产 callgraph不需要：

```text
删除 standalone splice.c/h
```

## 必修

```text
EPOLLET + fairness budget stall
WRITE interest error handling
partial transfer
SIGPIPE
byte integrity
```

## 建议 commit

```text
refactor(splice): consolidate stream datapath
```

## 验收

```text
单 edge + >budget 数据不会永久 stall
字节完整性通过
无 duplicate datapath
```

---

# STEP 19 — Async Validate

## 提交给 Codex

```text
ATPD_ASYNC_VALIDATE_LIFECYCLE_HARDENING_PLAN.md
```

## 关键 bug

当前可能：

```text
EOF callback
→ WNOHANG child还运行
→ return

timeout callback
→ reap child
→ 等 IO callback finish

但 EOF 已经消费
→ 永远不会 finish
```

## 修复

所有路径统一：

```text
finish/reap state machine
```

## 增加

```text
cancel
pipe2 CLOEXEC
nonblocking
exec-error pipe
drain output
```

## 建议 commit

```text
fix(async-validate): unify child completion lifecycle
```

## 验收

```text
timeout/EOF race无挂死
child只 reap一次
shutdown可 cancel
无 zombie
```

---

# STEP 20 — UDS

## 提交给 Codex

```text
ATPD_UDS_RELIABILITY_HARDENING_PLAN.md
```

## 修复

```text
accept FD leak
idle FD exhaustion
partial write
client lifecycle
bounded requests
```

## Status transport

为后面：

```text
structured/plain status
```

准备。

## 建议 commit

```text
fix(uds): harden local control socket lifecycle
```

## 验收

```text
客户端异常断开不泄漏FD
slow/idle client有上限
partial response可正确发送
```

---

# PHASE G — Diagnostics / Platform / Presentation

---

# STEP 21 — Diagnostic Error Ring

## 提交给 Codex

```text
ATPD_ERROR_DIAGNOSTICS_HARDENING_PLAN.md
```

## 目标

`atpd_error` 成为唯一 diagnostics history owner。

## 删除

context里的：

```text
last_error
error_count
第二套 error record
```

## 修复

```text
check-before-lock race
borrowed pointer getter
logging under error mutex
runtime mutex reinit
```

## 建议 commit

```text
refactor(error): centralize diagnostic event history
```

## 验收

```text
copy-out getter
无 duplicate error truth
errno保存明确
diagnostics不等于 health state
```

---

# STEP 22 — Logger

## 提交给 Codex

```text
ATPD_LOGGER_RELIABILITY_HARDENING_PLAN.md
```

## 修复

```text
NONE level array OOB
min_level data race
fake timestamp/syslog settings
init semantics
rotation/path handling
```

## 禁止

logger内部调用：

```text
atpd_error
```

造成 recursion。

## 建议 commit

```text
fix(logger): harden logging state and file safety
```

## 验收

```text
level无OOB
多线程无data race
file open/rotation安全
logger/error无递归依赖
```

---

# STEP 23 — Utils / Platform

## 提交给 Codex

```text
ATPD_UTILS_PLATFORM_SAFETY_REFACTOR_PLAN.md
```

## 重要

这一 Step **不要做成一个巨大 commit**。

至少拆：

### 23.1 Command runner

```text
exec_cmd_argv timeout
nonblocking pipe
single monotonic deadline
drain overflow
exec-error pipe
CLOEXEC
```

Commit：

```text
fix(exec): make command timeout cover full child lifecycle
```

### 23.2 `str_replace`

修 size_t underflow/overflow。

Commit：

```text
fix(utils): make string replacement overflow-safe
```

### 23.3 Process identity / procfs

```text
remove broad cmdline matching
PID + starttime
/proc stat parser
metrics unknown semantics
```

Commit：

```text
refactor(procfs): centralize process identity and metrics
```

### 23.4 Timezone

拆：

```text
timezone.c/h
```

原则：

```text
explicit init
getter无副作用
fallback UTC
locale != timezone
```

Commit：

```text
refactor(timezone): isolate platform timezone handling
```

### 23.5 Paths/files

修：

```text
mkdir truncation
get_app_dir fallback "."
write_file policy
PATH trust
```

Commit：

```text
fix(platform): harden privileged path and file handling
```

### 23.6 Dead APIs

清：

```text
stale declarations
unused process helpers
domain-specific helpers
```

Commit：

```text
refactor(utils): remove stale utility APIs
```

## 最终目标

`utils.c`：

```text
100–250 LOC 左右纯 generic helpers
```

不是硬指标。

---

# STEP 24 — CLI

## 提交给 Codex

```text
ATPD_CLI_STRICT_PARSING_REFACTOR_PLAN.md
```

## 删除旧命令

随 eBPF removal：

```text
旧 eBPF command/options
```

## 修复

```text
atoi
path truncation
trailing args ignored
verbose/quiet order-dependent
foreground/daemon contradiction
```

## CLI options

不再写：

```text
atp_config_t
```

## Version

统一 Step 2 的 canonical version。

## Exit

使用稳定：

```text
process exit mapping
```

而不是 raw ATP negative result。

## 建议 commit

```text
refactor(cli): enforce strict command parsing
```

## 验收

```text
非法参数必失败
尾随垃圾不忽略
互斥选项明确
version无需 config即可工作
```

---

# STEP 25 — Status

## 提交给 Codex

```text
ATPD_STATUS_OBSERVABILITY_REFACTOR.md
```

## 前置

到这里：

```text
service
API
netlink
session
config
error
```

都已有 owner。

## Status 只负责聚合

```text
service snapshot
API snapshot
VPN/netlink snapshot
session metrics
config generation
runtime lifecycle
last diagnostic
```

## 删除

```text
duplicate readiness
fake eBPF telemetry
process FD count = active connections
global mutable reads
```

## 建议 commit

```text
refactor(status): aggregate owner snapshots
```

## 验收

```text
status读取一致 snapshot
无 fake dataplane指标
无 duplicate authoritative state
```

---

# STEP 26 — UI

## 提交给 Codex

```text
ATPD_UI_RENDERING_BOUNDARY_HARDENING_PLAN.md
```

## 建立

```c
ui_render_ctx_t
```

包含：

```text
FILE *out
width
color_enabled
emoji_enabled
```

## 修复

```text
ui_set_no_color() 无效
全局 FILE*
STDOUT width误用于 UDS
ANSI污染 UDS
UTF-8截断
alignment
```

## 原则

```text
status snapshot
→ UI renderer
```

UI 不读取 global config/runtime。

## 建议 commit

```text
refactor(ui): isolate presentation rendering
```

## 验收

```text
no-color真正有效
UDS/plain无ANSI
renderer无global sink
UTF-8不被截断到半字符
```

---

# STEP 27 — Core Header 最终清理

## 再次提交

```text
ATPD_CORE_HEADER_OWNERSHIP_CLEANUP_PLAN.md
```

## 告诉 Codex

这是：

```text
第二阶段 / 最终阶段
```

不要重复 Step 3 已完成内容。

## 目标

此时各 subsystem owner 已稳定。

开始：

```text
逐 source direct include
public header self-contained
移除 umbrella dependency
```

## 最终尽量删除

```text
include/atp.h
```

## 禁止创建

```text
common.h
base.h
all.h
```

作为新的 umbrella。

## CI

每个 public header：

```c
#include "xxx.h"
int main(void) { return 0; }
```

必须单独编译。

## 建议 commit

```text
refactor(headers): remove legacy umbrella header
```

## 验收

```text
无 atp.h include
每个 public header self-contained
无隐藏系统header依赖
```

---

# PHASE H — Stability / Release

---

# STEP 28 — 全仓 Stability Checklist

## 提交给 Codex

```text
ATPD_C_SOURCE_STABILITY_FIX_PLAN.md
```

## 注意

这份现在不是“大重构设计”。

用途是：

```text
全仓 regression checklist
```

## 逐项重新确认

例如：

```text
UDS accept FD leak
child zombie
timer leak
idle FD exhaustion
XFRM false registration
PID identity
partial write
reactor failure
config atomicity
ignored return values
```

## 已经被前面 Step 修掉的

标记：

```text
RESOLVED
```

## 尚未解决的

只补漏项。

## 禁止

因为这份 MD 又重新大改已经稳定的 architecture。

## 建议 commit

```text
fix(stability): close remaining lifecycle regressions
```

## 验收

所有 P0/P1：

```text
resolved / tested / documented
```

---

# STEP 29 — Resource Regression Tests

## 提交给 Codex

```text
ATPD_RESOURCE_TESTING_IMPLEMENTATION.md
```

## 建立

```text
baseline
stress
recovery
```

## 指标

至少：

```text
VmRSS
VmHWM
VmSize
PSS
Private Clean/Dirty
FD
Threads
goroutines
```

## 建议初始 gate

```text
MAX_BASELINE_RSS_KB=3072
MAX_RSS_GROWTH_KB=512
MAX_RSS_SLOPE_KB_PER_MIN=64
MAX_FD_GROWTH=1
MAX_THREAD_GROWTH=0
MAX_GOROUTINE_GROWTH=5
```

具体阈值根据真实设备数据校准。

## Stress

包括：

```text
reload loop
restart loop
sing-box crash/restart
UDS connect/disconnect
VPN state changes
session churn
```

## 建议 commit

```text
test(stability): add resource regression gates
```

## 验收

```text
资源曲线可记录
有明确 FAIL/WARN
recovery后资源回落
无持续 leak slope
```

---

# STEP 30 — RC / Stable Release Gate

这一 Step 不需要新增专项 MD。

使用：

```text
所有前面 MD 的 invariants
+
测试结果
```

## Sanitizers

至少：

```text
ASan
UBSan
TSan
```

能跑的平台尽量覆盖。

## Android 实机矩阵

至少覆盖：

```text
Android 12+
不同 GKI/device
Magisk
KernelSU
APatch
```

## 场景

```text
Wi-Fi → 5G
5G → Wi-Fi
airplane mode
screen off/on
VPN/IPsec
hotspot
sing-box crash
sing-box restart
ATPD reload
ATPD restart
upgrade
uninstall/cleanup
```

## Soak

Stable 前建议：

```text
24h
```

至少一轮真实设备 soak。

## Release 路径

```text
Beta
↓
RC
↓
Stable v1.0
```

## 不允许

```text
“编译通过 + unit tests通过”
直接宣称 Stable。
```

---

# 2. 废弃 / 特殊 MD 清单

## 废弃

```text
ATPD_SERVICE_SUPERVISOR_OPTIMIZATION_PLAN.md
```

不要再提交给 Codex。

替代：

```text
ATPD_SERVICE_C_REFACTOR_PLAN.md
```

---

## Context 两份联合使用

```text
ATPD_CONTEXT_STATE_OWNERSHIP_REFACTOR_PLAN.md
ATPD_CONTEXT_PUBLIC_BOUNDARY_REFACTOR_PLAN.md
```

在 Step 9 一起给 Codex。

第二份视为第一份的：

```text
补充 + 强化
```

不要机械执行两遍。

---

## Stability MD

```text
ATPD_C_SOURCE_STABILITY_FIX_PLAN.md
```

放：

```text
Step 28
```

作为最终 checklist。

不要最先实施。

---

## Go Rewrite

```text
ATPD_GO_REWRITE_PLAN.md
```

不进入当前 C 重构施工序列。

用途：

```text
未来 ATPD-Go 独立路线
```

---

## Header MD

```text
ATPD_CORE_HEADER_OWNERSHIP_CLEANUP_PLAN.md
```

执行两次：

```text
Step 3
→ 第一轮 ownership cleanup

Step 27
→ 最终 umbrella removal
```

---

# 3. 每一步给 Codex 的标准提示词

每次：

```text
上传当前 Step 对应 MD
```

然后复制下面内容。

---

## Codex 标准执行模板

```text
请按照我提供的当前阶段 MD 实施 ATPD ebpf-native-api 分支的本阶段重构。

执行要求：

1. 开始修改前，先重新检查当前 ebpf-native-api 分支代码。
   不要假定 MD 中记录的旧行号、函数位置和 callsite 仍然完全一致。

2. 先做完整 callsite / ownership audit，再修改代码。
   特别是删除函数、文件、enum、struct 字段、macro 之前，必须确认所有 consumer。

3. 只实施当前 MD 所定义的阶段。
   不要提前实施后续 MD 的大规模重构。

4. 如果发现属于后续阶段的问题：
   - 不要顺手大改；
   - 记录为 TODO；
   - 在最终报告中说明应在哪个后续阶段处理。

5. Ownership 原则：
   每一种 runtime/config/resource state 只能有一个 authoritative owner。
   不要通过新增 global、singleton、common context 或 duplicated cache 解决 ownership 问题。

6. 不要为了减少 LOC 机械拆文件。
   只有职责确实独立时才拆分。

7. 不要为了兼容旧设计保留没有实际 consumer 的 fake/stub API。
   删除前必须做 callsite audit。

8. 所有失败路径都必须检查：
   - FD ownership
   - memory ownership
   - child process ownership
   - timer/reactor registration
   - rollback
   - partial initialization
   - shutdown ordering

9. 修改完成后必须完整编译项目。

10. 运行当前仓库已有的相关测试。

11. 针对本次修复发现的 bug / invariant 增加 regression tests。

12. 如果测试因为当前环境无法运行：
    - 明确说明没有运行的测试；
    - 说明原因；
    - 不要声称测试已经通过。

13. 不要修改与当前阶段无关的代码格式或做大范围 cosmetic rewrite。

14. 本阶段完成后停止，不要自动开始下一份 MD。

最终报告必须包含：

A. 本阶段完成内容
B. 修改文件列表
C. 删除文件列表
D. 关键 ownership 变化
E. 行为变化
F. 新增/修改测试
G. 实际执行的 build/test 命令
H. build/test 结果
I. 未完成 TODO
J. 对照当前 MD invariants，逐条说明 PASS / NOT DONE / NOT APPLICABLE
K. 建议的 Git commit message

如果当前源码与 MD 的假设已经不同：
优先保证 MD 的架构目标和 invariant，
不要机械恢复旧代码结构。
```

---

# 4. Step 9 专用 Codex 提示补充

Step 9 同时提交：

```text
ATPD_CONTEXT_STATE_OWNERSHIP_REFACTOR_PLAN.md
ATPD_CONTEXT_PUBLIC_BOUNDARY_REFACTOR_PLAN.md
```

在标准模板前加：

```text
这两份 MD 属于同一个 context 重构阶段。

ATPD_CONTEXT_PUBLIC_BOUNDARY_REFACTOR_PLAN.md
是
ATPD_CONTEXT_STATE_OWNERSHIP_REFACTOR_PLAN.md
的补充和强化。

请合并理解两份方案，不要机械执行两遍。

如果两份文档在细节上存在冲突，
优先采用 ownership 更清晰、global exposure 更少、
runtime/config 分离更严格的方案。
```

---

# 5. Step 23 Utils 专用提示补充

给 Codex：

```text
ATPD_UTILS_PLATFORM_SAFETY_REFACTOR_PLAN.md
```

但明确：

```text
不要做成一个巨大 commit。
```

要求按：

```text
23.1 command runner
23.2 str_replace
23.3 procfs/process identity
23.4 timezone
23.5 paths/files
23.6 dead APIs
```

分 commit。

每个子阶段：

```text
编译 + test
```

后再继续。

---

# 6. Step 28 Stability 专用提示补充

提交：

```text
ATPD_C_SOURCE_STABILITY_FIX_PLAN.md
```

同时告诉 Codex：

```text
这份 MD 现在是 regression checklist，不是重新设计整个项目。

请逐项检查其中的问题在 Step 1–27 后是否已经解决。

对于已解决项：
标记 RESOLVED，并指出对应代码/测试。

对于仍存在的问题：
只修剩余问题。

不要重新引入已删除的旧 architecture。
```

---

# 7. 每个 Step 完成后的人工检查

Codex 完成后，不要立即进入下一 Step。

至少检查：

```text
git diff --stat
git diff
git status
```

确认：

```text
没有修改无关文件
没有偷偷实施后续阶段
没有留下 generated junk
没有误删仍有 caller 的 API
没有新增 global workaround
```

---

# 8. 推荐 Git 策略

建议：

```text
main / ebpf-native-api
保持可运行
```

每 Step：

```text
独立 branch
```

例如：

```text
refactor/result-model
refactor/version-source
refactor/config-model
fix/config-validation
refactor/config-transaction
refactor/remove-ebpf
refactor/context
...
```

如果你直接在当前开发分支工作：

至少保证：

```text
每 Step 独立 commit
```

---

# 9. 每一步的最小合并门槛

必须同时满足：

```text
build PASS
相关 unit/integration test PASS
新增 regression test PASS
无明显 sanitizer regression
MD invariants 检查完成
diff scope合理
```

才进入下一 Step。

---

# 10. 如果某一步失败怎么办

不要：

```text
继续后面 Step 再说。
```

应该：

```text
停在当前 Step
↓
修到稳定
↓
重新 review
↓
commit
↓
再进入下一 Step
```

因为后续 MD 是建立在前面 invariant 已成立的基础上。

---

# 11. 最关键依赖链

整个路线中最不能乱序的是：

```text
RESULT / VERSION / HEADER
        ↓
CONFIG MODEL
        ↓
CONFIG VALIDATION
        ↓
CONFIG TRANSACTION
        ↓
EBPF REMOVAL
        ↓
GLOBAL REMOVAL
        ↓
CONTEXT SHRINK
        ↓
INIT / SHUTDOWN
        ↓
MAIN
```

这是：

```text
架构地基
```

---

第二条关键链：

```text
REACTOR
   ↓
SERVICE
   ↓
SINGBOX API
   ↓
API FACADE
```

---

第三条：

```text
CONTEXT SHRINK
   ↓
NETLINK
   ↓
SESSION
   ↓
SPLICE
```

---

第四条：

```text
OWNER STATES STABLE
   ↓
STATUS
   ↓
UI
```

因此：

```text
STATUS/UI
```

不要提前做。

---

# 12. 完整 Step 快速索引

```text
01 ATPD_RESULT_ERROR_MODEL_REFACTOR_PLAN.md

02 ATPD_VERSION_SINGLE_SOURCE_RELEASE_PLAN.md

03 ATPD_CORE_HEADER_OWNERSHIP_CLEANUP_PLAN.md
   [第一阶段]

04 ATPD_CONFIG_MODEL_IMMUTABILITY_REFACTOR_PLAN.md

05 ATPD_CONFIG_VALIDATOR_STRICTNESS_HARDENING_PLAN.md

06 ATPD_CONFIG_TRANSACTIONAL_RELOAD_PLAN.md

07 ATPD_EBPF_MODULE_REMOVAL_PLAN.md

08 ATPD_GLOBAL_STATE_ELIMINATION_PLAN.md

09 ATPD_CONTEXT_STATE_OWNERSHIP_REFACTOR_PLAN.md
   +
   ATPD_CONTEXT_PUBLIC_BOUNDARY_REFACTOR_PLAN.md

10 ATPD_INIT_SHUTDOWN_ROLLBACK_HARDENING_PLAN.md

11 ATPD_MAIN_LIFECYCLE_ORCHESTRATION_REFACTOR_PLAN.md

12 ATPD_REACTOR_STABILITY_HARDENING_PLAN.md

13 ATPD_SERVICE_C_REFACTOR_PLAN.md

14 ATPD_SINGBOX_NATIVE_API_RELIABILITY_PLAN.md

15 ATPD_API_CONTROL_BOUNDARY_REFACTOR_PLAN.md

16 ATPD_NETLINK_XFRM_STABILITY_HARDENING_PLAN.md

17 ATPD_SESSION_LIFECYCLE_OWNERSHIP_HARDENING_PLAN.md

18 ATPD_SPLICE_DATAPATH_CONSOLIDATION_PLAN.md

19 ATPD_ASYNC_VALIDATE_LIFECYCLE_HARDENING_PLAN.md

20 ATPD_UDS_RELIABILITY_HARDENING_PLAN.md

21 ATPD_ERROR_DIAGNOSTICS_HARDENING_PLAN.md

22 ATPD_LOGGER_RELIABILITY_HARDENING_PLAN.md

23 ATPD_UTILS_PLATFORM_SAFETY_REFACTOR_PLAN.md
   [拆多个 commits]

24 ATPD_CLI_STRICT_PARSING_REFACTOR_PLAN.md

25 ATPD_STATUS_OBSERVABILITY_REFACTOR.md

26 ATPD_UI_RENDERING_BOUNDARY_HARDENING_PLAN.md

27 ATPD_CORE_HEADER_OWNERSHIP_CLEANUP_PLAN.md
   [第二阶段 / 最终清理]

28 ATPD_C_SOURCE_STABILITY_FIX_PLAN.md
   [最终 regression checklist]

29 ATPD_RESOURCE_TESTING_IMPLEMENTATION.md

30 RC / Stable release validation
```

---

# 13. 不进入施工序列

```text
ATPD_GO_REWRITE_PLAN.md
```

这是未来路线。

---

# 14. 明确废弃

```text
ATPD_SERVICE_SUPERVISOR_OPTIMIZATION_PLAN.md
```

不要再给 Codex。

---

# 15. 每完成一个大 Phase 后建议重新 Review

推荐检查点：

## Checkpoint A

完成：

```text
Step 1–3
```

检查：

```text
public definitions
version
header ownership
```

---

## Checkpoint B

完成：

```text
Step 4–7
```

重点重新 review：

```text
config.c
config_validator.c
eBPF residue
sing-box ownership
```

---

## Checkpoint C

完成：

```text
Step 8–11
```

重点 review：

```text
global/context
startup/shutdown
main
```

这是一次非常重要的 architecture checkpoint。

---

## Checkpoint D

完成：

```text
Step 12–16
```

重新 review：

```text
reactor
service
Native API
API
netlink
```

---

## Checkpoint E

完成：

```text
Step 17–20
```

重点：

```text
session lifecycle
splice integrity
async child lifecycle
UDS FD ownership
```

---

## Checkpoint F

完成：

```text
Step 21–27
```

重点：

```text
diagnostics
logger
utils
CLI
status/UI
public headers
```

---

## Final

完成：

```text
Step 28–30
```

才进入：

```text
RC / Stable
```

---

# 16. 最终架构目标

完成整个路线后，ATPD C 版本应该趋向：

```text
CLI / UI
    │
    ▼
UDS / Status
    │
    ▼
Control API
    │
    ├──────────────► sing-box Native API
    │
    ▼
Service Supervisor
    │
    ▼
sing-box child
    │
    ▼
sing-box ebpf-in
    │
    ▼
kernel eBPF
```

旁路：

```text
Netlink/XFRM
    │
    ▼
VPN Observation
    │
    ▼
Policy Reconcile
```

runtime：

```text
Reactor
├── Service
├── Native API
├── Netlink
├── Session
├── Async validation
└── UDS
```

core：

```text
Immutable Config
Lifecycle
Diagnostics
Logger
```

---

# 17. 最终 Ownership 目标

```text
Config
→ desired configuration only

Service
→ child PID/process lifecycle

singbox_api
→ Native API transport + cached snapshots

Netlink
→ XFRM/VPN observation

Session
→ session registry/lifecycle

Reactor
→ FD/timer/signal event ownership

atpd_error
→ diagnostic history

Logger
→ output/log files

Status
→ aggregate snapshots only

UI
→ rendering only

sing-box
→ eBPF dataplane
```

---

# 18. 最终禁止出现

```text
g_atpd god global

public mutable g_atpd_ctx

config runtime readiness

ATPD-owned eBPF syscalls/probes

duplicate session registries

duplicate service stop implementations

duplicate API transport state

duplicate last_error

fake atomic reload

fake rollback

silent config truncation

silent unknown config keys

name-based child process ownership

main directly manipulating service internals

status directly querying every subsystem synchronously

UI reading global config

new common.h umbrella
```

---

# 19. Stable 的定义

完成代码重构不代表 Stable。

至少：

```text
P0/P1 fixed
↓
sanitizers
↓
stress/resource regression
↓
crash/restart/reload recovery
↓
real Android device matrix
↓
24h soak
↓
RC
↓
Stable
```

---

# 20. 最后给 Codex 的总原则

如果 Codex 在任何 Step 面临：

```text
“为了兼容旧结构”
vs
“保持明确 ownership”
```

优先：

```text
明确 ownership
```

如果面临：

```text
“复制一份状态更方便”
vs
“从 authoritative owner 获取 snapshot”
```

优先：

```text
authoritative owner
```

如果面临：

```text
“加一个 global 快速解决”
vs
“明确传递依赖”
```

优先：

```text
明确依赖
```

如果面临：

```text
“ATPD自己再管理一遍 eBPF”
vs
“sing-box owns ebpf-in”
```

永远选择：

```text
sing-box owns ebpf-in
```

---

# 21. 一句话施工原则

```text
先统一基础语义，
再纯化配置，
再删除错误 ownership，
再重建生命周期，
再加固 runtime owner，
最后做 status/UI/测试和发布。
```

不要反过来。
