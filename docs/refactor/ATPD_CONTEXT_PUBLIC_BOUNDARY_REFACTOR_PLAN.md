# ATPD `atpd_context.h / atpd_context.c` 公共边界与全局状态收敛方案

## 1. 结论

当前：

```text
include/atpd_context.h 131 lines
src/atpd_context.c     315 lines
```

这个模块当前已经不是简单的“daemon context”，而是同时承担：

```text
VPN state machine
XFRM fd / iface state
session registry
VPN kill-switch
eBPF state
daemon runtime state
component readiness
statistics
last error
callbacks
global object
```

这与前面已经确定的方向冲突：

> context 应该逐步缩小，而不是继续成为所有模块共享的 global state container。

本轮重点建议：

```text
删除 eBPF ownership
删除 session registry ownership
删除 duplicate error/status/stats
隐藏 g_atpd_ctx
只保留真正 daemon lifecycle + 少量跨模块协调状态
```

---

# 2. 当前 public header 直接暴露完整 struct

当前：

```c
typedef struct {
    ...
} atpd_context_t;

extern atpd_context_t g_atpd_ctx;
```

这意味着任何模块都可以：

```c
g_atpd_ctx.runtime_state = ...
g_atpd_ctx.components.api_ready = ...
g_atpd_ctx.sessions = ...
g_atpd_ctx.vpn_iface[...] = ...
```

完全绕过 owner API。

---

# 3. 这是最大 encapsulation 问题

只要完整 struct 和：

```text
extern g_atpd_ctx
```

继续公开，

所谓：

```text
state transition function
snapshot API
owner thread
```

都无法成为真正 invariant。

---

# 4. 推荐最终隐藏 struct

public header：

```c
typedef struct atpd_context atpd_context_t;
```

如果外部甚至不需要 handle：

可以连 typedef都不要。

只公开：

```text
runtime lifecycle API
snapshot API
```

---

# 5. `g_atpd_ctx` 必须最终删除 public extern

至少：

```c
static atpd_context_t g_ctx;
```

留在 `.c`。

更长期：

```text
由 daemon owner 显式持有
```

而不是 global。

---

# 6. eBPF state 整体删除

当前 header：

```c
typedef enum {
    EBPF_STATE_UNINITIALIZED,
    EBPF_STATE_LOADING,
    EBPF_STATE_READY,
    EBPF_STATE_FAILED,
    EBPF_STATE_DISABLED
} ebpf_state_t;
```

以及：

```text
ebpf_state
ebpf_enabled
ebpf_probed
components.ebpf_ready
atpd_ebpf_state_transition()
ebpf_state_string()
```

全部属于旧 ATPD-owned eBPF model。

---

# 7. 当前架构已经明确

```text
sing-box owns ebpf-in
```

所以 ATPD不应该维护：

```text
自己的 eBPF lifecycle state machine。
```

---

# 8. 如果 status需要 dataplane状态

来源应该是：

```text
sing-box Native API/service snapshot
```

不是：

```text
g_atpd_ctx.ebpf_state
```

---

# 9. 删除完整 eBPF context surface

包括：

```text
ebpf_state_t
ebpf_state
ebpf_enabled
ebpf_probed
components.ebpf_ready
atpd_ebpf_state_transition
ebpf_state_string
```

---

# 10. VPN state 当前也混了太多 ownership

当前 context里：

```text
vpn_state
xfrm_if_id
vpn_iface
vpn_state_since
xfrm_fd
sessions
vpn_teardown_cb
vpn_transitions
splice_bytes_total
vpn_mode_callback
userdata
```

其中至少四个不同 owner。

---

# 11. `xfrm_fd` 应由 netlink/XFRM owner管理

FD 是生命周期资源。

不应放 generic global context。

---

# 12. `sessions` 应由 session manager拥有

当前：

```c
struct atpd_session_list *sessions;
```

context直接成为 session registry。

这和前面 session ownership方案冲突。

---

# 13. `splice_bytes_total`

属于：

```text
session/datapath metrics
```

不应该放 daemon context。

---

# 14. `vpn_transitions`

可以是：

```text
VPN state snapshot metric
```

但应由 VPN/netlink owner统计后 copy-out。

---

# 15. `vpn_mode_callback`

这实际上把 context变成 event bus

当前：

```c
void (*vpn_mode_callback)(...)
void *userdata;
```

这种单 callback slot：

```text
只有一个 subscriber
lifetime隐式
callback ownership不清
```

---

# 16. 更推荐直接 owner-to-owner wiring

例如：

```text
netlink/VPN observer
→ api reconcile callback
```

由 startup显式注册。

如果保留 callback：

