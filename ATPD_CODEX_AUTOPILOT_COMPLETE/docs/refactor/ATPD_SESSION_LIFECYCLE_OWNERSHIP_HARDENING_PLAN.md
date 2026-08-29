# ATPD `session.c` 生命周期与 Ownership 加固方案

## 1. 模块结论

当前 `session.c` 约 688 行，已经具备比较成熟的设计基础：

- 显式 session state
- atomic reference counting
- reactor-held references
- GC queue
- splice pipe
- emergency drain
- per-session traffic statistics
- context session registry
- state/ownership 注释

因此本模块不建议立刻按文件长度机械拆分。

当前最重要的问题不是“代码太长”，而是：

> 让文档中声明的 ownership contract 和 state machine 真正成为代码层面的硬约束。

优先加固：

```text
P0/P1
1. state transition 不允许 CLOSING 被覆盖回 ACTIVE/PIPE_DIRTY
2. initial reference ownership 必须全仓确认并固化
3. reactor registration / removal ownership 必须可证明 exactly-once
4. GC queue 必须保证最终被处理
5. emergency drain / destroy / callback 的并发模型必须明确

P1
6. reactor_modify_fd() 失败不能静默
7. pipe_pending / state 更新需要事务化
8. context session list ownership 应集中
9. killswitch 当前存在重复遍历/重复 destroy 逻辑
10. session public header 暴露过多内部结构

P2
11. pipe size 设置失败应有 telemetry
12. session health/status snapshot
13. idle/stuck session detection
```

---

# 2. 本次不建议立即拆文件

第一轮继续：

```text
session.c
session.h
```

因为当前复杂度主要来自：

```text
state
refcount
reactor
pipe
GC
```

这些概念彼此高度关联。

如果先机械拆文件，容易把 ownership 分散到多个模块。

优先顺序：

```text
先把 invariants 做实
        ↓
加测试
        ↓
再判断是否需要拆
```

---

# 3. 当前最重要的设计优点：已有 Ownership Contract

源码已经明确写出：

```text
create                  +1
reactor fd_in            +1
reactor fd_out           +1
GC enqueue               +1

reactor free_cb          -1
reactor free_cb          -1
GC process               -1
```

这是非常正确的方向。

必须保留。

后续所有修改都不应回退到：

```text
“谁方便谁 free”
```

---

# 4. P0/P1：initial reference 必须明确最终由谁释放

`atpd_session_create()`：

```c
atomic_init(&s->ref_count, 1);
```

这代表：

```text
creator owns initial reference
```

随后 register：

```text
fd_in reactor +1
fd_out reactor +1
```

closing：

```text
reactor remove → -1
reactor remove → -1
GC enqueue → +1
GC process → -1
```

如果 creator 的 initial `+1` 没有显式：

```c
atpd_session_put(s);
```

最终 refcount 会停在：

```text
1
```

session 永远不会 destroy。

---

# 5. Codex 必须先做全仓 initial-ref audit

搜索：

```text
atpd_session_create(
atpd_session_register(
atpd_session_put(
atpd_session_destroy(
```

对每个 create call site 列出：

```text
create success
register success
register failure
caller handoff
caller final put
```

必须回答：

> create 返回的 initial reference 最终在哪里释放？

如果当前所有调用点都正确释放：

保留设计并增加注释/测试。

如果没有：

这是 P0/P1 leak，必须修复。

---

# 6. 推荐 creator ownership contract

推荐 API contract：

```text
atpd_session_create()
→ caller owns +1

atpd_session_register()
→ reactor takes its own references
→ does NOT consume caller reference

caller 完成 handoff 后：
atpd_session_put(session)
```

示例：

```c
atpd_session_t *s = atpd_session_create(...);
if (!s)
    return -1;

if (atpd_session_register(r, s) != 0) {
    atpd_session_destroy(s);
    atpd_session_put(s);
    return -1;
}

/* reactor/session subsystem now owns runtime lifetime */
atpd_session_put(s);
```

这个规则必须写进 `session.h`。

---

# 7. 也可以选择 consume-on-register，但不要混合

另一种设计：

```text
register success
→ consume creator reference
```

也可以。

但不要出现：

