# ATPD `async_validate.c` 异步验证生命周期加固方案

## 1. 模块结论

当前 `async_validate.c` 约 331 行，职责集中，**不建议拆文件**。

它目前负责：

```text
fork sing-box check
        ↓
pipe 收集 stdout/stderr
        ↓
timerfd 5 秒 timeout
        ↓
reactor 驱动
        ↓
callback 返回验证结果
```

总体方向是正确的，而且源码已经明确提出：

```text
callback exactly once
resources exactly once
child reaped exactly once
FDs closed exactly once
```

真正的问题是：

> 这些 lifecycle contract 目前还没有完全被实现路径保证。

本轮优先级：

```text
P0/P1
1. 修复 timeout/reap 后 validation 可能永久悬挂
2. 明确 ctx / userdata / callback lifetime contract
3. 明确 cancel/manual cleanup 的 callback 语义
4. child ownership/reap 必须 exactly once
5. reactor registration + FD ownership 必须 transactional

P1
6. pipe 使用 CLOEXEC
7. read 应完整 drain
8. waitpid/EINTR/error 语义统一
9. timeout/cancel 不应在 reactor 中无界 blocking waitpid
10. exec failure 应与 config validation failure区分

P2
11. result 改成 typed enum
12. timeout 可配置
13. validation status/metrics
```

---

# 2. 已确认的最高优先级 bug：timeout path 可能永远不完成

当前 timeout callback：

```c
pid_t wr = waitpid(ctx->child_pid, NULL, WNOHANG);

if (wr == ctx->child_pid) {
    /* Child already exited, wait for IO callback to handle */
    return;
}
```

这里有两个问题。

第一：

```text
waitpid 已经把 child reap 了
```

但是没有保存：

```text
exit status
```

第二：

它假设：

```text
IO callback 之后一定还会再触发
```

这个假设不成立。

---

# 3. 典型卡死时序

可能发生：

```text
pipe EOF
↓
IO callback
↓
waitpid(..., WNOHANG) == 0
child此刻仍在运行
↓
IO callback return
↓
之后 child exit
↓
没有新的 pipe data/EOF edge
↓
5s timer触发
↓
waitpid() reap child
↓
timeout callback直接 return
↓
没有任何后续 event
```

最终：

```text
ctx->completed == 0
child_pid仍保存旧 PID
pipe/timer可能仍注册
callback永远不触发
```

这是 lifecycle correctness bug。

---

# 4. Timeout callback 不能把“已退出 child”交回 IO callback

如果 timeout callback：

```text
成功 reap child
```

它自己就必须负责完成 validation。

例如：

```c
int status = 0;

pid_t wr = waitpid(pid, &status, WNOHANG);

if (wr == pid) {
    drain_remaining_output(ctx);
    complete_from_wait_status(ctx, status);
    return;
}
```

不要：

```text
reap child
然后等待另一个 callback
```

---

# 5. 更好的统一完成模型

不要让：

```text
IO callback
timer callback
manual cleanup
```

各自拥有一套 completion 算法。

建议集中：

```c
static void validation_try_finish(async_validate_ctx_t *ctx);
static void validation_finish(async_validate_ctx_t *ctx,
                              async_validate_result_t result,
                              const char *reason);
```

---

# 6. Completion state

推荐：

```c
typedef enum {
    ASYNC_VALIDATE_RUNNING = 0,
    ASYNC_VALIDATE_SUCCESS,
    ASYNC_VALIDATE_INVALID_CONFIG,
    ASYNC_VALIDATE_EXEC_FAILED,
    ASYNC_VALIDATE_TIMEOUT,
    ASYNC_VALIDATE_CANCELLED,
    ASYNC_VALIDATE_IO_ERROR,
    ASYNC_VALIDATE_INTERNAL_ERROR
} async_validate_result_t;
```

不要长期只用：

```text
1
0
-1
```

因为：

```text
invalid config
timeout
exec fail
cancel
internal error
```

对 config transaction 的语义完全不同。

---

# 7. Callback API

长期推荐：

