# ATPD 初始化 / Shutdown / Rollback 生命周期加固方案

涉及文件：

```text
src/atpd_init.c
include/atpd_init.h
src/cleanup.c
include/cleanup.h
src/main.c
```

目标不是增加更多 cleanup 函数，而是建立一条唯一、可证明、幂等的 daemon 生命周期。

---

# 1. 总体结论

当前 ATPD 已经有“分阶段初始化”的正确雏形：

```text
CONFIG
LOGGER
EBPF
NETLINK
SERVICE
API
READY
```

也已经有：

```text
atpd_init_run()
atpd_init_rollback()
```

但是实际程序同时又在：

```text
main.c
do_start()
run_event_loop()
```

维护另一套初始化、启动、停止和释放逻辑。

因此当前真实结构是：

```text
main
├─ pre-load config
├─ init context
│
└─ do_start
   ├─ init context again
   ├─ atpd_init_run
   │  └─ rollback system A
   │
   └─ run_event_loop
      ├─ reactor/netlink/UDS/service start
      └─ cleanup system B
```

这会导致：

```text
谁拥有资源？
谁负责 rollback？
某资源是否已经 init？
cleanup 能否重复？
```

都难以回答。

本轮核心目标：

> 一个资源只能有一个 lifecycle owner；启动失败与正常 shutdown 走同一套逆序 teardown primitives。

---

# 2. 已确认问题：`atpd_context_init()` 被调用两次

`main()` 在 command dispatch 前调用：

```c
atpd_context_init();
```

之后 `CMD_START` 进入 `do_start()`，又调用：

```c
atpd_context_init();
```

而当前 context init 会重置整个 global context。

这意味着：

```text
main 初始化一次
↓
do_start 再 memset/reset一次
```

即使今天第一次 context 中尚未拥有很多 active resource，这也是错误 lifecycle。

我们前面的 context 方案已经要求：

```text
context init = one-shot
```

因此必须修。

---

# 3. 修复方式

建议：

```text
main
```

不要无条件初始化 daemon runtime context。

只有：

```text
CMD_START
```

真正启动 daemon 时初始化。

例如：

```text
main:
parse args
load minimal command config
switch command

CMD_START:
    atpd_context_init()
    do_start()
```

或者：

```text
do_start()
```

作为唯一 context initializer。

只能选一个。

推荐：

> `do_start()` 是 daemon lifecycle 的唯一入口。

---

# 4. 配置也被加载两次

当前：

```text
main()
    config_set_defaults
    config_load

do_start()
    atpd_init_run()
        INIT_PHASE_CONFIG
            config_load again
```

即：

```text
同一个配置文件
至少在 start command 路径读取两遍
```

这不仅浪费。

更麻烦的是未来 transactional/presence/source-path 语义加入后：

```text
第一次 parse state
第二次 parse state
```

可能产生重复 side effect 或 source metadata 被覆盖。

---

# 5. 配置生命周期应分 command config 与 daemon config

推荐：

### 对 CMD_START

只：

```text
config_init/defaults
↓
atpd_init_phase_config
```

一次。

### 对 CMD_STATUS / CHECK 等 standalone command

按需要：

```text
load standalone config
```

不要为了所有 command 统一而让 start 重复 load。

---

# 6. `config_set_defaults()` 还包含 mutex init

这使重复初始化尤其危险。

虽然当前 start 路径不是直接重复调用 defaults，但未来重构时很容易：

```text
config_set_defaults
→ config_set_defaults
```

造成 mutex double-init。

推荐配合 config 方案：

```c
config_init()
config_load()
config_destroy()
```

把 lifecycle 写清。

---

# 7. 已确认 P0/P1：service rollback 异步 stop 后立即 free

当前：

```c
service_stop_async(ctx->service, NULL, NULL);
free(ctx->service);
ctx->service = NULL;
```

这是非常危险的。

因为：

```text
service_stop_async
```

从名字和当前 supervisor architecture 看，就是异步 operation。

它可能仍然依赖：

```text
service_ctx_t
timer
reactor callback
child state
```

随后立刻：

```text
free(service_ctx)
```

存在潜在：

```text
UAF
timer callback on freed ctx
SIGCHLD callback on freed ctx
```

---

# 8. Rollback 绝不能“发起异步 stop 然后 free”

必须选一种明确模型：