```text
有的 caller put
有的 caller 不 put
```

推荐继续采用传统显式 refcount：

> create gives caller +1，caller 必须 put。

语义最清晰。

---

# 8. P0/P1：当前状态机没有真正强制执行

文件顶部声明了严格状态：

```text
IDLE
ACTIVE
PIPE_DIRTY
DRAINING
CLOSING
DESTROY_PENDING
DESTROYED
```

但实现中大量直接：

```c
atomic_store(&s->state, ...);
```

例如：

```text
PIPE_DIRTY → DRAINING
DRAINING → ACTIVE
GC → DESTROY_PENDING
destroy → DESTROYED
```

这意味着注释中的：

```text
Invalid Transitions rejected by CAS
```

并不完全成立。

---

# 9. 最危险的状态复活场景

假设未来或当前存在跨线程路径：

```text
Thread/Reactor A:
state = DRAINING

Thread B:
mark_closing()
DRAINING → CLOSING

Thread A:
drain completes
atomic_store(ACTIVE)
```

最终：

```text
CLOSING → ACTIVE
```

session 被“复活”。

类似：

```text
CLOSING → PIPE_DIRTY
```

也可能发生。

这是生命周期模块必须避免的状态。

---

# 10. 引入统一 state transition helper

建议：

```c
static bool session_transition(
    atpd_session_t *s,
    atpd_session_state_t from,
    atpd_session_state_t to
);
```

或者：

```c
static bool session_try_transition(
    atpd_session_t *s,
    atpd_session_state_t expected,
    atpd_session_state_t next
);
```

内部统一：

```c
atomic_compare_exchange_*
```

---

# 11. 所有非 terminal transition 必须 CAS

例如：

```text
ACTIVE → PIPE_DIRTY
PIPE_DIRTY → DRAINING
DRAINING → ACTIVE
DRAINING → PIPE_DIRTY
```

都必须保证：

```text
如果当前已经 CLOSING 或更高
→ transition fail
→ 不覆盖 terminal state
```

---

# 12. Terminal state 单调性

建立 invariant：

```text
CLOSING
DESTROY_PENDING
DESTROYED
```

是单向 terminal progression。

一旦：

```text
state >= CLOSING
```

永远不能回到：

```text
ACTIVE
PIPE_DIRTY
DRAINING
```

这个规则必须成为代码级 invariant。

---

# 13. 推荐 state helper

例如：

```c
static bool session_set_runtime_state(
    atpd_session_t *s,
    atpd_session_state_t next)
{
    for (;;) {
        int old = atomic_load(&s->state);

        if (old >= ATPD_SESSION_CLOSING)
            return false;

        if (atomic_compare_exchange_weak(
                &s->state,
                &old,
                next))
            return true;
    }
}
```

但更推荐显式合法 transition table，而不是允许任意 runtime state 跳转。

---

# 14. 合法 transition table

建议：

```text
IDLE
  → ACTIVE
  → CLOSING

ACTIVE
  → PIPE_DIRTY
  → CLOSING

PIPE_DIRTY
  → DRAINING
  → CLOSING

DRAINING
  → ACTIVE
  → PIPE_DIRTY
  → CLOSING

CLOSING
  → DESTROY_PENDING

DESTROY_PENDING
  → DESTROYED

DESTROYED
  → none
```

测试应覆盖非法 transition 被拒绝。

---

# 15. `atpd_session_register()` 状态转换

注册成功：

```text
IDLE → ACTIVE
```

不要：

```c
atomic_store(ACTIVE);
```

应该 CAS：

```text
expected IDLE
```

如果 session 已经：

```text
CLOSING
```

注册必须失败。

---

# 16. registration rollback ownership

当前逻辑：

```text
get fd_in ref
add fd_in

get fd_out ref
add fd_out
```

第二个失败：

```text
remove fd_in
put fd_out pre-ref
```

这个方向是合理的。

需要通过 fault-injection test 证明：

```text
add fd_in fail
add fd_out fail
```

每条路径 refcount 都回到 creator baseline。

---

# 17. 建议 debug ref invariant

debug/test build 可提供：

```c
session_debug_refcount(s)
```

或者测试通过 status hook 检查。

预期：

