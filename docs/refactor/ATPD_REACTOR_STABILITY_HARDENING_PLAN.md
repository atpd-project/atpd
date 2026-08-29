# ATPD `reactor.c` 稳定性加固方案

## 1. 目标

当前 `reactor.c` 的整体架构是成立的，不建议像 `service.c` 一样拆文件。

现有能力包括：

- epoll
- eventfd
- signalfd
- FD handler
- timer list
- callback + userdata
- userdata free callback
- reactor statistics
- single-thread event loop

本次目标不是重写 reactor，而是：

> 把 reactor 从“基本可用的事件循环”加固成可以长期作为 ATPD 核心基础设施使用的可靠组件。

重点解决：

- 初始化失败回滚
- FD 注册失败的一致性
- signalfd 更新失败
- `modify_fd()` kernel/user-space 状态分叉
- callback 中增删 FD 的安全语义
- stale epoll event / FD reuse
- timer 起始时间与 ownership
- timer cancel 生命周期
- stats 准确性
- fault injection / stress coverage

---

# 2. 本次不做的事情

不建议：

- 拆成多个 reactor 源文件
- 改成 libevent/libuv
- 引入线程池
- 改成多线程 reactor
- 改整个 timer 数据结构
- 为了“现代化”重写全部 API

继续保持：

```text
reactor.c
reactor.h
```

即可。

---

# 3. P0/P1：`reactor_create()` 必须完整检查内部 FD 注册

当前初始化逻辑中：

```text
epoll_fd
event_fd
signal_fd
```

创建后会注册到 reactor。

问题：

```text
reactor_add_fd()
```

失败时没有完整处理，可能返回一个“部分初始化成功”的 reactor。

结果：

```text
reactor_create() != NULL
```

但：

```text
eventfd / signalfd 实际没有进入 epoll
```

这是不可接受的。

---

# 4. 初始化原则

`reactor_create()` 必须满足：

> 要么完全成功，要么完全失败。

任何一步失败：

```text
malloc
epoll_create1
eventfd
signalfd
reactor_add_fd(eventfd)
reactor_add_fd(signalfd)
```

都必须：

```text
reverse rollback
→ close created fds
→ free handlers/timers/private data
→ return NULL
```

---

# 5. 推荐初始化结构

示意：

```c
reactor_t *reactor_create(void)
{
    reactor_t *r = calloc(...);
    if (!r)
        goto fail;

    r->epoll_fd = epoll_create1(...);
    if (r->epoll_fd < 0)
        goto fail;

    r->event_fd = eventfd(...);
    if (r->event_fd < 0)
        goto fail;

    if (reactor_add_fd(...) != 0)
        goto fail;

    r->signal_fd = signalfd(...);
    if (r->signal_fd < 0)
        goto fail;

    if (reactor_add_fd(...) != 0)
        goto fail;

    return r;

fail:
    reactor_destroy_partial(r);
    return NULL;
}
```

不要继续依赖“后面 cleanup 会处理”。

---

# 6. 增加 partial-init-safe cleanup

建议让：

```c
reactor_destroy()
```

天然支持 partially initialized object。

要求：

```text
fd < 0 → skip
NULL → skip
重复调用不 crash
```

或者新增内部：

```c
static void reactor_cleanup_internal(
    reactor_t *r
);
```

用于：

```text
create failure
normal destroy
```

统一清理。

---

# 7. P0/P1：`reactor_watch_signal()` 必须保证原状态可恢复

当前风险：

```text
旧 signalfd 被移除/关闭
        ↓
创建新 signalfd
        ↓
reactor_add_fd(new) 失败
        ↓
signal handling 丢失
```

并且调用方可能仍收到 success。

---

# 8. Signal 更新应采用 prepare → commit

推荐：

```text
prepare new signal mask
        ↓
create new signalfd
        ↓
register new fd successfully
        ↓
commit
        ↓
remove old signalfd
```

不要：

```text
先破坏旧状态
再尝试创建新状态
```

---

# 9. Signal 更新失败要求

如果新 signalfd：

```text
create 失败
register 失败
```

必须：

```text
旧 signalfd 继续工作
旧 signal mask / handler 状态保持不变
return error
```

这是典型 transactional update。

---

# 10. `reactor_modify_fd()` 必须先改 kernel，再改内存状态

当前风险：

```c
h->events = new_events;

epoll_ctl(... MOD ...)
```

如果：

```text
epoll_ctl fails
```

则：

```text
handler.events = NEW
kernel epoll = OLD
```

造成状态分叉。

正确顺序：