```c
typedef void (*validate_callback_t)(
    async_validate_result_t result,
    const char *output,
    bool output_truncated,
    void *userdata);
```

如果第一轮不想破坏 API：

先内部 typed enum，再映射到旧 int。

---

# 8. `ctx` lifetime 当前没有明确契约

现在 API：

```c
int async_validate_config(async_validate_ctx_t *ctx, ...);
```

reactor callback 直接保存：

```text
ctx pointer
```

因此调用方必须保证：

```text
从 async_validate_config() 成功
直到 callback 或 cleanup 完成
ctx 始终有效
```

但 header 当前没有说明。

这是典型潜在 UAF 来源。

---

# 9. Header 必须写清

至少：

```text
The caller owns async_validate_ctx_t storage.

After async_validate_config() returns 0,
the caller MUST NOT free/reuse ctx until
completion callback has returned or cancellation
has completed.
```

同样：

```text
userdata must remain valid until callback returns
```

---

# 10. 更好的长期 API：内部拥有 ctx

长期更安全：

```c
async_validate_handle_t *
async_validate_start(...);
```

内部：

```text
calloc ctx
```

完成后 subsystem自己 free。

caller只拿 opaque handle。

但本轮不要求重写。

第一阶段先把 external-storage contract写死并测试。

---

# 11. 禁止在 active validation 上再次 memset/reuse ctx

当前 start 一开始：

```c
memset(ctx, 0, sizeof(*ctx));
```

如果 caller错误地：

```text
同一个 ctx validation还没结束
又调用 async_validate_config()
```

会直接覆盖：

```text
child_pid
reactor fd
callback
completed
```

造成严重泄漏/UAF。

---

# 12. Start 必须检测 active state

目前 `memset()` 让它无法判断旧状态。

建议增加：

```text
initialized/running magic/state
```

或者改变 API：

```text
ctx_init
start
cleanup
```

至少：

```text
active ctx cannot be restarted
```

---

# 13. 不要依靠 `completed` 既表示 initialized 又表示 lifecycle

建议明确状态：

```c
typedef enum {
    ASYNC_VALIDATE_IDLE,
    ASYNC_VALIDATE_RUNNING,
    ASYNC_VALIDATE_COMPLETING,
    ASYNC_VALIDATE_DONE
} async_validate_state_t;
```

如果严格 reactor-thread-only：

普通 enum 即可。

如果 cancellation跨线程：

再决定 atomic策略。

---

# 14. 当前 `atomic_completed` 可能掩盖线程模型不清

源码使用 atomic：

```text
atomic_exchange(completed)
```

但：

```text
reactor_remove_fd()
close fd
waitpid()
callback
```

本身并不是因为一个 atomic 就突然 thread-safe。

所以必须明确：

> async_validate lifecycle 是否只允许 reactor thread操作？

---

# 15. 推荐线程契约

推荐：

```text
start:
reactor thread / before loop

IO callback:
reactor thread

timeout callback:
reactor thread

cancel/cleanup:
reactor thread
```

如果其他线程要 cancel：

```text
post/wake reactor
```

不要直接：

```text
reactor_remove_fd + close
```

---

# 16. 如果 manual cleanup 可以跨线程调用

当前实现并不足够安全。

`atomic_completed` 只能防止：

```text
double cleanup body
```

不能防：

```text
callback正在使用 ctx
另一个线程 close fd
reactor handlers被并发修改
```

所以必须：

```text
禁止 cross-thread direct cleanup
```

或实现 reactor-post cancel。

---

# 17. Manual cleanup 的 callback 语义不清

当前：

```c
async_validate_cleanup(ctx)
→ validate_cleanup(ctx, -1, "manual cleanup")
→ callback(...)
```

所以：

```text
cleanup
```

实际同时意味着：

```text
cancel + invoke callback
```

很多调用者会误以为 cleanup 只是资源清理。

---

# 18. 推荐 API 分离

```c
int async_validate_cancel(async_validate_ctx_t *ctx);
void async_validate_destroy(async_validate_ctx_t *ctx);
```

语义：

```text
cancel
→ completes operation as CANCELLED
→ callback exactly once

destroy
→ only valid after DONE/IDLE
→ no callback
```