```text
after create             1
after register           3
after caller handoff     2
after mark closing       1 (GC ref)
after gc process         0 → destroy
```

这是非常有价值的测试。

---

# 18. `atpd_session_put()` 防 underflow

当前：

```c
old = atomic_fetch_sub(...);
```

如果错误调用：

```text
ref_count == 0
```

会 underflow 成巨大 unsigned value。

建议 debug/production defensive check：

```c
if (old == 0) {
    LOG_FATAL / LOG_ERROR;
    abort in debug;
}
```

不能静默继续。

---

# 19. `atpd_session_get()` 防 resurrection

如果：

```text
ref_count == 0
```

session 理论上已经进入 destroy。

不要允许：

```text
0 → 1
```

如果确实存在跨线程 weak reference，需要专门的：

```text
try_get()
```

但当前没有必要。

建议 debug assert：

```text
old > 0
```

---

# 20. destroy_started 与 refcount 关系

当前 destroy 只在：

```text
put old == 1
```

触发。

这是好设计。

`destroy_started` 更多是 defensive protection。

建议保持，但 invariant 应是：

```text
destroy_internal() entry
→ refcount == 0
```

如果不是：

```text
debug build assert
production LOG_ERROR
```

而不是普通 WARN 后继续 free。

---

# 21. 非零 ref 时 destroy 是否应该继续 free

当前 destroy 中：

```text
if refs != 0:
    LOG_WARN
```

然后仍然：

```text
free(s)
```

理论上如果真的出现 non-zero ref：

这可能造成：

```text
其他 holder 继续访问 freed session
```

更安全：

```text
debug abort/assert
```

生产环境至少：

```text
LOG_FATAL
```

并明确这是 invariant violation。

由于 destroy 只有 ref==0 应能进入，这种情况代表严重 bug。

---

# 22. Reactor-held reference contract

必须和 `reactor.c` 加固方案一致：

```text
reactor_add_fd_ex success
→ reactor owns userdata ref

reactor_remove_fd
→ free_cb exactly once

reactor_destroy
→ free_cb exactly once

add failure
→ free_cb NOT called
```

session 正确性高度依赖这个 contract。

---

# 23. `session_free_cb()` 应保持极简

当前：

```text
free_cb
→ atpd_session_put()
```

这是正确的。

不要在 free callback 中加入：

```text
close
GC enqueue
state transition
logging-heavy logic
```

它只负责：

> release reactor-held reference。

---

# 24. P1：`reactor_modify_fd()` 返回值必须检查

当前 pipe dirty 时：

```c
reactor_modify_fd(
    reactor,
    fd_out,
    READ | WRITE | EDGE
);
```

返回值被忽略。

如果 MOD 失败：

```text
pipe_pending > 0
state = PIPE_DIRTY
但 EPOLLOUT 没启用
```

结果：

```text
session 永远等不到 writable callback
```

形成 stuck session。

---

# 25. Enable WRITE failure

必须：

```c
if (reactor_modify_fd(...) != 0) {
    LOG_ERROR(...);
    mark_closing();
    return error;
}
```

不要留下：

```text
PIPE_DIRTY but no WRITE subscription
```

---

# 26. Disable WRITE failure

drain 完成：

```text
PIPE_DIRTY → ACTIVE
```

然后：

```text
remove WRITE interest
```

如果 `reactor_modify_fd()` 失败：

可能继续收到 unnecessary EPOLLOUT。

不一定 fatal，但 state 与 reactor interest 分叉。

推荐：

```text
MOD failure
→ mark session closing
```

基础设施失败后关闭单条 session 比保持未知状态更安全。

---

# 27. Pipe state 与 epoll interest 应视为一个事务

理想 invariant：

```text
PIPE_DIRTY
↔
fd_out has WRITE interest
```

不能出现：

```text
PIPE_DIRTY + no EPOLLOUT
```

也尽量不要：

```text
ACTIVE + permanent EPOLLOUT
```

---

# 28. `pipe_pending` 更新需要和状态顺序统一

写阻塞：

推荐顺序：

```text
calculate pending
        ↓
store pipe_pending
        ↓
enable EPOLLOUT
        ↓
transition ACTIVE/DRAINING → PIPE_DIRTY
```