至少由 VPN owner持有，

不是 generic context。

---

# 17. `vpn_teardown_cb` 默认绑定 `atpd_vpn_killswitch`

这更明显是错误耦合：

```text
context
→ session manager destructive operation
```

---

# 18. VPN teardown应该发 owner-level event

例如：

```text
vpn state changed to teardown
→ session manager reacts
```

而不是 context自己持 session list并销毁。

---

# 19. 新发现：`atpd_vpn_killswitch()` 当前逻辑存在重复 destroy

当前第一段：

```c
struct atpd_session *session_ptrs[256];
...
for (...) {
    atpd_session_destroy(session_ptrs[i]);
}
```

然后又：

```c
node = g_atpd_ctx.sessions;
while (node) {
    if (node->session) {
        atpd_session_destroy(node->session);
    }
    free(node);
    node = next;
}
```

---

# 20. 结果

前 256 个 session：

```text
destroy called once in first loop
destroy called again in second loop
```

虽然 `atpd_session_mark_closing()` 有状态检查，

但这是：

```text
重复 side effect
重复日志
重复 closed count
```

而且 lifecycle依赖 `destroy()` 的异步行为。

---

# 21. 更严重：context手工 free registry node

session lifecycle本身还有：

```text
GC queue
reactor references
context unregister
```

这里直接：

```c
free(node);
```

会让：

```text
registry ownership
session object ownership
```

彻底混在一起。

---

# 22. 推荐删除整个 killswitch registry实现

改为：

```text
session_manager_close_all(reason)
```

由 session manager内部遍历自己的 registry。

context/VPN owner只调用一次。

---

# 23. 不要固定 256

当前：

```text
session_ptrs[256]
```

只是为了尝试 safe traversal。

这不是正确 ownership方案。

---

# 24. `atpd_session_emergency_drain_all()` 也直接访问 `g_atpd_ctx.sessions`

session.c 当前同样：

```c
struct atpd_session_list *node = g_atpd_ctx.sessions;
```

这说明 session manager和 context互相耦合。

---

# 25. 最终应该反过来

```text
session.c
owns registry

context
does not know session internals
```

---

# 26. `struct atpd_session_list` 不应公开在 context header

当前 public header定义：

```c
struct atpd_session_list {
    struct atpd_session *session;
    struct atpd_session_list *next;
};
```

这是纯 implementation detail。

删除。

---

# 27. `atpd_session_register_to_ctx()` / unregister 删除

改成：

```text
session_manager_register()
session_manager_unregister()
```

或者 session create/destroy内部自动管理。

---

# 28. Runtime state 是 context里最可能保留的核心职责

当前：

```c
typedef enum {
    UNINITIALIZED,
    INITIALIZING,
    RUNNING,
    RELOADING,
    STOPPING,
    STOPPED,
    FAILED
} atpd_runtime_state_t;
```

这部分合理。

---

# 29. 但 RELOADING语义需要收敛

前面 main/config方案已经确定：

```text
reload signal ≠ daemon state immediately becomes RELOADING
```

只有 transaction真正开始：

```text
RELOADING
```

---

# 30. Reload失败但旧 runtime健康

不应该：

```text
FAILED
```

而应该恢复：

```text
RUNNING / DEGRADED
```

并记录：

```text
last_reload_result
```

---

# 31. `atpd_runtime_can_reload()`

当前：

```c
return state == RUNNING ||
       state == RELOADING;
```

这意味着：

```text
已经 RELOADING 时仍然 can_reload
```

语义可疑。

---

# 32. 更合理

如果不支持并发 reload：

```text
RUNNING only
```

RELOADING时：

```text
coalesce / reject new request
```

---

# 33. uptime bug再次确认

当前 transition：

```c
if (new_state == RUNNING) {
    g_atpd_ctx.start_time = time(NULL);
}
```

所以：

```text
startup RUNNING → start_time set
reload RELOADING → RUNNING → start_time重置
```

uptime归零。

---

# 34. `start_time` 必须 one-shot

只在：

```text
daemon lifecycle start
```

设置一次。

---

# 35. uptime使用 wall clock也不理想

当前：

```text
time(NULL) - start_time
```

wall clock可能因：

```text
NTP
manual time change
```

跳变。

---

# 36. 推荐 monotonic uptime

保存：

```c
struct timespec started_mono;
```

getter：

```text
CLOCK_MONOTONIC now - started_mono
```

---

# 37. wall startup timestamp如果 status需要

可以另外保存：

```text
started_wall_epoch
```

两个语义分开。

---

# 38. `uptime_seconds` 不需要缓存

当前：

```text
start_time
uptime_seconds
```