如果 external-storage ctx 不需要 destroy：

甚至只保留：

```text
cancel
```

---

# 19. Callback exactly once contract

最终必须定义：

以下都触发一次 callback：

```text
success
invalid config
exec failure
timeout
cancel
I/O/internal failure
```

或者明确：

```text
cancel不回调
```

二选一。

推荐：

> cancel 也回调 `CANCELLED`。

这样 config transaction 不会一直等待。

---

# 20. Callback invocation timing

当前 cleanup：

```text
remove reactor FDs
close FDs
reap/kill child
ctx->reactor = NULL
↓
callback
```

这个顺序总体正确。

保持：

> callback 被调用时 operation 已经完全脱离 reactor、child 已不再属于 ctx。

---

# 21. Callback 可以立即复用 ctx 吗？

需要明确。

如果 callback return 后：

```text
ctx已 DONE
```

可以允许 caller重新 init/start。

但是不要在 callback内部直接再次 start 同一个 ctx，除非明确支持 reentrancy。

推荐第一阶段：

```text
callback期间不要 reuse same ctx
```

---

# 22. `output` 指针 lifetime

成功时：

```text
output == ctx->output
```

timeout时可能：

```text
output == "timeout"
```

callback必须知道：

```text
output pointer only valid for duration of callback
```

如果需要保存：

```text
copy
```

写进 header。

---

# 23. P1：pipe 应用 `pipe2()`

当前：

```c
pipe(pipe_fds)
fcntl(read_fd, O_NONBLOCK)
```

推荐 Linux/Android直接：

```c
pipe2(pipe_fds, O_CLOEXEC | O_NONBLOCK);
```

但 child stdout/stderr write end是否希望 nonblocking需要考虑。

更合适：

```text
pipe2(O_CLOEXEC)
read end再 O_NONBLOCK
```

或：

```text
pipe2(O_CLOEXEC | O_NONBLOCK)
child dup2后清除 stdout/stderr O_NONBLOCK
```

简单方案：

```text
pipe2(O_CLOEXEC)
fcntl(read end, O_NONBLOCK)
```

---

# 24. 为什么 `CLOEXEC` 必须有

ATPD 是长期 daemon。

validation运行期间 ATPD可能：

```text
fork/exec sing-box/service
```

没有 CLOEXEC 的 parent pipe fd可能泄漏进其他 child process。

即使不造成 EOF阻塞：

仍然是 FD ownership污染。

---

# 25. Child dup2 后 CLOEXEC

`dup2(pipe_write, STDOUT_FILENO)`：

新 fd：

```text
STDOUT/STDERR
```

不会保留 close-on-exec，这正是需要的。

原始 pipe write fd再：

```text
close
```

即可。

---

# 26. `timerfd` 已使用 `TFD_CLOEXEC`

这一点是正确的。

保持。

---

# 27. IO callback 应 drain 到 EAGAIN

当前：

```c
read(fd, buf, 1023);
if n > 0:
    append
    return;
```

每次 callback只读一块。

如果 reactor未来使用 edge-triggered或者 event coalescing：

可能导致剩余数据长时间不被消费。

推荐标准 nonblocking pattern：

```text
for:
    read
    >0 → append
    0  → EOF
    EINTR → retry
    EAGAIN → stop
    other → fail
```

---

# 28. 即使当前是 level-triggered，也建议 drain

因为：

```text
validator输出可能很多
```

完整 drain减少 reactor wakeup次数。

但必须设置每 callback work budget，防止 child无限输出饿死 reactor。

---

# 29. Read budget

例如：

```text
最多 drain 64 KiB / callback
```

而实际保存：

```text
最多 4095 bytes
```

超出后：

```text
继续读取并丢弃
```

直到 EAGAIN，避免 pipe fill阻塞 validator child。

---

# 30. 当前 output truncation 有一个重要行为

buffer满后：

```text
output_truncated = 1
```

但仍必须继续读 pipe。

否则 validator输出超过 4 KiB后：

```text
child可能因为 pipe满而阻塞
```

导致原本很快的 validation变成 timeout。

