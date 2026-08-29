# ATPD `atpd_context` 运行态中心与 Ownership 收敛方案

## 1. 模块结论

当前：

```text
src/atpd_context.c      ~315 lines
include/atpd_context.h  ~131 lines
```

文件本身不大，不需要拆分。

但它现在同时承担了：

```text
VPN state
XFRM state
session registry
eBPF state
daemon runtime state
component readiness
global statistics
last error
callbacks
kill-switch
```

这已经开始表现出典型的：

> “global context 逐渐变成所有模块共享的万能 struct”。

本轮目标不是把 context 做得更大，而是：

> 把它收缩成真正属于 ATPD 全局生命周期的少量 authoritative state，其余状态回到各自 owner module。

---

# 2. 推荐最终职责

`atpd_context` 最终只保留类似：

```text
daemon runtime lifecycle
VPN high-level observed state
daemon start monotonic timestamp
global shutdown/reload generation
少量跨模块 immutable/runtime references
```

不应该长期保存：

```text
eBPF subsystem state
service内部状态
API内部状态
reactor内部统计
session registry ownership
模块内部 FD
重复 error subsystem
```

---

# 3. 当前确认的明确 bug：reload 会重置 uptime

当前：

```c
void atpd_runtime_state_transition(atpd_runtime_state_t new_state) {
    ...
    if (new_state == ATPD_RUNTIME_STATE_RUNNING) {
        g_atpd_ctx.start_time = time(NULL);
    }
}
```

假设：

```text
startup
UNINITIALIZED
→ INITIALIZING
→ RUNNING
```

第一次设置 `start_time` 没问题。

但 reload：

```text
RUNNING
→ RELOADING
→ RUNNING
```

又会：

```text
start_time = now
```

于是：

```text
daemon uptime
```

被重置。

---

# 4. 正确 daemon uptime 语义

daemon start time 应该：

```text
context init / successful startup时设置一次
```

之后：

```text
reload
network transition
service restart
API reconnect
```

均不能重置。

推荐字段：

```c
struct timespec started_at_mono;
time_t started_at_wall;
```

其中：

```text
started_at_mono
→ uptime

started_at_wall
→ 人类显示启动时间
```

---

# 5. Uptime 使用 CLOCK_MONOTONIC

当前：

```text
start_time = time(NULL)
uptime = time(NULL) - start_time
```

wall clock 可能因为：

```text
NTP
手工改时间
RTC correction
```

跳变。

daemon elapsed time应使用：

```text
CLOCK_MONOTONIC
```

---

# 6. 不需要缓存 `uptime_seconds`

当前 context既保存：

```text
start_time
```

又保存：

```text
uptime_seconds
```

后者是 derived state。

推荐删除：

```text
uptime_seconds
```

每次：

```text
now_mono - started_at_mono
```

直接算。

避免 stale/duplicate state。

---

# 7. `last_activity_time` 语义也比较模糊

当前 runtime state transition时更新：

```text
last_activity_time
```

但：

```text
session traffic
Netlink event
API telemetry
timer
```

都未必更新。

因此名字：

```text
last_activity_time
```

会误导。

如果它实际表示：

```text
last runtime state transition
```

就重命名：

```text
runtime_state_changed_at
```

否则删除。

---

# 8. eBPF state 应从 context 删除

我们已经确定：

```text
sing-box owns ebpf-in
```

因此以下字段不再属于 ATPD context：

```c
ebpf_state_t ebpf_state;
bool ebpf_enabled;
bool ebpf_probed;
components.ebpf_ready;
```

以及：

```c
atpd_ebpf_state_transition()
ebpf_state_string()
```

如果没有其他独立用途：

全部删除。

---

# 9. Context 不应保留旧 eBPF compatibility state

不要为了旧 status 继续留下：

```text
EBPF_STATE_READY
EBPF_STATE_FAILED
```

否则删除 `ebpf.c` 后会留下一个没人真正维护的假状态。