```c
struct epoll_event ev = ...;

if (epoll_ctl(...) < 0)
    return -1;

h->events = events;
return 0;
```

---

# 11. 所有 reactor API 必须明确 ownership contract

建议在 `reactor.h` 中写清楚：

## `reactor_add_fd()`

成功：

```text
reactor owns handler object
reactor owns userdata only if free callback supplied
caller still owns underlying fd unless API 明确说明
```

失败：

```text
reactor 不持有任何 ownership
caller 继续拥有 fd / userdata
```

特别重要：

> `reactor_add_fd()` 失败时 reactor 不应该偷偷 close caller fd。

否则调用方很难写可靠 cleanup。

---

# 12. `reactor_remove_fd()` 语义

必须明确：

```text
remove from epoll
free handler
invoke userdata destructor if configured
```

但默认：

```text
不 close underlying fd
```

除非当前 API 设计明确就是 reactor-own-fd。

如果现在行为不同，应在头文件中准确写明。

核心要求不是一定哪种模式，而是：

> ownership 不能靠猜。

---

# 13. Callback self-remove

当前实现中 callback 自己调用：

```c
reactor_remove_fd(r, fd);
```

通常可以工作。

需要正式将其定义为支持的行为：

```text
callback may remove itself
```

并增加测试。

---

# 14. Callback remove-other-fd

也要明确是否支持：

```text
callback A
→ remove fd B
```

如果支持：

必须验证：

```text
本轮 epoll_wait 已经返回 B 的 event
```

时不会调用已失效 handler。

这是下一项 stale-event 问题。

---

# 15. P1：防止 stale epoll event + FD reuse

当前 epoll userdata 如果只保存：

```text
fd number
```

存在理论风险：

```text
epoll_wait 返回 fd=10 的旧 event
        ↓
callback A remove fd=10
        ↓
系统快速复用 10
        ↓
new handler add fd=10
        ↓
当前 batch 后续旧 event
可能命中新 handler
```

这类 bug 很难复现，但对于长期 daemon 是典型边界风险。

---

# 16. 推荐 generation token

建议 handler：

```c
typedef struct reactor_handler {
    int fd;
    uint64_t generation;
    ...
} reactor_handler_t;
```

reactor：

```c
uint64_t next_handler_generation;
```

每次 add：

```c
handler->generation =
    ++priv->next_handler_generation;
```

---

# 17. epoll userdata 不再只存裸 fd

推荐使用：

```c
ev.data.ptr = handler;
```

但直接存裸 handler 指针也有 UAF 风险，如果 callback 删除 handler 而当前 epoll batch 中还有 stale pointer。

更稳妥可以：

```text
slot / token
```

例如：

```c
typedef struct {
    int fd;
    uint64_t generation;
} reactor_event_token_t;
```

或者维护 stable slot。

如果第一阶段不想扩大改动，可以先：

```text
fd + generation validation
```

通过稳定 handler table 做校验。

---

# 18. 第一阶段可不立即重构 epoll token

如果当前代码风险可控：

P1 可以先做测试与 contract。

真正引入 generation token 可以放后续 commit。

不要把这一项和初始化修复绑成一个大改。

---

# 19. Timer 起始时间应使用真实 monotonic now

当前如果使用：

```text
priv->current_time_ms + timeout
```

则 callback 执行时间较长时，新 timer 会基于旧时间。

例如：

```text
loop timestamp = T0
callback takes 80 ms
callback add timer 100 ms
expires = T0 + 100
实际剩余约 20 ms
```

推荐：

```c
uint64_t now = get_monotonic_ms();
timer->expires_ms = now + timeout_ms;
```

不要依赖 cached loop timestamp 作为创建时刻。

---

# 20. `current_time_ms` 仍可保留

cached time 仍适合：

```text
metrics
same-loop timeout calculation
process expired timers
```

但：

```text
reactor_add_timer()
```

这种 API 应以真实调用时刻为准。

---

# 21. Timer ownership 必须写清楚

当前 timer 有：

```text
public timer handle
internal timer object
pending_delete
```

这种实现可以保留，但头文件必须明确。

例如：

```text
reactor_add_timer()
→ returns owned handle

reactor_cancel_timer()
→ handle invalid immediately

callback fires for one-shot
→ handle must not be reused

cancel twice
→ invalid usage
```

---

# 22. 建议 timer handle API 收紧

如果可行，推荐：

```c
int reactor_cancel_timer(
    reactor_t *r,
    reactor_timer_t **timer
);
```

成功后：

```c
*timer = NULL;
```

调用方：

```c
reactor_cancel_timer(r, &ctx->retry_timer);
```

天然避免：