### 方案 A：rollback 使用同步 teardown primitive

推荐 startup failure阶段采用：

```c
service_abort_init(service);
```

保证返回前：

```text
timers detached
child stopped/reaped
reactor detached
all service-owned resources released
```

然后才：

```text
free(service)
```

### 方案 B：整个 shutdown 也是异步状态机

复杂得多，不适合 startup rollback。

所以这里推荐 A。

---

# 9. `service_abort_init()` 与正常 shutdown 可以共享底层 primitive

不要复制一套 stop逻辑。

例如：

```c
int service_quiesce(service_ctx_t *ctx);
int service_stop_owned_child_sync(service_ctx_t *ctx);
void service_destroy(service_ctx_t *ctx);
```

startup rollback：

```text
quiesce
stop/reap
destroy
```

normal shutdown也用同样 primitive。

---

# 10. `main.c` 已经又实现了一份 `service_stop_sync()`

当前 `main.c` 自己直接操作 service internals：

```text
monitor_timer
retry_timer
health_timer
reactor
child_pid
validated_pid
state
pidfile
kill
waitpid
```

这说明 service lifecycle ownership已经泄漏到 main。

这是需要删除的。

---

# 11. `main.c` 不应知道 service timer字段

最终：

```c
service_stop_sync(...)
```

应该由：

```text
service.c / service supervisor
```

提供 public API。

main只调用：

```c
service_shutdown(g_svc);
```

不能自己 cancel：

```text
monitor_timer
retry_timer
health_timer
```

---

# 12. `main.c` 不应直接 kill/reap service child

同理：

```text
kill(SIGTERM)
waitpid
kill(SIGKILL)
```

必须由唯一 service child owner执行。

否则前面 service lifecycle方案中：

```text
generation
PID starttime
owned child
SIGCHLD
```

都会被绕过。

---

# 13. 初始化 phase rollback 起点存在语义问题

当前：

```c
for (int i = phase; i >= 0; i--)
```

即从：

```text
失败的 phase 本身
```

开始 rollback。

但是失败 phase可能：

```text
完全没初始化
部分初始化
已经自己 rollback
```

三种情况都有。

因此：

```text
switch phase name
```

不足以知道资源状态。

---

# 14. 必须记录“成功初始化”的 phase

推荐：

```c
uint64_t initialized_mask;
```

只有 phase 完整成功：

```text
mark initialized
```

rollback：

```text
reverse iterate only initialized phases
```

对于 phase 内部 partial initialization：

它自己的 handler必须在返回失败前：

```text
rollback partial resources
```

或者 phase提供：

```text
prepare + cleanup
```

---

# 15. 更推荐 phase descriptor带 cleanup

例如：

```c
typedef struct {
    init_phase_t phase;
    const char *name;

    int (*init)(atpd_init_context_t *);
    void (*cleanup)(atpd_init_context_t *);

    bool required;
} init_phase_config_t;
```

然后：

```text
init成功
→ push phase

失败
→ reverse cleanup successful stack
```

不再在：

```text
atpd_init_rollback()
```

写巨大的 switch。

---

# 16. Cleanup 与 init 应保持局部对应

结构：

```text
config_init
config_cleanup

logger_init
logger_cleanup

netlink_init
netlink_cleanup

service_init
service_cleanup

api_init
api_cleanup
```

但是：

> cleanup必须代表真正完整的 owner teardown，而不是只有函数名。

---

# 17. EBPF phase 应整体删除

根据已经确认的架构：

```text
sing-box owns ebpf-in
```

因此：

```text
INIT_PHASE_EBPF
atpd_init_phase_ebpf
#include "ebpf.h"
Pure eBPF Engine probe log
```

都应配合：

```text
ATPD_EBPF_MODULE_REMOVAL_PLAN
```

删除。

---

# 18. READY 文案也应该更新

当前：

```text
Pure eBPF Environment ready
Pure eBPF Reactor running
Engine: Pure eBPF active
```

这些文案已经不符合新的 ownership。

推荐：

```text
ATPD runtime ready
sing-box supervisor initialized
reactor running
```

如果要提 eBPF：

明确：

```text
traffic interception: sing-box ebpf-in
```

---

# 19. `cleanup.c` 当前不是 cleanup subsystem

当前实际行为：

```text
register config pointer
atexit(handler)

handler:
    LOG_DEBUG(...)
```