---

# 10. `xfrm_fd` 也不应放在 global context

当前：

```c
g_atpd_ctx.xfrm_fd
```

同时 `netlink.c` 自己已经拥有：

```text
g_xfrm_fd
```

这是 duplicated ownership。

FD 应只有一个 owner：

```text
netlink/XFRM module
```

context/status如果需要：

```text
netlink_get_status()
```

读取 snapshot。

不要复制 FD number。

---

# 11. 为什么不能复制 subsystem FD

因为：

```text
fd >= 0
```

不等于：

```text
registered
healthy
usable
```

之前 netlink review已经确认：

```text
XFRM socket open
```

和：

```text
reactor registered
```

是不同状态。

所以 global context持一个 raw FD只会制造假 readiness。

---

# 12. Component readiness 当前属于 duplicated state

context保存：

```c
netlink_ready
ebpf_ready
service_ready
api_ready
reactor_ready
```

但各 module本身都拥有真正状态。

例如：

```text
service
→ STOPPED/STARTING/RUNNING/BACKOFF/FAILED

singbox_api
→ HEALTHY/DEGRADED/...

netlink
→ REGISTERED/DEGRADED/...
```

压缩成：

```text
bool ready
```

会丢失信息并容易 stale。

---

# 13. 推荐删除 generic component readiness

长期：

```text
status collector
```

直接从 owner module snapshot收集：

```text
service_get_status()
singbox_api_get_snapshot()
netlink_get_status()
reactor_get_stats()
session_get_stats()
```

不要：

```text
module改变
↓
顺手记得同步 g_atpd_ctx.components.xxx
```

---

# 14. `atpd_component_set_ready(const char *name, ...)`

这是 stringly-typed API：

```c
if strcmp(name, "netlink")
else if strcmp(name, "ebpf")
...
```

问题：

```text
typo静默无效
未知 component静默无效
编译器无法检查
```

如果 readiness最终删除：

整个 API 删除。

如果短期必须保留：

至少改 enum。

但不建议继续投资这套 duplicated readiness。

---

# 15. Global statistics 同样需要 ownership审计

context保存：

```text
events_processed
timers_fired
signals_received
errors_total
bytes_rx
bytes_tx
```

其中很多真正 owner已经很明确：

```text
reactor
→ events/timers/signals

session
→ bytes

error subsystem
→ errors
```

因此 global context不应该再维护第二份计数。

---

# 16. 特别是 reactor statistics

`reactor.c` 本身已有 stats。

context再提供：

```c
atpd_stats_increment_events()
atpd_stats_increment_timers()
atpd_stats_increment_signals()
```

会产生：

```text
reactor stats
vs
context stats
```

两个来源。

应保留 owner module authoritative stats。

---

# 17. bytes_rx / bytes_tx

session已有：

```text
bytes_in
bytes_out
splice_bytes
```

所以 context中的：

```text
bytes_rx
bytes_tx
splice_bytes_total
```

必须做全仓调用审计。

如果没有一个清楚、统一的累计策略：

删除或迁移到：

```text
session aggregate stats
```

---

# 18. Last error 与 `atpd_error` 重复

`atpd_context_init()`首先：

```c
atpd_error_init();
```

但 context自己又保存：

```text
last_error_code
last_error_msg
last_error_time
error_count
```

并实现：

```c
atpd_error_record()
```

这说明 error ownership可能已经有两套。

---

# 19. Codex 必须先审计 error subsystem

搜索：

```text
atpd_error_init
atpd_error_record
atpd_error_get_last_code
last_error
error_count
ATPD_ERR_
```

回答：

```text
atpd_error.c
和
atpd_context.c
谁才是 authoritative error store？
```

推荐：

> error state只由 `atpd_error.c` 管理。

context删除重复 last_error。

---

# 20. Global error 也不应成为所有模块唯一错误

真正 status应该有：

```text
service last error
API last error
netlink last error
config last reload error
```