或根据 callback semantics 做适合的顺序。

关键是失败时必须有 rollback/close。

不要让 partial state 留下来。

---

# 29. `atpd_session_drain_pipe()` 的状态 CAS

当前：

```text
检查 PIPE_DIRTY
atomic_store DRAINING
```

应改：

```text
CAS PIPE_DIRTY → DRAINING
```

如果 CAS 失败：

```text
如果 CLOSING
→ stop

如果其他 state
→ no-op/error
```

---

# 30. Drain 完成

不要直接：

```text
DRAINING → ACTIVE
```

用 CAS：

```text
expected DRAINING
```

如果期间已经进入：

```text
CLOSING
```

则不要恢复 ACTIVE。

---

# 31. Drain 再次 EAGAIN

同样：

```text
DRAINING → PIPE_DIRTY
```

必须 CAS。

如果已经 CLOSING：

```text
保持 CLOSING
```

---

# 32. emergency drain

当前 emergency drain：

```text
close pipe read
close pipe write
pipe_pending = 0
mark closing
```

思路可以保留。

但必须明确调用线程模型。

如果另一线程正在：

```text
splice(pipe_fd)
```

同时这里：

```text
close(pipe_fd)
```

FD number 可能被系统复用。

这是经典：

> close-vs-use FD reuse race。

---

# 33. 明确 session 是否 single-thread owned

推荐强 contract：

> session I/O、state transition、reactor registration/removal、emergency drain 全部只在 reactor thread 执行。

跨线程只能：

```text
post event / wake reactor
```

不能直接 close session FD。

如果项目实际遵守这一点：

大量 atomics/pthread complexity可以后续简化。

---

# 34. 如果必须支持跨线程

则不能仅靠 atomic state 保证 FD 生命周期。

至少需要：

```text
session lock
or
reactor-thread deferred close
```

推荐仍然：

```text
所有 FD close 回 reactor thread
```

这是最干净的模型。

---

# 35. `atpd_session_emergency_drain_all()` 线程要求

必须在 API comment 中写：

```text
MUST run on reactor thread
```

或者改名/实现为：

```text
atpd_session_request_emergency_drain_all()
```

内部投递到 reactor。

---

# 36. GC queue 为什么需要 mutex

当前 GC queue：

```text
pthread_mutex
```

说明设计可能允许跨线程 enqueue。

需要 Codex审计：

```text
gc_enqueue 调用线程
gc_process 调用线程
```

如果所有实际路径都 reactor-thread only：

可以后续去掉 mutex。

第一轮不改，先明确 contract。

---

# 37. P1：GC process 必须保证被周期执行

当前 lifecycle 的最终 destroy 依赖：

```text
gc_enqueue()
        ↓
gc_process()
```

如果 GC processing 没有被可靠调度：

```text
DESTROY_PENDING
```

会无限堆积。

Codex必须全仓搜索：

```text
atpd_session_gc_process(
```

确认：

- 谁调用
- 每轮 reactor 是否调用
- shutdown 是否最后调用
- reactor stop 后是否仍可能有 pending GC

---

# 38. 推荐 GC invariant

```text
任何 gc_enqueue
→ 最终一定有一次 gc_process
```

shutdown：

```text
stop accepting new sessions
close/mark all
process GC
destroy reactor
```

顺序必须保证。

---

# 39. 可考虑去掉显式 GC queue吗？

暂时不要。

当前 GC 的作用很合理：

> 避免 callback/remove/free 路径里立刻 free session，降低 reentrancy/UAF 风险。

因此保留 deferred destruction。

先把 guarantee 做实。

---

# 40. GC node 嵌入 session 是合理的

当前：

```c
struct session_gc_node gc_node;
```

避免 enqueue 时额外 malloc。

这个设计很好，建议保留。

---

# 41. P1：Context session registry allocation failure

`atpd_session_create()`：

```text
pipe 创建成功
atpd_session_register_to_ctx(s)
```

但 context register 内如果：

```text
calloc list node fail
```

只是 LOG_ERROR，然后 create 仍然成功。

结果：

```text
session 存活
但不在 g_atpd_ctx.sessions
```