并没有：

```text
service cleanup
netlink cleanup
UDS cleanup
API cleanup
reactor cleanup
PID file cleanup
session cleanup
```

因此：

```text
cleanup.c
```

这个名字严重高估了它的职责。

---

# 20. `atp_cleanup_all()` 目前也是假语义

名字表示：

```text
cleanup all ATP resources
```

实际：

```text
只调用一个打印日志的 handler
```

这和前面发现的 no-op success API属于同一类问题：

> API名字承诺了不存在的行为。

---

# 21. 推荐删除 `cleanup.c`

如果 eBPF旧架构删除后：

```text
atp_register_cleanup()
atp_cleanup_all()
atp_cleanup_manual()
```

没有真实职责：

建议整个：

```text
src/cleanup.c
include/cleanup.h
```

删除。

不要把真正 daemon teardown重新塞进 atexit handler。

---

# 22. 为什么不推荐用 `atexit()` 做 daemon teardown

长期 daemon 的 teardown必须控制顺序：

```text
stop accepting work
↓
cancel async work
↓
stop service
↓
drain/reap children
↓
cleanup UDS/netlink/API
↓
destroy reactor
↓
close pid lock
```

而：

```text
atexit
```

不能很好表达：

```text
部分初始化状态
异步状态
reactor依赖
child ownership
error handling
```

所以正常生命周期应该显式 teardown。

---

# 23. `atexit` 最多作为 emergency last-resort

即使保留：

也只做：

```text
best-effort non-blocking primitive cleanup
```

绝不能成为 correctness依赖。

例如：

```text
unlink own temporary marker
```

而不是：

```text
异步 stop child
reactor操作
```

---

# 24. 初始化 Netlink 的 reactor 语义混乱

`atpd_init_phase_netlink()`：

```c
netlink_init(NULL, config);

if (ctx->reactor) {
    netlink_set_reactor(ctx->reactor);
    netlink_xfrm_init(ctx->reactor);
}
```

但 `do_start()` 创建 `init_ctx` 时：

```text
reactor = NULL
```

因为 reactor直到：

```text
run_event_loop()
```

才创建。

所以 startup phase实际上只是：

```text
netlink_init(NULL)
```

然后 event loop里再次：

```text
reactor_add_fd
netlink_set_reactor
netlink_xfrm_init
```

---

# 25. 这说明 init phase划分不准确

Netlink实际上有两阶段：

```text
NETLINK CORE INIT
REACTOR ATTACH
```

应该明确：

```c
netlink_init(config);
netlink_attach_reactor(reactor);
```

不要让同一个：

```text
netlink_init
```

同时接受 nullable reactor。

---

# 26. Reactor应该更早成为正式生命周期 phase

当前 reactor在：

```text
run_event_loop()
```

临时创建。

但很多 subsystem：

```text
netlink
service
api
UDS
async validation
```

都依赖 reactor。

因此推荐启动顺序：

```text
CONFIG
LOGGER
CONTEXT
REACTOR
NETLINK
SERVICE
API
UDS
SERVICE_START
READY
RUN
```

---

# 27. 为什么 reactor 应进入 init transaction

因为当前如果：

```text
reactor_create失败
```

`run_event_loop()`只是：

```text
LOG_ERROR
return
```

然后 `do_start()`仍然：

```text
ret = 0
```

这是明确 correctness bug。

---

# 28. 已确认：reactor 创建失败可能导致 start 返回成功

当前：

```c
run_event_loop();   // void
ret = 0;
```

而 `run_event_loop()`：

```c
if (!g_reactor) {
    LOG_ERROR(...);
    return;
}
```

所以：

```text
reactor_create失败
↓
event loop没运行
↓
do_start返回0
```

CLI/launcher看到：

```text
start success
```

实际上 daemon没有正常工作。

这是 P0/P1。

---

# 29. `run_event_loop()` 必须返回 int/result

例如：

```c
int run_event_loop(void);
```

失败：

```text
return -1
```

`do_start()`：

```text
ret = run_event_loop() == 0 ? 0 : 1;
```

但更推荐：

> reactor setup直接进入 init transaction，不再由 run_event_loop同时负责初始化。

---

# 30. signal watch 失败当前只是 WARN

对于：

```text
SIGTERM
SIGINT
SIGCHLD
```

这些并不是可随便缺失的。