单个：

```text
global last_error
```

很容易被无关的新错误覆盖。

所以 context中的 last-error store长期价值有限。

---

# 21. VPN state 是 context中相对合理的全局状态

当前：

```text
IDLE
PREDICTING
READY
TEARDOWN
```

代表 ATPD 对系统 VPN lifecycle 的高层观察。

这是跨：

```text
Netlink/XFRM
session killswitch
mode callback
```

的协调状态。

因此暂时可以留在 context。

---

# 22. 但 VPN state update 目前不是整体原子的

只有：

```c
atomic_int vpn_state;
```

是 atomic。

同时 transition修改：

```text
xfrm_if_id
vpn_iface
vpn_state_since
vpn_transitions
```

这些都不是 atomic/lock保护。

因此 reader可能看到：

```text
vpn_state = READY
但 iface/if_id仍是旧值
```

---

# 23. 单线程 reactor 模型下，不需要伪线程安全

如果所有 VPN transition和读取都明确：

```text
reactor thread
```

那么：

```text
atomic_int
```

本身没有太大价值。

正确做法是：

> 明确 thread ownership。

---

# 24. 如果 status/other thread确实跨线程读取

那一个 atomic state也不够。

应使用：

```text
snapshot lock
or
seqlock-style generation
or
copy under mutex
```

保证：

```text
state
iface
if_id
timestamp
```

来自同一个 snapshot。

---

# 25. 推荐 VPN snapshot

```c
typedef struct {
    vpn_state_t state;
    uint32_t if_id;
    char iface[32];
    uint64_t changed_at_ms;
    uint64_t transitions;
} atpd_vpn_snapshot_t;
```

外部：

```c
int atpd_context_get_vpn_snapshot(
    atpd_vpn_snapshot_t *out);
```

不要直接读：

```text
g_atpd_ctx.xxx
```

---

# 26. `atpd_vpn_state_transition()` 目前接受任意 transition

当前：

```text
atomic_exchange(new_state)
```

没有合法 transition验证。

需要决定 VPN state是不是严格 state machine。

推荐至少：

```text
IDLE → PREDICTING/READY
PREDICTING → READY/IDLE/TEARDOWN
READY → TEARDOWN/IDLE/PREDICTING
TEARDOWN → IDLE/PREDICTING
```

具体根据 Netlink/XFRM实际行为确定。

---

# 27. 如果 state只是 observation label

那就不要称它为严格 state machine。

可以允许任何 observed transition。

关键是：

```text
语义要一致
```

不要文档写严格 FSM、代码却只是 latest observed label。

---

# 28. `old_state == new_state && !iface_changed` 的遗漏

当前：

```c
bool iface_changed = ...
if (old_state == new_state && !iface_changed) return;
```

它没有检查：

```text
if_id changed
```

如果：

```text
state相同
iface字符串相同
但 XFRM if_id发生变化
```

transition会被忽略。

---

# 29. Same-state update 应检查完整 identity

至少：

```text
state
if_id
iface
```

任一变化都应更新 snapshot。

---

# 30. `iface == NULL/empty` 时 callback获得什么

当前：

```c
vpn_mode_callback(new_state, iface, userdata);
```

如果 transition里：

```text
iface == NULL
```

callback也收到 NULL。

但是 context可能已经：

```text
保留旧 iface
或清空 iface
```

callback看到的并不一定等于 authoritative snapshot。

---

# 31. 推荐 callback传 snapshot

长期：

```c
typedef void (*atpd_vpn_state_callback_t)(
    const atpd_vpn_snapshot_t *snapshot,
    void *userdata);
```

这样 callback拿到：

```text
state
if_id
iface
timestamp
```

是同一份 committed state。

---

# 32. Callback raw userdata lifetime没有 contract

context长期保存：

```text
vpn_mode_callback
vpn_mode_userdata
vpn_teardown_cb
```

如果 owner module cleanup后：

```text
userdata被释放
```