```text
dangling pointer
double cancel
```

如果修改 API 影响面太大，可以暂不做，只补 contract + helper。

---

# 23. 可提供安全 helper

例如：

```c
static inline void reactor_cancel_timer_safe(
    reactor_t *r,
    reactor_timer_t **timer
)
{
    if (!timer || !*timer)
        return;

    reactor_cancel_timer(r, *timer);
    *timer = NULL;
}
```

若项目不希望 public helper，可以由各模块自己封装。

---

# 24. Timer userdata destructor

需要确认以下三条路径：

```text
one-shot fired
timer cancelled
reactor destroyed
```

userdata destructor：

```text
exactly once
```

不能：

```text
0 times → leak
2 times → double free
```

这一点必须通过测试验证。

---

# 25. Timer callback 中 cancel 自己

必须明确是否允许。

例如：

```text
callback(timer A)
→ reactor_cancel_timer(A)
```

如果不支持：

头文件必须写明。

如果支持：

必须保证：

```text
no double free
no iterator corruption
```

---

# 26. Timer callback 中新增/删除其他 timer

当前链表迭代逻辑必须验证：

```text
callback A add B
callback A cancel C
```

不会：

```text
skip
double fire
UAF
```

建议增加专项测试。

---

# 27. Timer stats

检查：

```c
reactor_stats_t.timers_fired
```

在实际 timer fire 时递增。

建议：

```c
stats.timers_fired++;
```

放在：

```text
确认 timer 到期并准备 callback
```

之后。

重复 timer 每次触发都计一次。

---

# 28. FD stats

同时审计：

```text
events_processed
fd_callbacks
timers_fired
wakeups
signal_events
```

确保：

```text
字段存在
≈
真实有更新
```

不要保留永远为 0 的 telemetry。

---

# 29. eventfd wakeup

审计：

```text
write eventfd
read eventfd
EAGAIN
EINTR
counter saturation
```

推荐：

```text
write/read 均处理 EINTR
EAGAIN 视语义决定是否成功
```

eventfd 用于 wakeup 时：

```text
counter 已非零
```

再 write 遇到边界情况通常无需 fatal。

---

# 30. signalfd read

必须处理：

```text
short read
EAGAIN
EINTR
multiple queued signals
```

推荐：

```text
read loop until EAGAIN
```

以避免一次 callback 只消费一个 signal，而剩余 signal 延迟。

---

# 31. epoll_wait error

必须区分：

```text
EINTR
fatal errors
```

`EINTR`：

```text
continue
```

其他异常：

```text
log
return failure / controlled shutdown
```

不要无限 tight loop。

---

# 32. EPOLLERR / EPOLLHUP

reactor 本身不要过度替业务模块解释：

```text
EPOLLERR
EPOLLHUP
EPOLLRDHUP
```

应原样传给 callback。

但要确认：

```text
callback 注册的 event mask
+
kernel implicit ERR/HUP
```

行为在 API 文档中有说明。

---

# 33. Reactor stop / wakeup

如果：

```text
reactor_stop()
```

可能从 callback 内调用：

必须确保：

```text
running=false
event loop exits cleanly
```

如果未来从其他线程调用：

eventfd wakeup 才有意义。

当前若 reactor 明确 single-thread only，应在文档中写清楚 thread-safety。

---

# 34. Thread-safety contract

建议明确：

```text
reactor API 默认仅 reactor thread 调用
```

如果某些 API 允许跨线程：

必须逐一标明。

不要给人：

```text
有 eventfd
= 所有 API thread-safe
```

的错觉。

---

# 35. Handler 数组边界

如果 handlers 使用：

```text
handlers[fd]
```

必须明确最大 FD。

当前如果有：

```text
fd >= max_fds
→ fail
```

这是合理的。

但：

```text
max_fds
```

应来自：

```text
RLIMIT_NOFILE
```

或配置的明确上限。

避免硬编码过小导致 Android/Linux 环境异常。

---

# 36. FD registration failure semantics

对于：

```text
fd >= max
malloc fail
epoll_ctl ADD fail
```

统一返回：

```text
-1
errno preserved/meaningful
```

并保证：

```text
handler table 未改变
fd 未关闭
userdata 未释放
```

除非 public contract 另有规定。

---

# 37. Duplicate add

如果同一个 fd 已注册：

```c
reactor_add_fd(r, fd, ...)
```

应明确：

```text
返回 EEXIST
```

而不是覆盖旧 handler。

修改必须走：

```text
reactor_modify_fd()
```

---

# 38. Remove nonexistent fd

明确：

```text
return -1 / ENOENT
```