如果关键 signal注册失败：

startup应该失败。

至少分类：

```text
required signals:
SIGTERM
SIGINT
SIGCHLD

optional:
SIGUSR1
SIGHUP?（如果 reload必须支持也应 required）
```

---

# 31. `reactor_add_fd(netlink)` 返回值没检查

当前：

```c
reactor_add_fd(g_reactor, nl_fd, ...);
```

失败后继续 startup。

这是我们 reactor/netlink方案已确认的 false registration / state divergence问题。

必须检查。

---

# 32. `netlink_set_reactor()` / `netlink_xfrm_init()` 返回值要处理

不能：

```text
调用
忽略
继续 ready
```

尤其 XFRM可以是 optional capability：

```text
DEGRADED
```

但状态必须真实。

---

# 33. UDS init failure当前是 WARN

需要明确产品要求。

如果 UDS 是：

```text
status
stop/control
```

的主要 daemon control API：

建议：

```text
required
```

startup失败。

如果 CLI有可靠 fallback：

可以 optional。

但必须在 status显示：

```text
UDS unavailable
```

不要悄悄运行。

---

# 34. 已确认的危险路径：service start失败时先 destroy reactor

当前：

```c
if (service_start_async(g_svc) < 0) {
    reactor_destroy(g_reactor);
    g_reactor = NULL;
    return;
}
```

但在这之前已经：

```text
netlink attached
XFRM attached
UDS initialized
service set_reactor
```

---

# 35. 这可能制造 dangling reactor pointer

尤其：

```text
UDS global保存 reactor pointer
service ctx保存 reactor pointer
netlink保存 reactor pointer
```

先：

```text
reactor_destroy
```

而后续 cleanup才发生。

如果 cleanup函数尝试：

```text
reactor_remove_fd(old_pointer,...)
```

就是潜在 UAF。

---

# 36. 原则：永远先 detach owner，再 destroy dependency

依赖图：

```text
UDS ───────┐
Netlink ───┤
Service ───┤→ Reactor
API ───────┘
```

shutdown必须：

```text
UDS detach
Netlink detach
Service detach/cancel timers
API detach
↓
Reactor destroy
```

不能反过来。

---

# 37. Reactor destruction 应接近最后

只要还有模块可能：

```text
remove fd
cancel timer
post callback
```

reactor就必须活着。

---

# 38. Shutdown顺序建议

推荐：

```text
1. runtime state → STOPPING
2. stop accepting new control/reload
3. cancel active async validation
4. UDS stop accepting clients
5. service supervisor quiesce
6. stop/reap sing-box child
7. session close_all + final GC
8. API detach/cleanup
9. Netlink/XFRM detach/cleanup
10. UDS final cleanup
11. reactor destroy
12. service/context object destroy
13. pid file unlock/unlink
14. logger flush/close
15. runtime state → STOPPED
```

具体 6–10 顺序根据 callback依赖可调整。

---

# 39. STOPPED 状态不能在资源仍存活时设置

当前 `on_idle()`：

```c
if (!g_running) {
    atpd_runtime_state_transition(STOPPED);
    reactor_stop(r);
}
```

但真正：

```text
UDS
service child
reactor
netlink
API
sessions
```

都还没 cleanup。

所以此时：

```text
runtime state = STOPPED
```

是假的。

---

# 40. 正确状态

收到 shutdown：

```text
RUNNING
→ STOPPING
```

完成所有 teardown后：

```text
→ STOPPED
```

---

# 41. `on_signal()` 当前没有进入 STOPPING

SIGTERM/SIGINT：

```text
g_running = 0
reactor_stop
```

应该同时：

```text
runtime transition(STOPPING)
```

但不要进入 STOPPED。

---

# 42. Reactor stop与 teardown

`reactor_stop()`的语义应该只是：

```text
退出 event loop
```

不是：

```text
资源已经清理
```

这两个概念必须区分。

---

# 43. Session GC 必须 final drain

当前 event loop结束：

```c
atpd_session_gc_process(g_reactor);
```

只有一次。

如果 service/network cleanup随后又触发：

```text
session close
```

可能产生新的 GC work。

因此 shutdown需要：

```text
session_close_all
↓
process GC until empty / deterministically destroy
```

配合 session方案实现。

---

# 44. API callback必须在 context/userdata释放前 unregister

init：