context仍可能 callback。

这是潜在 UAF。

---

# 33. Callback registration必须支持 clear/unregister

当前：

```c
atpd_set_vpn_mode_callback(callback, userdata);
```

可以通过 callback=NULL清理吗？

实现允许，但 header没有写 contract。

需要明确：

```text
owner cleanup
→ unregister callback
→ then free userdata
```

---

# 34. 更安全的 ownership原则

因为 ATPD多数 subsystem单例：

callback注册/清理可以保持简单。

但必须明确 shutdown顺序：

```text
stop producer transitions
↓
unregister callback
↓
destroy callback owner
```

---

# 35. Kill-switch 不应该放在 context module

当前：

```c
g_atpd_ctx.vpn_teardown_cb = atpd_vpn_killswitch;
```

然后 `atpd_vpn_killswitch()` 自己操作：

```text
session registry
session destroy
```

这让 context既：

```text
保存 VPN state
```

又：

```text
实现 session destruction policy
```

职责过重。

---

# 36. Kill-switch 应迁移到 session/controller owner

推荐：

```text
VPN state transition
↓
registered observer/controller
↓
session_close_all(VPN_TEARDOWN)
```

context本身只：

```text
commit state
notify observer
```

不应该知道 session list结构。

---

# 37. Session registry 应从 context删除

当前 context header公开：

```c
struct atpd_session_list {
    struct atpd_session *session;
    ...
};
```

这正是我们在 session review里要收紧的 ownership。

session registry属于：

```text
session subsystem
```

不是 global context。

---

# 38. 推荐迁移

从：

```text
g_atpd_ctx.sessions
atpd_session_register_to_ctx()
atpd_session_unregister_from_ctx()
atpd_vpn_killswitch()
```

迁移到：

```text
session.c
```

例如：

```c
session_registry_add()
session_registry_remove()
atpd_session_close_all(reason)
```

---

# 39. 当前 kill-switch 存在重复 destroy逻辑

当前：

```text
先复制最多256 session pointer
↓
destroy这些session
↓
再次遍历 g_atpd_ctx.sessions
↓
再 destroy
↓
手工 free list nodes
```

这和 session review里已经确认的问题一致。

应该整体删除，不要在 context里修一个更复杂版本。

---

# 40. 为什么 context不应手工 free session list node

如果：

```text
session final destroy
→ unregister_from_ctx
→ free node
```

同时 kill-switch：

```text
free node
```

就存在两个 ownership路径。

哪怕 deferred destroy当前避免了立即 double-free：

设计也不正确。

节点只能由 session registry owner释放。

---

# 41. `atpd_context_init()` 当前直接 memset整个 global context

```c
memset(&g_atpd_ctx, 0, sizeof(g_atpd_ctx));
```

第一次 startup没问题。

但如果错误地调用第二次：

```text
active sessions
callbacks
fds
state
```

都会直接丢失。

---

# 42. Init必须是明确 one-shot

推荐：

```text
atpd_context_init()
MUST be called exactly once
before any subsystem starts
```

debug build：

```text
assert !initialized
```

不要把它当 reset函数。

---

# 43. Context cleanup

当前看不到：

```text
atpd_context_cleanup()
```

如果最终 context只保存 value state，不需要复杂 cleanup。

但 callback registration最好在 subsystem cleanup阶段被 clear。

不要为了对称而增加一个大 cleanup owner。

---

# 44. Runtime state 目前也不是 atomic

```c
g_atpd_ctx.runtime_state
```

普通 enum。

如果只有 reactor/main thread：

没问题。

如果 UDS/status线程读取：

需要 snapshot或明确 single-thread。

再次强调：

> 不要用局部 atomic制造“看起来 thread-safe”的错觉。

---

# 45. Runtime FSM应该更严格

当前：

```text
UNINITIALIZED
INITIALIZING
RUNNING
RELOADING
STOPPING
STOPPED
FAILED
```