所以：

> 保存可以截断，消费不能停止。

---

# 31. 输出 buffer可以继续保持 4 KiB

不必无限增长。

推荐：

```text
capture first 4 KiB
or
capture last 4 KiB
```

对于错误诊断，通常最后几 KiB更有价值。

可选 ring/tail buffer。

第一轮保留 first 4 KiB即可。

---

# 32. EOF 与 child exit 是两个独立条件

正确完成条件应理解为：

```text
pipe EOF
+
child status known
```

两个事件先后顺序都可能不同：

```text
A:
child exits → pipe EOF

B:
child closes stdout/stderr → pipe EOF → child later exits
```

因此不能假设：

```text
EOF == child exited
```

当前源码已经遇到了这个边界。

---

# 33. 推荐状态字段

```c
bool pipe_eof;
bool child_reaped;
int child_status;
```

或者 internal state。

然后：

```text
IO EOF → pipe_eof = true
try_finish()

SIGCHLD/timer/check → child_reaped = true
try_finish()
```

---

# 34. 最好利用统一 child-reap机制

ATPD已经有 service child supervision。

需要全仓审计：

```text
SIGCHLD是谁处理
是否 waitpid(-1)
是否只 wait service child
```

如果全局 SIGCHLD handler会：

```text
waitpid(-1)
```

那么 async_validate自己 waitpid会竞争。

---

# 35. 必须建立 child ownership registry

长期最好：

```text
PID → owner
```

service child：

```text
service supervisor
```

validation child：

```text
async_validate
```

由一个统一 SIGCHLD dispatcher：

```text
waitpid(-1, WNOHANG)
↓
dispatch PID
```

这是最稳的。

---

# 36. 如果当前 service只 wait自己的 PID

可以暂时让 async_validate继续自己 waitpid。

但必须写入 architecture contract：

```text
service must never reap validation child
```

---

# 37. timeout本身不是 child exit notification

当前用 5 秒 timer同时：

```text
超时
+
顺便检查 child是否已经退出
```

这是 workaround。

更理想：

```text
SIGCHLD → immediate completion
timerfd → only timeout
pipe → output
```

这样 validator 0.1 秒退出就能立刻完成，而不用等待另一个 pipe edge。

---

# 38. 推荐最终事件模型

```text
child stdout/stderr pipe
        ↓
capture output

SIGCHLD
        ↓
capture exit status

timer
        ↓
enforce timeout
```

完成：

```text
child_reaped
+
pipe drained/EOF
```

或者 child exited后主动 drain remaining pipe直到 EOF。

---

# 39. 如果不想现在引入 SIGCHLD dispatcher

最低成本修复：

IO callback：

```text
drain pipe
EOF
→ waitpid WNOHANG
→ 若已退出则 finish
→ 否则记录 pipe_eof
```

timer callback：

```text
waitpid WNOHANG with status
→ if exited:
     drain remaining pipe
     finish from status
→ if running:
     kill
     reap
     timeout finish
```

至少不会永久挂起。

---

# 40. P1：timeout kill + blocking `waitpid(...,0)`

当前：

```text
kill(SIGKILL)
waitpid(pid, NULL, 0)
```

发生在 reactor callback。

SIGKILL正常很快，但：

> reactor callback里依然不应依赖无界 blocking wait。

---

# 41. 最低限度处理 EINTR

现在：

```c
waitpid(pid, NULL, 0);
```

返回值没检查。

如果：

```text
EINTR
```

child可能未 reap。

推荐 helper：

```c
static int reap_blocking_eintr(pid_t pid, int *status);
```

循环只处理：

```text
EINTR
```

---

# 42. 更好的完全异步 kill/reap

timeout：

```text
SIGKILL
state = KILLING
```

然后通过：

```text
SIGCHLD
```

完成 reap。

可以另设非常短 fallback timer。

但不必第一 commit一次做完。

---

# 43. `kill()` 返回值必须检查

当前 timeout：

```c
kill(pid, SIGKILL);
waitpid(...)
```

未检查 kill。

可能：

```text
ESRCH
EPERM
```

需要区分：