两份状态。

getter每次计算即可。

删除：

```text
atpd_runtime_update_uptime()
uptime_seconds mutable field
```

---

# 39. `last_activity_time`

语义不清。

现在只在：

```text
runtime transition
```

更新。

它并不是：

```text
last daemon activity
```

---

# 40. 建议删除或重命名

如果只是：

```text
state_changed_at
```

就明确叫：

```text
runtime_state_since
```

---

# 41. `reload_count`

可以属于 daemon lifecycle stats。

但应该定义：

```text
attempts
successful reloads
committed generations
```

当前只是字段，没有清晰 contract。

---

# 42. 更推荐 config generation代替一部分 reload_count

例如：

```text
config generation = 1,2,3...
```

比：

```text
reload_count
```

更有诊断价值。

---

# 43. Component readiness是重复状态

当前：

```text
netlink_ready
ebpf_ready
service_ready
api_ready
reactor_ready
```

这些 owner本身都已有状态。

---

# 44. 这会产生双重 truth

例如：

```text
service.state = RUNNING
ctx.components.service_ready = false
```

谁是真实状态？

---

# 45. 推荐删除 generic components readiness

status snapshot直接向 owner获取：

```text
service snapshot
api snapshot
netlink snapshot
reactor state
```

然后：

```text
status builder
```

组合。

---

# 46. 不要在 context做 readiness cache

除非它是严格的：

```text
aggregated immutable status snapshot
```

但那又应该属于 status模块。

---

# 47. `atpd_component_set_ready(const char *name, int ready)` API设计很弱

字符串 dispatch：

```c
strcmp(name, "netlink")
strcmp(name, "ebpf")
...
```

问题：

```text
拼错名字静默无效
编译器无法检查
新增组件要改字符串switch
```

---

# 48. 如果短期保留

至少用：

```c
enum atpd_component
```

但长期建议整体删除。

---

# 49. Generic stats也是 duplicate ownership

当前：

```text
events_processed
timers_fired
signals_received
errors_total
bytes_rx
bytes_tx
```

---

# 50. `events_processed`

应该由：

```text
reactor
```

统计。

---

# 51. `timers_fired`

也是：

```text
reactor
```

---

# 52. `signals_received`

可以由 lifecycle/signal owner统计。

但价值有限。

---

# 53. `errors_total`

已与：

```text
atpd_error_total()
```

重复。

---

# 54. `bytes_rx/bytes_tx`

应由：

```text
session/datapath
```

统计。

---

# 55. 所以 generic context stats大部分删除

status聚合 owner metrics。

---

# 56. 新发现：context里还有第二套 last-error实现

字段：

```c
struct {
    uint32_t last_error_code;
    char last_error_msg[128];
    uint64_t last_error_time;
} last_error;
```

以及：

```text
error_count
stats.errors_total
```

---

# 57. 与 `atpd_error.c` ring重复

项目已经有：

```text
ATPD_ERROR_MAX=128
entries
count
total_count
get_last
```

所以 context这套没有存在价值。

---

# 58. 更明显的是

`atpd_context.c` 实现：

```c
void atpd_error_record(int code, const char *msg);
uint32_t atpd_error_get_last_code(void);
```

但：

```text
atpd_context.h没有声明
atpd_error.h也没有声明
```

---

# 59. 这说明它已经成为半死 legacy API

不要补 declaration。

直接删除。

---

# 60. Error truth统一到 `atpd_error`

status要 last error：

```text
atpd_error_get_last(copy-out)
```

而不是 context cache。

---

# 61. Context init不应该初始化 error subsystem

当前：

```c
void atpd_context_init(void) {
    atpd_error_init();
    memset(&g_atpd_ctx,...);
}
```

这又制造 lifecycle ownership混淆。

---

# 62. 前面 init方案已经要求

每个 subsystem：

```text
有明确 phase owner
```

error diagnostics初始化：

```text
由 startup lifecycle
```

而不是 context偷偷调用。

---

# 63. 更何况 `atpd_error` 当前 static initializer已足够

前面 error review已经建议：

```text
不要 runtime re-init清历史
```

所以 context里的：

```text
atpd_error_init()
```

应删除。

---

# 64. Concurrency模型目前不一致

只有：

```text
vpn_state
```

是 `atomic_int`。

其他：

```text
runtime_state
ebpf_state
components
stats
vpn_iface
callbacks
sessions
```

都是普通字段。

---

# 65. 如果真有跨线程访问

这就是 data races。

---

# 66. 不建议把全部字段改 atomic

那只会把 architecture问题掩盖。

正确做法：

```text
明确 owner thread
通过 copy-out snapshot跨线程
```