这是很适合真正做 FSM 的。

推荐合法 transition：

```text
UNINITIALIZED
→ INITIALIZING

INITIALIZING
→ RUNNING
→ FAILED
→ STOPPING

RUNNING
→ RELOADING
→ STOPPING
→ FAILED

RELOADING
→ RUNNING
→ STOPPING
→ FAILED

FAILED
→ STOPPING
→ STOPPED
or explicit recovery if designed

STOPPING
→ STOPPED

STOPPED
→ none
```

---

# 46. `atpd_runtime_can_reload()` 当前语义可疑

现在：

```c
return state == RUNNING ||
       state == RELOADING;
```

也就是说：

```text
已经 RELOADING
```

仍然返回：

```text
can reload = true
```

这与我们 config transaction方案：

```text
only one reload at a time
```

冲突。

---

# 47. 推荐

```c
atpd_runtime_can_reload()
```

只在：

```text
RUNNING
```

返回 true。

如果正在：

```text
RELOADING
```

新 reload：

```text
EBUSY
```

或者由 config manager serialize。

---

# 48. `reload_count` ownership

当前 context有：

```text
reload_count
```

但 config transaction更适合维护：

```text
config generation
reload successes/failures
```

如果这个字段只用于 config reload：

迁移到 config subsystem。

context无需再维护。

---

# 49. `error_count`

同理：

如果 error subsystem有完整计数：

删除。

不要存：

```text
context.error_count
context.stats.errors_total
```

两份 error计数。

---

# 50. Context应减少 mutable public fields

目前 header：

```c
extern atpd_context_t g_atpd_ctx;
```

任何模块都可以：

```text
g_atpd_ctx.foo = ...
```

这样 context API无法维护 invariant。

---

# 51. 长期推荐 opaque/private global

可以继续有 global singleton：

```text
static atpd_context_t g_atpd_ctx;
```

但不要在 public header暴露 struct。

Public只暴露：

```text
get snapshot
transition
get runtime state
```

---

# 52. 第一阶段不必马上 opaque

直接 opaque会造成全仓大改。

建议先：

```text
grep g_atpd_ctx.
```

建立 direct-field access列表。

逐步迁走。

最后再把：

```text
extern g_atpd_ctx
```

删除。

---

# 53. Codex 必须全仓搜索 direct access

搜索：

```text
g_atpd_ctx.
```

分类：

```text
VPN
XFRM
eBPF
session
runtime
components
stats
errors
```

每一个 direct access必须决定：

```text
keep via context API
or
move to owner module
```

---

# 54. Context 不应该成为 status database

我们的 status方案是：

```text
owner module snapshot
↓
status_collect
```

不是：

```text
所有模块定期 copy state到 g_atpd_ctx
↓
status
```

后者一定会产生 stale duplicated state。

---

# 55. 推荐最终结构

```c
typedef struct {
    atpd_runtime_state_t runtime_state;

    struct timespec started_at_mono;
    time_t started_at_wall;

    struct {
        vpn_state_t state;
        uint32_t if_id;
        char iface[32];
        struct timespec changed_at_mono;
        uint64_t transitions;
    } vpn;

    bool initialized;
} atpd_context_t;
```

甚至还能进一步缩小。

---

# 56. 不要在 context保存 reactor pointer / service pointer除非必要

如果未来有人想把所有 subsystem pointer都塞进：

```text
atpd_context
```

应克制。

依赖注入/owner module singleton比万能 service locator更清晰。

---

# 57. Runtime start与 process start

如果 daemon process已经启动：

```text
started_at
```

应该在 context initialization/main startup设一次。

如果想统计：

```text
last became RUNNING
```

另加：

```text
running_since
```

不要复用一个字段承担两种语义。

---

# 58. VPN state timestamp也应该 monotonic

当前已经：

```text
CLOCK_MONOTONIC
```

这一点是正确的。

保留。

---

# 59. `elapsed_us` 计算

当前手工：