```c
atpd_set_vpn_mode_callback(api_vpn_mode_callback, ctx->api);
```

cleanup：

```text
必须先清 callback
再 api_cleanup/free
```

否则之后 VPN transition可能 callback到失效 userdata。

---

# 45. `api_init()` 返回值当前完全忽略

`atpd_init_phase_api()`：

```c
api_init(...)
atpd_set_vpn_mode_callback(...)
api_start_with_reactor(...)
return 0;
```

任何失败都没有传播。

如果 API真的是 optional：

也应该：

```text
return error
```

让 phase runner根据：

```text
required=false
```

记录 degraded。

不能 handler永远成功。

---

# 46. Optional phase失败也要记录状态

当前 optional：

```text
LOG_WARN
continue
```

但没有：

```text
degraded mask
```

最终 READY仍然说：

```text
all components initialized
```

这是 misleading。

---

# 47. READY 应区别 READY 与 DEGRADED

例如：

```text
ATPD ready
Warnings:
- Native API unavailable
- XFRM unavailable
```

不要：

```text
all components initialized
```

---

# 48. `skip_on_failure` 当前似乎没有使用

phase struct：

```c
int skip_on_failure;
```

但 runner没有使用它控制流程。

这是 dead field / incomplete design。

应：

```text
删除
```

或真正实现。

推荐删除，保持：

```text
required
```

足够。

---

# 49. Logger phase失败处理不完整

例如：

```text
mkdir_recursive(log_dir)
```

返回值忽略。

```text
log_set_file(log_path)
```

返回值似乎也没检查。

如果 logger file不能创建：

需要明确：

```text
fallback stderr
or
startup fail
```

不能无条件算 phase成功。

---

# 50. Config phase调用 `atp_register_cleanup()`

因为 `cleanup.c`实际上不做资源清理，这个注册应删除。

config ownership与 atexit无关。

---

# 51. PID lock FD 当前没有正常 close

`g_pid_fd` open并加 write lock。

正常 do_start cleanup：

```text
unlink(pp)
```

但没有显式：

```text
close(g_pid_fd)
```

最终 process退出当然会自动关闭。

但显式 lifecycle应该：

```text
close
unlock
set -1
```

然后 unlink。

---

# 52. 为什么显式 close重要

如果未来：

```text
start/stop生命周期用于测试
embedded test harness
restart without exec
```

依赖 process exit回收会出问题。

同时它让 resource tests更清楚。

---

# 53. PID file的 unlink应该由 PID owner负责

不要散在：

```text
do_start
do_stop
service stop
```

ATPD自己的 PID file：

```text
daemon instance owner
```

sing-box PID file：

```text
service owner
```

完全分开。

---

# 54. `write_pid_file()` 的 fsync错误被忽略

当前：

```c
fsync(g_pid_fd);
return 0;
```

如果 pidfile durability/visibility重要：

检查返回。

但它不是本轮最高优先级。

---

# 55. Daemonize 在 PID file之前是合理的

当前：

```text
daemonize
↓
write PID file
```

PID内容是最终 child PID。

这是正确的。

---

# 56. 但 daemonize中的 `exit()` 会触发 atexit handler

如果 parent process已经注册复杂 atexit cleanup：

就可能意外执行 daemon teardown。

目前 cleanup handler只是log所以问题不大。

这也是为什么：

> 不应把 daemon correctness依赖放进 atexit。

---

# 57. Normal `do_start()` exit code

必须区分：

```text
startup failure
runtime fatal failure
clean SIGTERM shutdown
```

例如：

```text
clean shutdown → 0
startup failure → nonzero
fatal reactor error → nonzero
```

方便 service manager判断。

---

# 58. `reactor_run()` 返回值/退出原因

如果当前 reactor_run是 void：

建议增加：

```text
exit reason
```

至少能区分：

```text
reactor_stop requested
fatal epoll failure
```

如果不改API：

main维护：

```text
shutdown_requested
fatal_runtime_error
```

也可以。

---

# 59. SIGHUP reload失败不应把整个 runtime state永久设 FAILED

当前：

```text
config_reload failed
→ runtime_state = FAILED
```

但 daemon可能：

```text
旧配置仍然正常运行
```

完成 transactional reload后应该：

```text
reload失败
→ config last_reload=FAILED
→ daemon保持 RUNNING/DEGRADED
```

不是整个 daemon FAILED。