---

# 67. `atomic vpn_state` 但关联字段非 atomic也不构成一致 snapshot

例如：

```text
vpn_state = READY
vpn_iface尚未更新
xfrm_if_id还是旧值
```

reader可能看到混合状态。

---

# 68. 所以 atomic单字段并不能解决 VPN snapshot一致性

推荐：

```c
typedef struct {
    vpn_state_t state;
    uint32_t if_id;
    char iface[IFNAMSIZ];
    struct timespec since;
    uint64_t transitions;
} vpn_snapshot_t;
```

由 owner thread copy-out。

---

# 69. 如果跨线程需要同步

用一个 owner mutex保护：

```text
完整 snapshot copy
```

而不是几个独立 atomic。

---

# 70. `vpn_iface[32]`

Linux interface推荐：

```text
IFNAMSIZ
```

而不是 magic 32。

---

# 71. 但更长期这个字段应该不在 context

VPN/netlink owner snapshot里用：

```text
IFNAMSIZ
```

---

# 72. `vpn_state_transition()` callback在 state mutation过程中同步调用

当前：

```text
更新 state
→ callback
→ teardown callback
```

如果 callback反过来访问/修改相关 state：

容易产生 reentrancy复杂度。

---

# 73. Reactor architecture下更推荐

```text
state commit
→ schedule/reconcile next action
```

而不是深层同步 callback chain。

---

# 74. 但第一阶段可以保留同步 callback

前提：

```text
文档说明 owner-thread only
callback不得 destroy owner
```

---

# 75. `atpd_vpn_killswitch()` 名称也不准确

它实际：

```text
close sessions
```

而不是：

```text
kernel kill-switch policy
```

建议不要继续保留该名字。

---

# 76. 如果 teardown时关闭所有 session

叫：

```text
session_close_all_for_vpn_teardown
```

更准确。

---

# 77. Runtime state transition API要做合法转移检查

当前：

```c
g_atpd_ctx.runtime_state = new_state;
```

任何：

```text
STOPPED → RUNNING
FAILED → RELOADING
UNINITIALIZED → STOPPED
```

都可以。

---

# 78. 推荐显式 transition table

至少 debug/assert/log：

```text
legal
illegal
```

---

# 79. 合理大致：

```text
UNINITIALIZED → INITIALIZING
INITIALIZING → RUNNING | FAILED | STOPPING
RUNNING → RELOADING | STOPPING | FAILED
RELOADING → RUNNING | STOPPING
STOPPING → STOPPED
FAILED → STOPPING | STOPPED
```

具体按 lifecycle方案定。

---

# 80. 不要允许 RELOADING → FAILED 仅因为 candidate invalid

old runtime仍可运行。

---

# 81. State string helper可以保留

```text
atpd_runtime_state_string()
```

纯函数、无副作用。

---

# 82. `vpn_state_string()`也可以移到 VPN owner

context不再拥有 VPN state后：

一起移动。

---

# 83. Context最终应该很小

一种可能：

```c
typedef struct {
    atpd_runtime_state_t runtime_state;
    struct timespec started_mono;
    uint64_t started_wall;
    uint64_t config_generation;
} daemon_lifecycle_state_t;
```

甚至不一定叫：

```text
atpd_context
```

---

# 84. 可以直接重命名为 daemon_state

如果最后只剩 lifecycle：

```text
daemon_state.c/h
```

语义更准确。

---

# 85. 但不用现在立刻 rename

先缩责任，

最后再看是否 `atpd_context` 还有存在价值。

---

# 86. Public snapshot推荐

例如：

```c
typedef struct {
    atpd_runtime_state_t state;
    uint64_t uptime_seconds;
    uint64_t config_generation;
} atpd_runtime_snapshot_t;

void atpd_runtime_snapshot(atpd_runtime_snapshot_t *out);
```

---

# 87. Status只读 snapshot

不读取：

```text
g_atpd_ctx.xxx
```

---

# 88. Main只调用 lifecycle intent

例如：

```text
atpd_runtime_begin_startup()
atpd_runtime_mark_running()
atpd_runtime_begin_shutdown()
```

或者一个合法 transition API。

---

# 89. 不要把 transition函数暴露给所有模块

最好只有：

```text
lifecycle owner/main/init
```

使用。

C没有 package-private，

可以通过 header separation：

```text
public snapshot header
internal lifecycle header
```

---

# 90. 不一定要新增 internal header

项目规模小，

也可以只：

```text
main/include对应 declaration
```

但避免所有模块都 include。

---

# 91. 推荐 Commit 1

```text
context: remove obsolete eBPF lifecycle state
```