```text
sec * 1,000,000 + nsec/1000
```

可以继续。

最好复用统一 time helper：

```text
timespec_diff_ms/us
```

避免各模块重复。

---

# 60. State string数组

runtime state string通过：

```text
designated initializer array
```

合理。

VPN用 switch也没问题。

无需为了风格统一而改。

---

# 61. Invalid enum防御

`runtime_state_names[state]` 前有 range检查。

保持。

如果未来 enum存在 sparse值：

改 switch。

当前没必要。

---

# 62. Error message NULL

当前：

```c
strncpy(..., msg,...)
```

若 caller：

```text
msg == NULL
```

会 crash。

如果该 API保留：

至少：

```text
msg ? msg : ""
```

但更推荐把 error ownership迁回 `atpd_error.c`。

---

# 63. Component API NULL name

当前：

```c
strcmp(name,...)
```

如果：

```text
name == NULL
```

会 crash。

同样，如果删除 component readiness整个问题消失。

---

# 64. VPN iface copy是 bounded的

当前：

```c
snprintf(vpn_iface, sizeof, "%s", iface);
```

不会 overflow。

但长 interface名会 truncate。

Linux IFNAMSIZ通常16，因此32足够。

推荐使用：

```text
IFNAMSIZ
```

而不是 magic 32。

---

# 65. `xfrm_if_id` 与 iface relation

这是 observed VPN identity的一部分。

如果 XFRM event只是 hint、full refresh才是 authority：

context transition只应接收：

```text
confirmed snapshot
```

不要把 raw SA event直接永久写为 authoritative state。

这与 netlink方案一致。

---

# 66. VPN_STATE_PREDICTING

这个 state很适合表示：

```text
event hint arrived
等待 full refresh确认
```

但必须确保：

```text
settle timer失败
```

不会永远卡在 PREDICTING。

这个问题主要由 netlink module负责。

context只记录收到的 state。

---

# 67. Kill-switch触发语义

当前任何：

```text
new_state == TEARDOWN
```

都会调用 kill-switch。

如果 same TEARDOWN + iface change：

前面不会 return，因此可能再次 kill-switch。

session close_all应幂等。

但最好决定：

```text
kill-switch只在 crossing into TEARDOWN
```

还是：

```text
每个 teardown observation都触发
```

---

# 68. 推荐只在 state edge触发 destructive side effect

即：

```text
old_state != TEARDOWN
&&
new_state == TEARDOWN
```

才：

```text
session_close_all
```

same-state metadata更新不应重复执行 destructive action。

---

# 69. 回调同样要决定 edge还是 update

mode callback可能希望：

```text
state没变但 iface变了
```

也收到更新。

因此区分：

```text
state_changed
identity_changed
```

callback可以收到任何 snapshot update。

destructive action只在 state edge执行。

---

# 70. Test：uptime不因 reload重置

```text
start
sleep/sample
RUNNING → RELOADING → RUNNING
```

验证：

```text
uptime单调增加
```

---

# 71. Test：wall clock跳变

mock wall time前后跳。

验证：

```text
uptime基于 monotonic不受影响
```

---

# 72. Test：can_reload

状态：

```text
RUNNING → true
RELOADING → false
STOPPING → false
FAILED → false
```

与 transactional reload一致。

---

# 73. Test：same state + if_id changed

```text
READY if_id=1 iface=ipsec0
→
READY if_id=2 iface=ipsec0
```

snapshot必须更新。

---

# 74. Test：same TEARDOWN metadata update

如果设计为 edge-trigger destructive action：

```text
TEARDOWN → TEARDOWN
```

不应再次执行 kill-switch。

---

# 75. Test：callback unregister

```text
register callback + userdata
clear callback
free userdata
transition
```

不能调用旧 pointer。

---

# 76. Test：VPN snapshot一致性

如果有跨线程 reader：

高频 transition与read。

验证永远不会出现：

```text
state from generation N
iface from generation N-1
```