---

# 60. 这与 config transaction方案必须同步

如果 old config仍有效：

```text
reload error ≠ daemon fatal error
```

所以 runtime FSM：

```text
RELOADING
→ RUNNING
```

即使 reload失败。

同时：

```text
config snapshot记录 failure
```

---

# 61. `api_init()` 在 reload里再次直接调用

当前：

```text
service_apply_config
api_init
```

说明 main自己又成为 config apply coordinator。

配置 transactional方案完成后：

全部应该移出 main：

```text
config_reload_transaction()
```

main只请求 reload。

---

# 62. 主循环应保持 orchestration薄层

理想：

```c
on_signal()
    request_reload / request_shutdown

on_idle()
    process requested operations
```

而不是自己实现：

```text
config apply
service config
API init
teardown internals
```

---

# 63. 推荐新的 init phase划分

删除 eBPF后：

```text
INIT_CONFIG
INIT_LOGGER
INIT_REACTOR
INIT_NETLINK
INIT_SERVICE
INIT_API
INIT_UDS
START_SERVICE
READY
```

是否把：

```text
SERVICE_INIT
SERVICE_START
```

分开非常有价值。

---

# 64. 为什么 service init/start要分开

`service_init` 只是建立 supervisor object。

`service_start` 会产生：

```text
child
timer
PID
health monitor
```

rollback复杂度不同。

显式分开更容易测试。

---

# 65. API也可以分 init / attach reactor

类似：

```text
api_init(config)
api_attach_reactor(reactor)
```

避免 nullable reactor参数和隐式partial state。

---

# 66. Init transaction状态

建议：

```c
typedef struct {
    uint64_t completed_phases;
    uint64_t degraded_phases;

    bool shutdown_started;
} atpd_init_context_t;
```

不需要过度复杂。

---

# 67. Phase runner伪代码

```c
for each phase:
    rc = phase.init(ctx)

    if rc == OK:
        mark completed
        continue

    if phase.required:
        rollback_completed_reverse()
        return failure

    mark degraded
```

注意：

```text
failed phase自己的 partial state
```

应由其 handler内部清理。

---

# 68. 更稳的 phase handler契约

```text
return 0:
phase owns fully initialized resource

return !=0:
phase leaves no owned resource behind
```

这是最简单的。

这样 generic rollback只处理：

```text
previous successful phases
```

---

# 69. Cleanup也必须返回/记录错误吗

正常 shutdown时：

```text
best effort继续执行所有 teardown
```

不能因为一个：

```text
netlink cleanup fail
```

就跳过：

```text
service/reacctor cleanup
```

因此 cleanup：

```text
record first/worst error
continue
```

---

# 70. Teardown必须幂等

每个 public cleanup API：

```text
called once
called twice
called after partial init
```

都不能：

```text
double close
double free
kill reused PID
```

---

# 71. 典型 idempotent pattern

```c
if (fd >= 0) {
    close(fd);
    fd = -1;
}

if (ptr) {
    destroy(ptr);
    ptr = NULL;
}
```

但对于：

```text
reactor registration
timers
child PID
```

还需要明确 registration/ownership flag，不能只靠数值。

---

# 72. Resource ownership matrix

Codex修改前必须建立：

```text
resource
created by
owner
depends on
rollback function
normal shutdown function
idempotent?
```

至少：

```text
config mutex/storage
logger fd
reactor
signal fd/event fd
netlink fd
XFRM fd
service ctx
service child PID
service timers
API state/connections/timers
UDS listener
UDS clients
sessions
async validator children
ATPD pid lock fd
ATPD pidfile path
callbacks
```

---

# 73. Dependency graph

至少建立：

```text
Reactor
↑
├─ Netlink
├─ XFRM
├─ Service timers
├─ API
├─ UDS
├─ Sessions
└─ Async validate
```

所以：

```text
Reactor destroy
```

天然接近 teardown最后。

---

# 74. Test：context init exactly once

启动命令：

instrument：

```text
atpd_context_init count == 1
```

---

# 75. Test：config load once

start：

```text
config_load count == 1
```

除非有明确 preflight parse设计。

---

# 76. Test：failure after every init phase

故障注入：

```text
CONFIG fail
LOGGER fail
REACTOR fail
NETLINK fail
SERVICE init fail
API fail
UDS fail
SERVICE start fail
```

每次验证：