```text
ESRCH → 可能已退出，尝试 waitpid
EPERM → internal/ownership error
```

---

# 44. PID reuse风险

只要 child从 fork后一直由 parent持有且未 reap：

```text
PID不会被重用
```

一旦 child被其他 SIGCHLD handler提前 reap：

ctx仍保存旧 PID，之后：

```text
kill(ctx->child_pid, SIGKILL)
```

理论上有 PID reuse风险。

所以：

> child必须只能由唯一 owner reap。

---

# 45. Exec failure 当前只能看到 exit 127

child：

```c
execl(...)
_exit(127);
```

caller无法区分：

```text
sing-box check主动 exit 127
```

和：

```text
exec本身失败
```

建议增加 exec-error pipe，或者至少 child在 exec前：

```text
dprintf stderr
```

报告：

```text
exec failed: errno
```

但 `dprintf` 在 fork后多线程安全方面需谨慎。

---

# 46. 推荐 exec error pipe

类似 service refactor：

```text
O_CLOEXEC error pipe
```

child：

```text
exec success
→ CLOEXEC自动关闭

exec fail
→ write errno
→ _exit(127)
```

parent可分类：

```text
EXEC_FAILED
```

这是最干净的。

---

# 47. `dup2()` 返回值应检查

当前：

```text
dup2 stdout
dup2 stderr
```

均忽略失败。

child应该：

```text
任一步失败 → report exec/setup error → _exit
```

否则 validation可能运行但没有输出 capture。

---

# 48. `/dev/null` open/dup2 failure

stdin redirect失败不是一定 fatal。

但至少：

```text
不应执行任何复杂 logging
```

child path尽量只用 async-signal-safe操作。

---

# 49. `work_dir` 必须验证

API只检查：

```text
ctx
reactor
bin_path
callback
```

但 child无条件：

```text
"-D", work_dir
```

如果：

```text
work_dir == NULL
```

execl varargs 会产生错误的 argv语义。

因此：

```text
work_dir必须 non-null + non-empty
```

或者明确：

```text
可选时不要添加 -D
```

---

# 50. `conf_path` 可选语义目前合理

如果：

```text
NULL/empty
```

就不加：

```text
-c
```

保持。

---

# 51. `bin_path` 预检查

不要求通过：

```text
access(X_OK)
```

代替 exec truth。

可以用于更清晰错误，但真正 authority：

```text
exec result
```

---

# 52. Timeout 不应硬编码在实现内部

当前：

```text
5秒
```

未来 config transaction可能需要不同 policy。

推荐 API/context：

```c
uint32_t timeout_ms;
```

默认：

```text
5000 ms
```

---

# 53. 配置验证 timeout 策略

本机：

```text
sing-box check
```

正常应很快。

5 秒可作为保守上限。

如果连续 timeout：

status应看到：

```text
validation_timeouts++
```

---

# 54. Validation generation

和 transactional config reload结合时非常重要。

可能发生：

```text
reload #12 starts validation
reload #13 requested
#12 callback later到
```

必须防止：

```text
旧 validation result提交新 transaction
```

---

# 55. 推荐 request generation

```c
uint64_t validation_id;
uint64_t config_generation;
```

callback携带：

```text
transaction id
```

上层确认：

```text
仍是当前 active transaction
```

否则：

```text
discard stale result
```

---

# 56. 更简单：只允许一个 config reload

我们已经建议：

```text
one reload transaction at a time
```

如果严格 serialize：

generation race会简单很多。

仍建议给 validation自身 request id用于 debug/status。

---

# 57. Cancel 与 config transaction

当 reload transaction被取消/daemon shutdown：

```text
async_validate_cancel
↓
child terminate/reap
↓
callback CANCELLED
↓
transaction abort
```

这条链必须完整。

---

# 58. Shutdown ordering

推荐：

```text
stop accepting reload
↓
cancel active validation
↓
wait async validation completion/reap
↓
destroy reactor
```

不能：

```text
destroy reactor
↓
validation ctx仍保存 reactor pointer
```

---

# 59. Reactor removal return

cleanup：

```text
reactor_remove_fd
```