或：

```text
idempotent success
```

二选一。

推荐基础设施 API 使用：

```text
ENOENT
```

调用方需要幂等 cleanup 时自己检查。

关键是语义固定。

---

# 39. `reactor_modify_fd()` 不允许偷偷替换 userdata

如果 modify API 只修改 event mask：

就只做这件事。

不要后续加入：

```text
callback
userdata
free callback
```

隐式替换。

如果未来需要：

新增独立 API。

---

# 40. Reactor destroy

销毁顺序推荐：

```text
stop accepting callbacks
        ↓
cancel/free timers
        ↓
remove/free handlers
        ↓
close signal_fd
event_fd
epoll_fd
        ↓
free private/context
```

如果 handler destructor 可能依赖 reactor 内部：

应在关闭 epoll 前完成。

---

# 41. Destroy 时 callback 不应再触发

`reactor_destroy()`：

```text
不执行普通业务 callback
```

只执行：

```text
userdata destructors
internal cleanup
```

避免 shutdown 时重新进入业务逻辑。

---

# 42. Fault Injection：必须新增

这是 reactor 最值得增加的测试能力。

推荐 test build 支持注入：

```text
malloc fail
epoll_ctl ADD fail
epoll_ctl MOD fail
epoll_ctl DEL fail
eventfd fail
signalfd fail
timer alloc fail
```

不一定要污染生产 API。

可以用：

```text
link-time wrapper
test hook
LD_PRELOAD
macro injection
```

任选最适合当前项目的一种。

---

# 43. 测试：create failure rollback

针对每一步：

```text
epoll_create fail
eventfd fail
eventfd register fail
signalfd fail
signalfd register fail
```

验证：

```text
reactor_create == NULL
no FD leak
no heap leak
```

---

# 44. 测试：watch_signal rollback

模拟：

```text
new signalfd create success
reactor_add_fd fails
```

验证：

```text
old signal handling still works
function returns error
no FD leak
```

---

# 45. 测试：modify failure

模拟：

```text
epoll_ctl MOD fails
```

验证：

```text
handler.events remains OLD
kernel state remains OLD
```

---

# 46. 测试：callback self-remove

```text
fd callback
→ remove itself
```

验证：

```text
no UAF
no double free
callback 不再次执行
```

---

# 47. 测试：callback removes another FD

同一个 epoll batch 中：

```text
A event
B event
```

A callback：

```text
remove B
```

验证：

```text
不会执行已销毁 B handler
```

如果当前实现不能保证：

这项测试应驱动 generation/token 修复。

---

# 48. 测试：FD reuse

构造：

```text
old fd N
remove
close
new fd obtains N
```

并制造 stale event。

验证：

```text
old event 不会调用 new handler
```

这是 generation 机制的核心验收。

---

# 49. 测试：timer timing

callback 内：

```text
sleep / deliberate delay
add_timer(100ms)
```

验证 timer 从：

```text
实际 add 时刻
```

开始计算，而不是 loop cached time。

---

# 50. 测试：timer ownership

覆盖：

```text
fire
cancel
double cancel misuse
cancel before fire
cancel from callback
destroy with active timer
repeat timer
```

检查：

```text
userdata destructor exactly once
```

---

# 51. 测试：mass timers

例如：

```text
1000 timers
```

随机：

```text
add
cancel
repeat
expire
```

验证：

```text
无 crash
无 leak
无 list corruption
```

---

# 52. 测试：mass FD churn

```text
socketpair / pipe
add/remove ×10000
```

检查：

```text
FD stable
RSS stable
handler count returns baseline
```

---

# 53. 测试：signal burst

快速发送：

```text
SIGUSR1 × many
SIGCHLD-like signal pattern
```

验证：

```text
signalfd queue consumed
reactor remains responsive
```

对 SIGCHLD 真正语义仍由 service 测试负责。

---

# 54. 测试：reactor responsiveness

在：

```text
大量 timers
FD churn
signal burst
```

情况下同时测：

```text
event latency
```

建议关注：

```text
p50
p95
p99
```

不要求把 reactor benchmark 做成复杂性能框架，但至少防止明显退化。

---

# 55. Sanitizers

Host 测试：

```text
ASan
UBSan
```

如果 test harness 引入多线程：

再考虑：

```text
TSan
```

重点：

```text
handler UAF
timer UAF
double free
list corruption
```

---

# 56. 静态编译检查

逐步开启：

```text
-Wall
-Wextra
-Wshadow
```

其他 warning 视项目情况。

不要为了 `-Werror` 强行做大量无意义 cast。

---

# 57. 第一阶段建议提交