killswitch/emergency drain/status 可能看不到它。

这是重要一致性问题。

---

# 42. Context registration 必须返回结果

改：

```c
int atpd_session_register_to_ctx(
    struct atpd_session *s
);
```

失败：

```text
session_create rollback
close pipe
free session
return NULL
```

不能允许：

> runtime session 未进入 authoritative registry。

---

# 43. Context registry ownership

需要明确：

```text
registry node owns session reference?
```

当前看起来：

```text
node 只保存 raw pointer
不持 ref
```

可以，但必须保证：

```text
session destroy 前先 unregister
```

当前 destroy_internal 已这么做。

保持即可。

---

# 44. 如果 registry 不持 reference

所有 registry traversal 必须发生在 session lifetime 可保证的线程/阶段。

跨线程遍历 raw pointer 是不安全的。

再次说明：

> 最好把 session registry 也限制为 reactor-thread owned。

---

# 45. `atpd_vpn_killswitch()` 当前逻辑应简化

当前大体是：

```text
先复制最多 256 个 session pointer
→ destroy

然后再次遍历整个 g_atpd_ctx.sessions
→ destroy
→ free list node
→ sessions = NULL
```

存在：

```text
重复 destroy
重复计数
职责混乱
```

虽然 `mark_closing()` 本身较幂等，但这段不够干净。

---

# 46. Kill-switch 不应手工 free registry nodes

registry node 生命周期应该唯一由：

```text
atpd_session_unregister_from_ctx()
```

管理。

killswitch 不应该：

```text
free(node)
```

否则 registry ownership 出现两个 owner。

推荐：

```text
snapshot pointers with refs
        ↓
mark closing each
        ↓
put snapshot refs
```

节点由最终 destroy unregister。

---

# 47. Snapshot 遍历要持 session reference

正确 pattern：

```text
for each registry session:
    session_get(s)
    add to local snapshot

for each snapshot:
    mark_closing(s)
    session_put(s)
```

这样即使 mark_closing 导致 deferred cleanup，也不会使 snapshot pointer 失效。

---

# 48. 不要固定 256 session 上限

当前临时：

```text
session_ptrs[256]
```

会让逻辑出现：

```text
前 256
+
后续第二遍
```

推荐：

- reactor thread 上直接安全遍历并缓存 next
- 或动态 snapshot
- 或统一 `session_close_all()` subsystem API

---

# 49. 新增统一 close-all API

建议放在 session 模块：

```c
void atpd_session_close_all(
    atpd_session_close_reason_t reason
);
```

由：

```text
VPN teardown
daemon shutdown
service restart
```

调用。

不要让 `atpd_context.c` 自己实现 session destroy算法。

---

# 50. Close reason

建议增加：

```c
typedef enum {
    ATPD_SESSION_CLOSE_EOF,
    ATPD_SESSION_CLOSE_ERROR,
    ATPD_SESSION_CLOSE_VPN_DOWN,
    ATPD_SESSION_CLOSE_SHUTDOWN,
    ATPD_SESSION_CLOSE_REACTOR_ERROR,
    ATPD_SESSION_CLOSE_BACKPRESSURE
} atpd_session_close_reason_t;
```

保存：

```text
last/final close reason
```

提升可观测性。

---

# 51. `emergency_drain` 语义

“Emergency drain” 实际做的是：

```text
close pipe
discard buffered data
```

它不是“drain”。

建议内部改名：

```text
session_discard_pending_pipe()
```

或：

```text
session_abort_pipe()
```

避免语义误导。

Public API：

```text
emergency_close_all
```

更准确。

---

# 52. Pipe size `fcntl` 返回值

当前：

```c
fcntl(F_SETPIPE_SZ, ATPD_SESSION_PIPE_SIZE);
```

返回值未检查。

在 Android/Linux 上可能：

```text
EPERM
EINVAL
kernel clamps size
```

这不一定是 fatal。

建议：

```text
if fail:
    WARN/DEBUG
    query actual F_GETPIPE_SZ
```

然后记录真实 pipe size。

---

# 53. 不要求 pipe size 设置成功才能创建 session

64 KiB 是优化，不应是 correctness 硬依赖。

因此：