```text
0 leaked FD
0 child
0 timer
0 UDS socket
0 stale reactor pointer
PID file removed
```

---

# 77. 这是本模块最重要的 test matrix

每个阶段都：

```text
inject failure immediately after resource creation
```

确认 rollback。

不要只测 phase入口失败。

---

# 78. Test：service async start partial failure

例如：

```text
child spawned
timer creation fails
```

startup rollback必须：

```text
child terminate/reap
timer cleanup
service object safe destroy
```

---

# 79. Test：UDS success → service start fail

这是当前危险路径的直接回归测试。

验证：

```text
UDS先 detach
再 reactor destroy
```

不能出现：

```text
uds_cleanup访问 destroyed reactor
```

---

# 80. Test：netlink attached → later phase fail

验证：

```text
netlink/xfrm unregister
close
clear reactor pointer
```

然后才 destroy reactor。

---

# 81. Test：API callback registered → API cleanup

cleanup后触发一个模拟 VPN transition。

不能 callback旧 userdata。

---

# 82. Test：double shutdown

```text
shutdown()
shutdown()
```

第二次：

```text
no-op/safe
```

不：

```text
double free
double kill
double close
```

---

# 83. Test：SIGTERM during startup

真实 daemon可能在：

```text
SERVICE_START
```

阶段收到 stop。

需要：

```text
abort startup
rollback
clean exit
```

至少不能崩溃。

---

# 84. Test：SIGTERM during reload

配合 transactional reload：

```text
cancel reload
abort candidate
shutdown old runtime
```

不能：

```text
commit half config
```

---

# 85. Test：SIGTERM with active async validation

配合 async_validate方案：

```text
cancel child
reap
then reactor destroy
```

---

# 86. Test：SIGCHLD during shutdown

service stop与 signal callback交错。

必须保证：

```text
child reaped once
```

---

# 87. Test：reactor_create failure

预期：

```text
atpd start returns non-zero
PID file removed
```

这是当前明确缺陷的回归测试。

---

# 88. Test：required signal watch failure

预期：

```text
startup fails cleanly
```

---

# 89. Test：optional API failure

如果 API确定 optional：

预期：

```text
daemon starts
status = DEGRADED
Native API unavailable
```

而不是：

```text
all components initialized
```

---

# 90. Test：clean normal shutdown

检查：

```text
runtime:
RUNNING → STOPPING → STOPPED
```

并且：

```text
STOPPED only after final cleanup
```

---

# 91. Test：reload failure

transactional reload失败：

```text
runtime最终仍 RUNNING
old config active
last reload failed
```

不能：

```text
daemon state FAILED
```

---

# 92. Test：PID lock FD

startup/shutdown循环：

```text
1000 cycles
```

检查：

```text
pid lock fd no leak
pid file removed
next start can lock
```

---

# 93. Test：resource stress

至少：

```text
1000 start/stop cycles
```

测试环境可用 fake sing-box。

统计：

```text
FD
RSS
zombies
socket files
pidfiles
```

---

# 94. 推荐 Commit 1

```text
init: make context and config initialization single-owner
```

内容：

- context init once
- start config load once
- command-specific config path
- tests

---

# 95. Commit 2

```text
init: make reactor a first-class startup phase
```

内容：

- reactor create进入 init
- required signal registration
- check all add/watch returns
- reactor failure propagates to start exit code

---

# 96. Commit 3

```text
init: replace phase switch rollback with registered cleanup
```

内容：

- phase cleanup callbacks
- completed mask/stack
- failed phase contract
- reverse teardown

---

# 97. Commit 4

```text
service: centralize shutdown ownership
```

内容：

- 删除 main.c `service_stop_sync`
- service module提供真实 sync shutdown/destroy
- rollback不再 async-stop + immediate free

---

# 98. Commit 5

```text
runtime: order dependency teardown before reactor destruction
```

内容：

- UDS/netlink/API/service detach
- sessions/async validation drain
- reactor最后 destroy
- service-start-failure regression

---

# 99. Commit 6

```text
cleanup: remove obsolete atexit cleanup shim
```

删除：

```text
cleanup.c
cleanup.h
atp_register_cleanup
atp_cleanup_all
atp_cleanup_manual
```

前提：

全仓确认无真实剩余职责。

---

# 100. Commit 7

```text
init: remove obsolete ebpf startup phase
```