如果 single-thread contract：

写相应线程 assertion测试。

---

# 77. Test：context init twice

debug/test build：

第二次调用：

```text
reject/assert
```

不能静默 memset active runtime。

---

# 78. Test：no eBPF state residual

完成 eBPF删除后：

```text
grep EBPF_STATE
grep ebpf_ready
grep ebpf_enabled
grep ebpf_probed
```

context不应残留旧状态。

---

# 79. Test：no raw XFRM FD duplicate

最终：

```text
g_atpd_ctx.xfrm_fd
```

不存在。

Netlink模块自己报告状态。

---

# 80. Test：no context session registry

最终：

```text
g_atpd_ctx.sessions
atpd_session_register_to_ctx
atpd_session_unregister_from_ctx
```

迁移/删除。

session subsystem拥有 registry。

---

# 81. Test：>256 sessions kill-switch

这项在 session方案里完成。

context移除固定：

```text
session_ptrs[256]
```

不再自己实现 close-all。

---

# 82. Test：stats single source

检查：

```text
reactor events
session bytes
error counters
```

status只从各 owner snapshot读取。

context不再有第二份数字。

---

# 83. Stress

高频：

```text
VPN transition
status snapshot
config reload
service restart
```

至少运行：

```text
10k–100k state operations
```

确保：

```text
state一致
uptime单调
无 callback UAF
```

---

# 84. 推荐 Commit 1

```text
context: fix runtime uptime and reload semantics
```

内容：

- monotonic daemon start
- RUNNING不重置 start
- can_reload只在RUNNING
- tests

---

# 85. Commit 2

```text
context: remove obsolete ebpf runtime state
```

配合：

```text
ATPD_EBPF_MODULE_REMOVAL_PLAN
```

删除：

```text
ebpf enum
fields
component ready
transitions
status dependencies
```

---

# 86. Commit 3

```text
context: move session ownership into session subsystem
```

内容：

- registry migration
- close_all migration
- kill-switch simplification
- remove list struct from context header

---

# 87. Commit 4

```text
context: remove duplicated subsystem readiness and stats
```

内容：

- component_set_ready/is_ready
- duplicated reactor stats
- duplicated session bytes
- status reads owner snapshots

可以分成两个 commit降低风险。

---

# 88. Commit 5

```text
context: remove duplicated error storage
```

前提：

```text
atpd_error.c
```

审计完成并作为 authoritative owner。

---

# 89. Commit 6

```text
context: expose consistent vpn snapshot
```

内容：

- snapshot API
- full identity comparison
- callback snapshot semantics
- destructive edge semantics

---

# 90. Commit 7

```text
context: hide mutable global internals
```

最后阶段：

- 清理 `g_atpd_ctx.` direct access
- context struct变 private/opaque
- public只保留窄 API

这是中期目标，不强制一次完成。

---

# 91. Codex 修改前必须先建立 direct-access map

运行：

```text
grep -R "g_atpd_ctx\." src include tests
```

对每个 access列：

```text
file
field
read/write
owner
replacement
```

不要直接把 struct私有化后全仓机械加 getter/setter。

目标不是：

```text
global fields
→ 100个 getter
```

目标是：

```text
state回到正确 owner
```

---

# 92. Codex 必须审计 `atpd_error.c`

在删除 context error前确认：

```text
error storage
thread model
status API
logging
```

如果 `atpd_error.c` 当前能力不足：

先增强它。

不要直接丢失 error observability。

---

# 93. 与 `session.c` 方案的关系

必须执行：

```text
session registry
session close_all
```

迁回 session subsystem。

context只触发：

```text
VPN state observer
```

不接触 session list节点。

---

# 94. 与 `netlink.c` 方案的关系

删除：

```text
g_atpd_ctx.xfrm_fd
```

status通过：

```text
netlink_get_status()
```

获得：

```text
OPEN
REGISTERED
DEGRADED
...
```