---

# 92. Commit 2

```text
session: move registry and close-all ownership out of context
```

顺带修 killswitch重复 destroy。

---

# 93. Commit 3

```text
context: remove duplicate error history and statistics
```

---

# 94. Commit 4

```text
status: derive readiness and metrics from subsystem snapshots
```

删除 components cache。

---

# 95. Commit 5

```text
context: make daemon uptime monotonic and one-shot
```

修 reload uptime reset。

---

# 96. Commit 6

```text
context: enforce legal runtime state transitions
```

---

# 97. Commit 7

```text
context: hide global context representation
```

删除：

```text
extern g_atpd_ctx
```

---

# 98. Commit 8

```text
context: expose immutable runtime snapshot only
```

---

# 99. 不建议加入更多原子变量

不要修成：

```text
atomic_runtime_state
atomic_error_count
atomic_api_ready
atomic_service_ready
...
```

这会形成一个“atomic god object”。

---

# 100. 不建议 context接管 config store

前面的 config方案已经明确：

```text
config owner/store
```

独立。

不要因为删 g_config就把：

```text
atp_config_t config
```

塞进 context。

---

# 101. 不建议 context接管 service/api/netlink pointers

同样会重新制造：

```text
atpd_global v2
```

---

# 102. 最终 context只保留真正 cross-cutting lifecycle状态

越少越好。

---

# 103. Tests：no eBPF context

```bash
grep -R 'ebpf_state\|ebpf_enabled\|ebpf_probed\|ebpf_ready' include/atpd_context.h src/atpd_context.c
```

目标：

```text
0
```

---

# 104. Test：no global extern

```bash
grep -R 'extern atpd_context_t g_atpd_ctx' include src
```

目标：

```text
0
```

---

# 105. Test：no session registry in context

```text
sessions
atpd_session_list
register_to_ctx
unregister_from_ctx
```

目标：

```text
0
```

在 context模块。

---

# 106. Test：VPN teardown

创建：

```text
>256 sessions
```

触发 teardown。

每个 session：

```text
exactly one close request
no UAF
no double free
```

---

# 107. Test：reload uptime

```text
start
sleep
reload
```

uptime：

```text
monotonic increasing
```

不能 reset。

---

# 108. Test：wall-clock change

修改系统时间/NTP模拟。

uptime不倒退。

---

# 109. Test：illegal transition

例如：

```text
STOPPED → RELOADING
```

必须：

```text
reject/assert/log
```

而不是成功。

---

# 110. Test：reload reentry

RELOADING期间再次请求 reload：

```text
coalesce/reject
```

不能启动第二 transaction。

---

# 111. Test：last error single truth

发生一次 service failure：

```text
atpd_error ring更新
context无第二份last_error
```

---

# 112. Test：component readiness

status显示 service/API/netlink readiness：

```text
来自各自 owner snapshot
```

不是 context cache。

---

# 113. Test：TSan

重点：

```text
runtime snapshot readers
VPN transition/readers
session close-all
```

不应有 data race。

---

# 114. Final invariants

```text
I1:
The context representation is not publicly mutable.

I2:
No ATPD-owned eBPF lifecycle state remains in context.

I3:
Session registry and destruction are owned by the session subsystem.

I4:
VPN teardown never double-destroys or manually frees session ownership nodes from context.

I5:
Runtime uptime is monotonic and never resets after reload.

I6:
Component readiness is sourced from subsystem owners, not duplicated in context.

I7:
Error history has one authoritative owner: atpd_error.

I8:
Generic statistics are not duplicated across context and subsystem owners.

I9:
Runtime state transitions are validated.

I10:
Cross-thread readers consume consistent snapshots rather than unrelated atomic/scalar fields.

I11:
g_atpd_ctx is not a public global API.

I12:
Context never becomes a replacement for atpd_global.
```

---

# 115. 最终结论

`atpd_context` 当前真正的问题已经不是 315 行，而是 ownership。

它现在同时保存：

```text
VPN
XFRM fd
sessions
eBPF
daemon state
component readiness
metrics
errors
callbacks
```

几乎就是第二个：

```text
atpd_global
```

而且已经出现实际后果：

```text
reload重置 uptime
duplicate eBPF state
duplicate error truth
duplicate readiness
session registry越权
VPN killswitch重复 destroy
```

最好的重构方向不是“把 context 做得更完整”，而是反过来：

> 让 service/session/netlink/API/error/status 各自重新拿回自己的状态，context最终只剩少量 daemon lifecycle state，并通过只读 snapshot 暴露。

这样才能避免前面删掉 `atpd_global` 之后，又无意中造出一个新的 god object。