```text
F_SETPIPE_SZ fail
→ continue with kernel default
```

但应该可观察。

---

# 54. `splice` capability fallback

当前：

```text
EINVAL → ATPD_SPLICE_NOTSUP
```

这本身合理。

需要确认上层收到：

```text
NOTSUP
```

后是否有 fallback：

```text
read/write
```

或明确关闭 session。

如果没有 fallback：

status/log 应能解释：

```text
splice unsupported
```

不要只表现成 generic session failure。

---

# 55. `bytes_in` / `bytes_out` 语义

当前：

```text
bytes_in = data moved fd_in → pipe
bytes_out = data moved pipe → fd_out
```

这个定义合理。

要明确：

```text
pipe 中 pending data
```

会造成：

```text
bytes_in > bytes_out
```

这是正常 backpressure，不是统计错误。

---

# 56. `last_active_at`

当前主要在 pump 完成后：

```text
total_moved > 0
```

更新。

如果只是 drain pending pipe：

也属于 activity。

建议 drain 成功发送数据时更新：

```text
last_active_at
```

方便识别真正 idle session。

---

# 57. 增加 session runtime status

推荐：

```c
typedef struct {
    uint64_t session_id;
    atpd_session_state_t state;

    uint64_t bytes_in;
    uint64_t bytes_out;
    size_t pipe_pending;

    uint64_t age_ms;
    uint64_t idle_ms;

    unsigned int ref_count;

    atpd_session_close_reason_t close_reason;
} atpd_session_status_t;
```

供 status snapshot 读取。

---

# 58. 不要让 status 持裸 session pointer 太久

如果 status 可能跨 callback/thread：

必须：

```text
get reference
copy snapshot
put reference
```

不要直接遍历 raw registry pointer 后做复杂 rendering。

---

# 59. Session aggregate stats

建议维护：

```text
created_total
destroyed_total
active
peak_active

closed_eof
closed_error
closed_vpn_down
closed_shutdown

splice_not_supported
pipe_backpressure_events
gc_pending
```

这对真实 Android soak 很有价值。

---

# 60. Stuck session watchdog

不是第一优先级。

后续可以检测：

```text
PIPE_DIRTY 持续超过 N 秒
```

或：

```text
DESTROY_PENDING 持续异常时间
```

输出 WARN/status。

不要一开始就自动 kill，先做 telemetry。

---

# 61. Public `session.h` 暴露太多内部结构

当前 public header 直接暴露：

```c
struct atpd_session {
    ...
};
```

以及：

```text
gc node
pipe fds
atomic refs
reactor pointer
```

这让其他模块可以直接修改内部字段。

长期不利于 ownership。

---

# 62. 中期建议 opaque session

Public：

```c
typedef struct atpd_session atpd_session_t;
```

只暴露 API。

内部结构移到：

```text
session_internal.h
```

或 `session.c`。

但第一轮 correctness 修复不必同时做。

---

# 63. 对外 API 应逐步缩小

理想 public API：

```c
atpd_session_t *atpd_session_create(...);
int atpd_session_register(...);

void atpd_session_get(...);
void atpd_session_put(...);

void atpd_session_close(...);

void atpd_session_close_all(...);

int atpd_session_get_status(...);
```

像：

```text
gc_enqueue
gc_process
drain_pipe
```

最好属于 subsystem internal API。

---

# 64. Test：initial reference

测试：

```text
create
register
caller put
close
gc process
```

验证：

```text
destroyed_total +1
refcount reaches 0
FD baseline
```

再测试 register failure。

---

# 65. Test：forgotten initial ref detector

debug test 可故意：

```text
create
register
不 caller put
close
gc
```

验证：

```text
session remains ref=1
```

从而证明 contract。

更好的是通过 API 改造让这种误用更难发生。

---

# 66. Test：register fd_in failure

故障注入：

```text
first reactor_add_fd_ex fails
```

验证：

```text
refcount returns 1
state IDLE/CLOSING according contract
no reactor ref leak
```

---

# 67. Test：register fd_out failure

```text
fd_in add success
fd_out add fail
```

验证：

```text
fd_in removed
fd_in free_cb exactly once
second pre-ref put exactly once
refcount returns creator baseline
```

---