目前忽略返回。

如果 remove失败而随后：

```text
close fd
```

reactor handler table可能还保留旧 userdata。

结合 reactor stale-FD风险非常危险。

---

# 60. 和 reactor加固方案联动

需要 reactor contract保证：

```text
remove_fd 成功
→ handler彻底失效

remove_fd失败
→ caller知道是否仍 registered
```

async_validate不能假定：

```text
remove失败也没关系，直接close
```

---

# 61. FD registration state

建议 ctx记录：

```c
bool pipe_registered;
bool timer_registered;
```

只有成功 add后置 true。

cleanup：

```text
if registered
→ remove
→ false
```

避免靠：

```text
fd >= 0
```

推断 reactor registration。

---

# 62. Add second FD失败 rollback

当前 timer add失败：

```text
remove pipe
goto fail
```

总体方向正确。

必须测试：

```text
pipe registration success
timer registration fail
```

验证：

```text
reactor handler removed
pipe closed
timer closed
child killed/reaped
callback不触发（因为 start返回失败）
```

---

# 63. Start failure callback policy

推荐明确：

```text
async_validate_config() 返回 -1
→ operation从未成功启动
→ callback NEVER invoked
```

caller同步处理 start failure。

一旦返回 0：

```text
callback exactly once
```

这是最清晰的 contract。

---

# 64. 不要 start failure 有时回调、有时不回调

当前都是：

```text
return -1
```

不 callback。

保持这个一致性。

---

# 65. Callback reentrancy

callback可能调用：

```text
config commit
config reload
service operation
```

因此在 callback前必须：

```text
validation subsystem完全处于 DONE
```

当前 cleanup大体这样做，继续保持。

---

# 66. Callback之后不要访问 ctx

如果 callback允许：

```text
free owner object
```

那么 `validate_cleanup()` 在 callback return后不能再访问：

```text
ctx
userdata
```

当前 callback在函数最后，这是正确布局。

保持。

---

# 67. Logging child output

不要自动把完整 4 KiB输出全部写 log。

可能包含：

```text
paths
config details
secret-adjacent data
```

callback/CLI按需要展示。

daemon log最多摘要。

---

# 68. Validation output status

建议保存：

```text
truncated=yes/no
```

让 caller知道错误信息不完整。

---

# 69. Metrics

可增加轻量 counters：

```text
started
succeeded
invalid
exec_failed
timed_out
cancelled
internal_failed
output_truncated
active
```

方便 soak和 status。

---

# 70. 不需要专门 status大段展示

正常：

```text
Config validator: idle
Last validation: success
```

异常：

```text
Last validation: timeout
```

足够。

---

# 71. Test：EOF before child exit

这是必须新增的核心回归测试。

child：

```text
close stdout/stderr
sleep
exit 0
```

预期：

```text
callback eventually SUCCESS
```

不能卡住。

---

# 72. Test：child exit before timeout, EOF已消费

专门模拟刚才的 bug时序。

预期：

```text
timeout callback若发现已退出
→ 自己完成
```

callback exactly once。

---

# 73. Test：normal success

validator：

```text
output
exit 0
```

验证：

```text
SUCCESS
output captured
child reaped
fds closed
callback once
```

---

# 74. Test：config invalid

validator：

```text
stderr error
exit != 0
```

结果：

```text
INVALID_CONFIG
```

而不是 generic internal failure。

---

# 75. Test：exec fail

```text
bin_path nonexistent
```

预期：

```text
EXEC_FAILED
```

如果第一阶段还没 exec-error pipe：

至少：

```text
failure + useful output
```

---

# 76. Test：timeout

child：

```text
sleep 30
```

timeout：

```text
kill
reap
callback TIMEOUT exactly once
```

---

# 77. Test：cancel before output

```text
start
immediate cancel
```

验证：

```text
CANCELLED
child reaped
callback once
```

---

# 78. Test：cancel after child exit race

交错：

```text
child exits
cancel called
IO callback pending
```

仍然：

```text
callback once
```

---

# 79. Test：timeout vs IO callback race

虽然单 reactor线程一般是序列化 callback：