而不是 raw FD。

---

# 95. 与 eBPF删除方案的关系

同步删除：

```text
ebpf_state
ebpf_ready
ebpf_enabled
ebpf_probed
```

不要留下 compatibility dead state。

---

# 96. 与 config方案的关系

`RELOADING` 必须与：

```text
one active transaction
```

一致。

新 reload在：

```text
RELOADING
```

应拒绝/serialize。

config generation放 config subsystem。

---

# 97. 与 status方案的关系

最终：

```text
status_collect
├─ context_get_runtime_snapshot
├─ context_get_vpn_snapshot
├─ service_get_status
├─ netlink_get_status
├─ singbox_api_get_snapshot
├─ reactor_get_stats
└─ session_get_stats
```

这比：

```text
所有状态先复制进 context
```

更可靠。

---

# 98. 是否拆 `atpd_context.c`

不拆。

相反：

> 它应该随着 ownership迁移而变小。

理想：

```text
100–200 LOC
```

完全足够。

---

# 99. 不要把 context改成 DI container

不要未来变成：

```c
struct atpd_context {
    reactor_t *reactor;
    service_t *service;
    netlink_t *netlink;
    session_manager_t *sessions;
    api_t *api;
    config_t *config;
    ...
};
```

除非整个项目明确采用对象化 dependency injection。

当前单 daemon C architecture没必要引入万能 service locator。

---

# 100. 推荐最终 API

大致：

```c
void atpd_context_init(void);

atpd_runtime_state_t atpd_runtime_get_state(void);
int atpd_runtime_transition(atpd_runtime_state_t next);
uint64_t atpd_runtime_get_uptime_ms(void);

int atpd_vpn_update(...);
void atpd_vpn_get_snapshot(atpd_vpn_snapshot_t *out);

void atpd_set_vpn_state_callback(...);
```

再少一点也可以。

---

# 101. 最终 Invariants

Codex最终应保证：

```text
I1:
daemon start timestamp is written once and uptime is monotonic

I2:
reload never resets daemon uptime

I3:
only RUNNING can start a new reload

I4:
context contains no ATPD-owned eBPF state

I5:
context does not own XFRM/netlink raw FDs

I6:
context does not own session registry nodes

I7:
subsystem readiness/stats have one authoritative owner

I8:
VPN snapshot fields are internally consistent

I9:
destructive VPN teardown action occurs only according to explicit edge policy

I10:
context initialization cannot silently reset an active daemon
```

---

# 102. 最终验收标准

## Uptime

```text
startup + multiple reloads
→ uptime continuously increases
```

## Reload

```text
RELOADING
→ second reload rejected/serialized
```

## Ownership

```text
session list belongs to session module
XFRM fd belongs to netlink
eBPF belongs to sing-box
errors belong to error subsystem
stats belong to producers
```

## VPN

```text
state/if_id/iface/timestamp snapshot consistent
```

## Kill-switch

```text
no fixed 256 session limit
no context-owned list free
no duplicate destroy path
```

## Context size

应明显减少，而不是增加。

---

# 103. 最终结论

`atpd_context` 当前最大的问题不是代码量，而是：

> 太多 subsystem state 被复制进一个全局 context，开始形成多个“真相来源”。

正确方向不是继续往 `g_atpd_ctx` 塞字段，而是反过来收缩：

```text
ATPD context
    ↓
只保存真正的 daemon-global lifecycle state
```

而：

```text
service
netlink
singbox_api
session
reactor
config
error
```

都各自维护自己的 authoritative state，并通过 snapshot API提供观察。

尤其本轮应优先完成：

```text
1. 修复 reload 重置 uptime
2. 删除 eBPF残余 context state
3. session registry/kill-switch迁回 session
4. 删除 duplicated component readiness/stats
5. 最终减少对 g_atpd_ctx direct field access
```

目标不是消灭 global context，而是让它重新成为：

> 一个小而可信的 ATPD runtime context。