# 68. Test：closing during drain

构造并发/模拟 interleaving：

```text
PIPE_DIRTY
→ DRAINING
→ mark CLOSING
→ drain completion
```

最终必须：

```text
CLOSING or later
```

绝不能：

```text
ACTIVE
```

这是核心 regression test。

---

# 69. Test：closing during backpressure transition

模拟：

```text
splice write EAGAIN
```

同时 closure。

最终不能：

```text
CLOSING → PIPE_DIRTY
```

---

# 70. Test：reactor modify failure

### Enable WRITE fail

验证：

```text
session closes
无 stuck PIPE_DIRTY
```

### Disable WRITE fail

验证：

```text
session enters controlled close/error
```

---

# 71. Test：GC exactly once

多次：

```text
mark_closing
destroy
emergency close
```

同一 session。

验证：

```text
gc enqueue once
destroy once
reactor free_cb once per registered fd
```

---

# 72. Test：GC delayed

enqueue 多个 session，不立刻 process。

验证：

```text
objects remain valid
```

process：

```text
all released
queue empty
```

---

# 73. Test：shutdown with pending GC

```text
active sessions
→ close all
→ reactor shutdown sequence
```

验证：

```text
GC 最终处理
0 session leak
```

---

# 74. Test：context registration allocation failure

故障注入：

```text
session registry node calloc fails
```

推荐行为：

```text
session_create fails
pipe closed
no orphan session
```

---

# 75. Test：killswitch >256 sessions

创建：

```text
300+
```

sessions。

触发 VPN teardown。

验证：

```text
全部进入 closing
无重复 registry free
最终 active=0
```

这可以直接暴露当前 256 snapshot workaround 的问题。

---

# 76. Test：emergency close twice

```text
emergency close all
emergency close all
```

必须幂等：

```text
no double close
no double enqueue
no ref underflow
```

---

# 77. Test：pipe size failure

mock：

```text
F_SETPIPE_SZ fail
```

session 仍可工作。

status/log 可看到：

```text
fallback pipe size
```

---

# 78. Test：backpressure

让 fd_out 不可写。

大量输入：

```text
pipe fills
→ PIPE_DIRTY
→ enable EPOLLOUT
```

随后恢复可写：

```text
DRAINING
→ ACTIVE
pipe_pending=0
```

---

# 79. Test：VPN drops mid-splice

运行 splice 时：

```text
VPN READY → TEARDOWN
```

验证：

```text
session terminal close
pending pipe discarded
无 state resurrection
```

---

# 80. Stress

建议：

```text
10,000 session create/register/close cycles
```

检查：

```text
FD baseline
RSS stable
GC queue empty
registry empty
created == destroyed
```

---

# 81. Parallel stress

如果明确支持跨线程：

```text
close
VPN teardown
GC
callback
```

进行并发压力，并运行：

```text
TSan
```

如果明确 reactor-thread-only：

则不要制造不存在的并发模型。

改为测试：

```text
reentrant callback ordering
```

---

# 82. Sanitizers

必须：

```text
ASan
UBSan
```

若支持跨线程：

```text
TSan
```

重点：

```text
refcount underflow
UAF
double close
registry UAF
GC UAF
pipe fd reuse
```

---

# 83. 推荐提交顺序

## Commit 1

```text
session: document and verify reference ownership
```

内容：

- 全仓 initial ref audit
- header ownership comment
- refcount tests
- underflow assertions

---

## Commit 2

```text
session: enforce monotonic state transitions
```

内容：

- CAS transition helper
- terminal-state invariant
- remove unsafe state atomic_store
- race/interleaving tests

---

## Commit 3

```text
session: harden reactor interest transitions
```

内容：

- check reactor_modify_fd
- PIPE_DIRTY ↔ WRITE interest invariant
- failure closes session

---

## Commit 4

```text
session: make registry membership authoritative
```

内容：

- register_to_ctx returns error
- create rollback on registry allocation failure
- registry ownership cleanup

---

## Commit 5

```text
session: centralize close-all and killswitch cleanup
```

内容：

- remove duplicate 256 + second traversal logic
- session_close_all()
- snapshot refs if needed
- no manual registry node free outside registry API

---

## Commit 6