配合 eBPF removal plan：

- INIT_PHASE_EBPF
- probe
- old logs
- config ready state

---

# 101. Commit 8

```text
runtime: report READY vs DEGRADED truthfully
```

- optional phases真实 error propagation
- degraded mask/snapshot
- READY文案修正

---

# 102. Commit 9

```text
runtime: make shutdown state truthful
```

- signal → STOPPING
- STOPPED only after teardown
- reload failure恢复 RUNNING
- fatal runtime error单独处理

---

# 103. 不建议拆 `atpd_init.c`

217行非常小。

重构后即使加入：

```text
phase cleanup descriptor
completed mask
```

仍不会太大。

而 `cleanup.c`：

> 更可能应该删除，而不是扩展。

---

# 104. `main.c` 应进一步变薄

本轮不是全面重构 main，但生命周期完成后：

main应只剩：

```text
parse command
dispatch
start daemon
reactor run
request reload
request shutdown
```

不再直接操作：

```text
service timer
service PID
Netlink FD internals
API re-init
resource cleanup internals
```

---

# 105. 推荐最终启动结构

```text
main
↓
daemon_start()
    CONFIG
    LOGGER
    CONTEXT
    REACTOR
    NETLINK
    SERVICE
    API
    UDS
    SERVICE_START
    READY
↓
reactor_run()
↓
daemon_shutdown()
```

---

# 106. 推荐最终 shutdown结构

```text
daemon_shutdown()
    state → STOPPING

    stop new work
    cancel validation
    stop UDS accept
    quiesce service
    stop/reap child
    close sessions + GC
    api cleanup
    netlink cleanup
    uds cleanup
    reactor destroy

    free service
    close pid lock
    unlink pid file
    logger flush

    state → STOPPED
```

---

# 107. Startup failure

无论失败在哪：

```text
daemon_start()
↓
reverse cleanup successful phases
↓
return nonzero
```

它与正常 shutdown共享：

```text
相同 cleanup primitive
```

这是关键。

---

# 108. 最终 Invariants

Codex最终必须保证：

```text
I1:
atpd_context_init occurs exactly once per daemon start

I2:
daemon config is loaded exactly once during startup

I3:
a startup phase returning failure owns no partial resource

I4:
only successfully completed phases are rolled back

I5:
service context is never freed while async callbacks/timers can still reference it

I6:
all reactor dependents detach before reactor destruction

I7:
reactor creation/setup failure makes `atpd start` fail

I8:
STOPPED is entered only after teardown is complete

I9:
reload failure does not mark a healthy old runtime as globally FAILED

I10:
all cleanup primitives are idempotent

I11:
every child process has one reap owner

I12:
normal shutdown and startup rollback use the same resource-release primitives
```

---

# 109. 最终验收标准

## Failure injection

每个 startup phase：

```text
fail before
fail after partial resource creation
```

结果：

```text
FD baseline
0 zombies
0 stale UDS socket
0 stale pidfile
0 dangling callback
```

## Lifecycle

```text
context init once
config load once
```

## Reactor

```text
dependency detach before destroy
```

## Service

```text
no async-stop + immediate free
```

## State

```text
RUNNING → STOPPING → STOPPED
```

## Reload

```text
failure → old runtime continues RUNNING
```

## Stress

```text
1000 start/stop
```

资源稳定。

---

# 110. 最终结论

这一组代码当前最需要修的不是某一个 `close()`，而是：

> ATPD 同时存在多套初始化与清理路径，导致资源 ownership 和 rollback 顺序无法被严格证明。

特别需要优先修复的实际问题：

```text
1. context 被初始化两次
2. startup config 被加载两次
3. rollback 中 service_stop_async() 后立即 free(service)
4. reactor_create 失败可能仍让 `atpd start` 返回成功
5. service start失败路径可能先 destroy reactor，再清依赖它的 UDS/netlink/service
6. cleanup.c 名义上“cleanup all”，实际上几乎什么也没清
7. STOPPED 状态在真正 teardown 前就被设置
```

最终应该收敛成：

```text
one startup transaction
+
one reverse teardown path
+
one owner per resource
+
idempotent cleanup primitives
```

这样前面已经做过的：

```text
service
reactor
UDS
netlink
session
async_validate
config
context
```

这些生命周期加固才能真正串成一个完整、可上线的 daemon。