同一 epoll batch可能两事件都 ready。

测试：

```text
pipe event
timer event
```

无论 dispatch顺序：

```text
cleanup once
callback once
```

---

# 80. Test：output > 4 KiB

child输出：

```text
1 MB
```

预期：

```text
output_truncated=1
child不会因 pipe fill卡住
validation按 exit status完成
ATPD memory bounded
```

---

# 81. Test：fragmented output

单字节/小块输出。

验证：

```text
正确 capture
```

---

# 82. Test：EINTR

对：

```text
read
waitpid
kill/reap helper
```

故障注入或 signal stress。

不能：

```text
错误认为 child失败
```

---

# 83. Test：reactor add pipe failure

预期：

```text
start returns -1
child killed/reaped
no callback
no FD leak
```

---

# 84. Test：reactor add timer failure

预期：

```text
pipe registration rollback
child killed/reaped
no callback
no FD leak
```

---

# 85. Test：reactor remove failure

配合 reactor fault injection。

确保不会产生：

```text
stale callback with freed ctx
```

如果当前 reactor contract无法安全恢复：

至少明确：

```text
fatal invariant
```

并修 reactor。

---

# 86. Test：same ctx double start

```text
start(ctx)
start(ctx) again before completion
```

第二次必须：

```text
EBUSY/-1
```

不能 memset掉第一条 operation。

---

# 87. Test：ctx reuse after callback

第一条 callback结束后：

```text
same ctx start again
```

如果设计允许：

必须正常。

如果不允许：

header明确。

推荐允许 after DONE。

---

# 88. Test：shutdown with active validation

```text
start long validation
daemon shutdown
```

验证：

```text
cancel
reap
reactor clean
no callback UAF
```

---

# 89. Stress

```text
10,000 validation cycles
```

包括：

```text
success
invalid
timeout
cancel
exec fail
```

检查：

```text
FD baseline
RSS stable
0 zombies
callbacks == successfully started operations
```

---

# 90. Zombie检测

压力后：

```text
/proc
waitpid
ps
```

确认：

```text
0 defunct validation children
```

---

# 91. PID ownership test

同时运行：

```text
sing-box service child
validation child
```

验证：

```text
service supervisor不会 reap/kill validation child
validator不会碰 service child
```

---

# 92. Sanitizers

```text
ASan
UBSan
```

如果 lifecycle严格 reactor-thread-only：

不必强行用 TSan。

如果提供跨线程 cancel：

必须跑：

```text
TSan
```

---

# 93. 推荐 Commit 1

```text
async-validate: fix child completion and timeout lifecycle
```

内容：

- timeout已reap立即完成
- 保存 wait status
- EOF/exit顺序修复
- EINTR处理
- regression tests

---

# 94. Commit 2

```text
async-validate: define context and callback ownership
```

内容：

- header lifecycle contract
- active/done state
- prevent double start
- userdata/output lifetime docs

---

# 95. Commit 3

```text
async-validate: make cancellation explicit
```

内容：

- cancel API
- CANCELLED result
- manual cleanup语义清理
- shutdown tests

---

# 96. Commit 4

```text
async-validate: harden pipe and reactor ownership
```

内容：

- CLOEXEC
- registration flags
- rollback
- complete read drain
- large-output tests

---

# 97. Commit 5

```text
async-validate: distinguish validation failures
```

内容：

- typed result enum
- exec failure
- timeout
- invalid config
- protocol/internal errors

---

# 98. Commit 6

```text
async-validate: integrate validation with config transaction
```

内容：

- transaction/request generation
- stale completion防护
- reload serialization
- abort semantics

---

# 99. 可选 Commit 7

```text
async-validate: integrate validator children with SIGCHLD dispatcher
```

如果 service refactor已经建立统一 child dispatcher：

把：

```text
validation child
```

一起纳入。

否则先保持 validator-own waitpid也可以。

---

# 100. Codex修改前必须先全仓搜索

```text
async_validate_config(
async_validate_cleanup(
async_validate_ctx_t
validate_callback_t
```

列出：