```text
session: guarantee GC shutdown lifecycle
```

内容：

- audit gc_process scheduling
- shutdown final GC
- stats/pending telemetry

---

## Commit 7

```text
session: add runtime observability
```

内容：

- close reason
- aggregate counters
- status snapshot
- pipe size telemetry

---

# 84. 是否拆 `session.c`

完成上述 correctness work 后再判断。

如果仍然：

```text
700–900 LOC
```

但边界清晰，可以继续单文件。

如果继续增长，可拆：

```text
session.c
session_io.c
session_internal.h
```

其中：

```text
session.c
    lifetime
    refcount
    state
    GC
    registry-facing API

session_io.c
    splice
    pipe
    reactor callbacks
    backpressure
```

但只有在 ownership 已经稳定以后才拆。

---

# 85. 不建议拆成

不要：

```text
session_gc.c
session_ref.c
session_close.c
session_pipe.c
session_callback.c
```

这样会把一个 session 生命周期分散到过多文件。

---

# 86. 推荐长期边界

未来如果拆，两层足够：

```text
session.c
    lifetime / state / ownership

session_io.c
    datapath / splice / backpressure
```

---

# 87. 与 reactor 加固方案的依赖

session 正确性依赖：

```text
reactor_add_fd_ex success ownership
reactor_remove_fd free_cb exactly once
reactor_modify_fd failure consistency
callback self-remove semantics
```

因此：

> reactor contract 应先稳定，session 再完成最终 ownership 验收。

---

# 88. 与 context 模块的关系

当前 session registry 放在：

```text
g_atpd_ctx
```

短期可以保留。

但 registry API 必须由 session subsystem主导语义：

```text
register
unregister
snapshot
close_all
```

不要让 context 模块自己操作 session 生命周期。

---

# 89. 与 status 重构的关系

新的 status 不应该直接遍历 session 内部结构。

推荐：

```text
session subsystem
→ aggregate snapshot
→ status snapshot
→ renderer
```

这样未来 opaque session struct 也不会影响 status。

---

# 90. 核心 Invariants

Codex 最终必须把以下写进源码注释和测试：

```text
I1:
ref_count == 0
→ object destruction starts exactly once

I2:
state >= CLOSING
→ state never returns below CLOSING

I3:
each successful reactor registration
→ exactly one reactor-held ref
→ exactly one free_cb release

I4:
GC enqueue happens at most once

I5:
registry membership is removed before object free

I6:
PIPE_DIRTY requires a viable path to writable notification

I7:
all session-owned FDs close exactly once

I8:
all created sessions eventually become destroyed
after close + GC

I9:
no direct cross-thread FD close unless explicitly synchronized
```

---

# 91. 最终验收标准

## Reference

```text
create/register/handoff/close/gc
→ refcount reaches 0 exactly once
```

## State

```text
CLOSING cannot resurrect
```

## Reactor

```text
each successful fd registration
→ one free_cb
```

## Backpressure

```text
PIPE_DIRTY cannot become permanently stuck due silent MOD failure
```

## Registry

```text
every live session is represented
no orphan session
no duplicate node free
```

## GC

```text
pending queue drains before shutdown completes
```

## VPN teardown

```text
all sessions close, including >256 sessions
```

## Resource

```text
10,000 lifecycle cycles
FD baseline
RSS stable
created == destroyed
```

## Sanitizers

```text
ASan clean
UBSan clean
TSan clean if concurrent model supported
```

---

# 92. 最终结论

`session.c` 当前已经不是“随意拼接的 socket 代码”，它实际上已经具备：

```text
state machine
reference counting
deferred destruction
backpressure
reactor ownership
```

这是好的基础。

真正需要做的是：

> 把这些设计从“注释中的规则”提升成“代码绝对无法轻易违反的 invariants”。

本轮最重要的三个动作：

```text
1. 全仓确认 initial reference 最终释放
2. 所有 state transition 改成不可覆盖 terminal state 的 CAS 规则
3. 把 reactor/GC/registry ownership 做成 exactly-once 可测试契约
```

完成这些后，`session.c` 会成为 ATPD C 版中一个相当扎实的生命周期模块，而不是需要推倒重写的模块。