## Commit 1

```text
reactor: make create fully transactional
```

内容：

- create 检查所有 add_fd
- partial-init rollback
- no leak

---

# 58. 第二阶段建议提交

## Commit 2

```text
reactor: harden signal fd replacement
```

内容：

- prepare new signalfd
- register
- commit
- failure 保留旧 signal fd

---

# 59. 第三阶段建议提交

## Commit 3

```text
reactor: preserve handler state on modify failure
```

内容：

- kernel success 后再更新 memory state
- duplicate/remove semantics cleanup

---

# 60. 第四阶段建议提交

## Commit 4

```text
reactor: fix timer clock and ownership contracts
```

内容：

- timer add 使用 monotonic now
- timer cancel contract
- destructor tests
- timer stats

---

# 61. 第五阶段建议提交

## Commit 5

```text
reactor: guard against stale fd events
```

内容：

- generation/token
- FD reuse tests
- callback mutation tests

这一项改动可能较大，应独立提交。

---

# 62. 第六阶段建议提交

## Commit 6

```text
tests: add reactor fault injection and stress coverage
```

也可以把测试跟随每个 commit 一起提交。

更推荐：

```text
每个修复 commit 自带对应测试
```

---

# 63. Codex 修改前必须先做 API Contract 表

先扫描：

```text
reactor_add_fd
reactor_modify_fd
reactor_remove_fd
reactor_add_timer
reactor_cancel_timer
reactor_watch_signal
reactor_stop
reactor_destroy
```

对每个 API 写：

```text
输入 ownership
成功后 ownership
失败后 ownership
callback 是否允许 self-remove
是否 thread-safe
返回值
errno
```

没有这张表不要先动实现。

---

# 64. Codex 必须审计的搜索项

```text
epoll_ctl
epoll_wait
eventfd
signalfd
reactor_add_fd
reactor_modify_fd
reactor_remove_fd
reactor_add_timer
reactor_cancel_timer
free(
close(
pending_delete
current_time_ms
timers_fired
handlers[
data.fd
data.ptr
```

---

# 65. 不允许的修复方式

禁止：

```text
epoll_ctl 失败只打印日志继续
```

禁止：

```text
signal fd 替换失败后仍返回 success
```

禁止：

```text
timer timing 问题靠增加 timeout 补偿
```

禁止：

```text
stale event 问题靠假设 Linux 不会快速复用 fd
```

---

# 66. 兼容要求

重构后保持：

```text
single-thread reactor
现有 reactor public API 尽可能兼容
service/netlink/uds/session 调用模式不被无必要破坏
```

若某个 API 必须改变：

优先做：

```text
兼容 wrapper
→ 调用点迁移
→ 再移除旧 API
```

---

# 67. 最终验收标准

## Create

任何初始化步骤失败：

```text
reactor_create == NULL
0 FD leak
0 heap leak
```

## Signal

signal fd 更新失败：

```text
旧 signal handling 仍然有效
```

## Modify

`EPOLL_CTL_MOD` 失败：

```text
user-space handler state 与 kernel state 不分叉
```

## Timer

```text
add_timer 从真实 monotonic now 计时
cancel ownership 清晰
userdata destructor exactly once
```

## Callback Mutation

```text
self-remove 安全
remove-other-fd 安全
```

## FD Reuse

```text
stale event 不命中新 generation handler
```

## Stress

大量：

```text
add/remove FD
add/cancel timer
signal burst
```

后：

```text
FD 回到 baseline
RSS 无持续增长
无 UAF
无 double free
```

## Responsiveness

reactor 本身不引入长时间 blocking path。

---

# 68. 目标状态

加固后的 reactor 应具备：

```text
transactional initialization
transactional signal replacement
consistent kernel/user state
explicit ownership contracts
safe callback mutation
safe timer lifecycle
stale event protection
observable stats
fault-injection tests
```

---

# 69. 最终结论

`reactor.c` 当前不需要结构性拆分。

正确方向不是：

> “把 500 多 LOC 拆成更多文件”

而是：

> “把它的失败语义、ownership、callback mutation 和 timer 生命周期做成非常明确的基础设施契约。”

因为 ATPD 上层的：

```text
service
netlink
XFRM
UDS
session
Native API
```

最终都依赖 reactor。

如果 reactor 的 contract 稳定，上层模块会明显更容易正确；如果 reactor 在失败边界上语义模糊，上层每个模块都会重复承担防御逻辑。

因此本方案建议将 `reactor.c` 定位为：

> ATPD C 版最重要的基础设施加固模块之一，但不是需要大重构的模块。