```text
ctx storage在哪里
ctx什么时候 free
userdata是谁
callback里做什么
cleanup在哪些 shutdown路径调用
是否存在 double start
是否存在 stack ctx
```

尤其：

> 如果任何 caller 把 `async_validate_ctx_t` 放在短生命周期 stack frame 后立即 return，这是直接 UAF风险。

必须先确认。

---

# 101. Codex还必须审计 SIGCHLD ownership

搜索：

```text
waitpid(
waitid(
SIGCHLD
signalfd
```

回答：

```text
谁可能 reap validator child？
```

如果存在：

```text
waitpid(-1)
```

必须先统一 child dispatcher。

---

# 102. 与 `config.c` transactional reload 的关系

推荐最终：

```text
reload transaction
↓
load candidate
↓
pure validation
↓
async sing-box `check`
↓
SUCCESS
↓
runtime prepare
↓
commit
```

如果：

```text
INVALID_CONFIG
TIMEOUT
EXEC_FAILED
CANCELLED
```

transaction：

```text
abort
old config保持
```

---

# 103. `sing-box check` 的角色

它验证的是：

```text
sing-box config runtime/schema validity
```

而不是 ATPD自己的所有 config。

因此仍然保留：

```text
config_validator.c
```

做 ATPD pure validation。

两层：

```text
ATPD validator
+
sing-box check
```

职责不重复。

---

# 104. 不要让 async validator修改 config

它应该纯粹：

```text
输入 paths
运行 child
返回 result/output
```

不能：

```text
commit config
restart service
update global cfg
```

这些属于 caller transaction。

---

# 105. 是否拆文件

答案：

```text
不拆
```

331行是非常合理的大小。

即使加入：

```text
state
typed result
cancel
helper
```

预计仍在：

```text
350–450 LOC
```

足够清晰。

---

# 106. 如果未来统一 child supervisor

那时也不要拆 async_validate。

可以抽一个通用：

```text
child_process.c
```

但只有：

```text
service
async_validate
```

确实共享可靠 fork/exec/reap primitives时再做。

不要提前抽象。

---

# 107. 最终 Invariants

Codex必须通过源码和测试保证：

```text
I1:
async_validate_config() return 0
→ callback exactly once

I2:
async_validate_config() return -1
→ callback never fires

I3:
each validator child is reaped exactly once

I4:
validation cannot remain RUNNING after its child has been reaped

I5:
timeout/cancel cannot leave child or FD behind

I6:
active ctx cannot be restarted/reused

I7:
ctx/userdata remain valid until completion callback returns

I8:
all successfully registered reactor FDs are removed before ctx becomes reusable

I9:
captured output may truncate, but pipe consumption must continue

I10:
validator child ownership never overlaps service child ownership
```

---

# 108. 最终验收标准

## Completion

```text
EOF-before-exit
exit-before-timeout
timeout-vs-IO
```

全部：

```text
callback exactly once
```

## Child

```text
0 zombies
```

## FD

```text
success/fail/cancel/timeout
→ FD returns baseline
```

## Output

```text
1 MB child output
→ bounded memory
→ no child pipe deadlock
```

## Lifecycle

```text
double start rejected
reuse after DONE works
```

## Shutdown

```text
active validation cancelled/reaped before reactor destruction
```

## Config

```text
validation failure
→ transactional reload does not commit candidate
```

---

# 109. 最终结论

`async_validate.c` 当前架构思路是对的：

```text
fork
+
pipe
+
timerfd
+
reactor
```

比在 reactor里同步执行：

```text
sing-box check
```

好得多。

这个模块不需要重写，也不需要拆文件。

真正要做的是：

> 把 child exit、pipe EOF、timeout、cancel、callback 和 ctx lifetime 统一成一个可证明 exactly-once 的异步生命周期。

最优先修复：

```text
1. timeout callback reap child后不能直接 return
2. ctx/userdata lifetime写成硬 contract
3. active ctx禁止重复 start
4. pipe持续 drain，即使 output已经截断
5. validator child与 service child的 SIGCHLD ownership明确
```

完成以后，它会非常适合作为 `config.c` transactional reload 的异步验证阶段。
