# ATPD `splice.c` 数据通路与重复实现收敛方案

## 1. 模块结论

当前：

```text
src/splice.c      ~348 lines
include/splice.h  ~71 lines
```

单看 `splice.c`，它已经做了不少正确工作：

```text
pipe2(O_NONBLOCK | O_CLOEXEC)
per-connection splice_state
pipe_pending accounting
先 drain pending，再读取新数据
bytes_in / bytes_out 分离
每 event work cap
EPOLLET/backpressure 意识
```

但是全仓结合 `session.c` 看，出现了一个更重要的问题：

> ATPD 当前实际上存在两套 splice datapath 实现。

第一套：

```text
splice.c
└─ atpd_bridge_splice_stateful()
   ├─ splice_state_t
   ├─ pipe_pending
   ├─ pipe_capacity
   └─ bytes_in/out
```

第二套：

```text
session.c
└─ atpd_session_splice_pump()
   ├─ session-owned pipe_fds
   ├─ atomic pipe_pending
   ├─ bytes_in/out
   ├─ PIPE_DIRTY / DRAINING
   └─ reactor WRITE interest
```

两套都直接调用：

```text
splice(2)
```

维护几乎相同的 backpressure/state。

因此本轮第一目标不是继续给 `splice.c` 堆功能，而是：

> 先确认生产路径到底使用哪一套，然后收敛到一个 authoritative datapath implementation。

---

# 2. Codex 第一件事：全仓调用图

必须搜索：

```text
atpd_bridge_splice_stateful(
atpd_bridge_splice(
atpd_splice_state_init(
atpd_splice_state_cleanup(
atpd_splice_pipe_init(
atpd_splice_pipe_cleanup(
splice_state_t
atpd_session_splice_pump(
atpd_session_drain_pipe(
```

输出：

```text
symbol
definition
callers
production path?
test-only?
legacy?
replacement
delete?
```

不能根据文件名判断是否还在使用。

---

# 3. 首选架构方向

如果审计确认真实 production datapath 已经全部由：

```text
session.c
```

负责，那么推荐：

```text
删除 splice.c/splice.h
```

而不是继续维护两套 pump。

因为 session 已经必须知道：

```text
session lifecycle
VPN state
pipe dirty state
reactor EPOLLOUT
session close
GC
metrics
```

独立 `splice_state_t` 很难脱离 session 独立存在。

---

# 4. 为什么两套实现危险

今天两套已经出现行为差异。

例如：

```text
session.c
→ EINTR retry

splice.c
→ EINTR falls into fatal error
```

这意味着同一种 kernel condition：

```text
不同调用路径
→ 不同结果
```

长期必然漂移。

---

# 5. 另一个差异：backpressure ownership

`session.c` 在：

```text
pipe -> fd_out
```

遇到 EAGAIN 时：

```text
pipe_pending = remaining
state = PIPE_DIRTY
enable EPOLLOUT
```

这与 reactor/session state machine 直接绑定。

而 `splice.c` 只返回：

```text
ATPD_SPLICE_EAGAIN
```

它本身不知道：

```text
谁应该 enable EPOLLOUT
谁负责 drain
谁负责 session close
```

因此 production path最终还是需要 session 层重新实现状态机。

---

# 6. 推荐最终职责

二选一。

### 方案 A：session 是 authoritative datapath owner

推荐。

```text
session.c
├─ splice pump
├─ pipe state
├─ backpressure
├─ reactor interest
└─ lifecycle
```

然后删除 standalone `splice.c`。

### 方案 B：splice.c 是 pure low-level pump

只有在确实存在多个调用方时才值得。

此时：

```text
splice.c
```

必须只做纯 datapath primitive：

```text
input/output/pipe/result
```

而：

```text
session.c
```

不能再自己写另一套 splice loop。

---

# 7. 不建议保留“两个都能用”

不要：

```text
session使用自己 pump
其他地方偶尔使用 splice.c
```

因为两套对：

```text
EAGAIN
EINTR
EOF
EPIPE
NOTSUP
pipe_pending
max work
```

的行为很容易再次分叉。

---

# 8. 当前明确问题：`splice.c` 没处理 EINTR

在：

```text
pipe -> fd_out
```

以及：

```text
fd_in -> pipe
```

错误分支中，当前只特殊处理：

```text
EAGAIN
EPIPE
ECONNRESET
EINVAL
ENOSYS
ESPIPE
```

没有：

```text
EINTR
```

所以 signal interruption会被：

```text
ATPD_SPLICE_ERROR
```

处理。

---

# 9. EINTR 正确语义

所有 splice syscall：

```c
if (errno == EINTR)
    continue;
```

但要注意 work budget。

不能因为持续 signal：

```text
无限 retry
```

最好每次 loop仍受：

```text
max bytes
or
iteration budget
```

约束。

---

# 10. P0/P1：NOTSUP 发生在 pipe 已经持有数据以后

最危险的时序：

```text
PHASE 2
fd_in → pipe
成功读取 N bytes
pipe_pending = N

PHASE 3
pipe → fd_out
splice returns EINVAL / ENOSYS / ESPIPE
↓
ATPD_SPLICE_NOTSUP
```

此时：

```text
N bytes 已经从 fd_in 消耗
但仍留在 kernel pipe
```

---

# 11. 如果 caller 接着 fallback 到普通 read/write

caller很可能：

```text
NOTSUP
→ switch to read(fd_in) / write(fd_out)
```

那么那 N bytes：

```text
不会重新出现在 fd_in
```

导致：

```text
data loss
or
stream reordering
```

这是不能接受的。

---

# 12. NOTSUP 的硬 invariant

最终必须保证：

> `ATPD_SPLICE_NOTSUP` 只能在尚未从 input 消耗任何数据时返回。

即：

```text
pipe_pending == 0
bytes consumed this call == 0
```

---

# 13. 如何实现

最佳方式：

启动 session前一次性确认：

```text
input/output pair是否支持 splice
```

然后选定 transport mode：

```text
SPLICE
or
READ_WRITE
```

不要运行中已经搬了一半数据才切模式。

---

# 14. 但不要为了 probe 破坏 stream

不要做会消耗数据的测试 splice。

更简单：

```text
第一次 fd_in→pipe EINVAL
且 pipe为空
→ mark NOTSUP
→ fallback safe
```

一旦任何数据进入 pipe：

```text
本 connection必须把它 drain完
```

不能直接切 ordinary read。

---

# 15. output side NOTSUP 怎么处理

如果：

```text
input splice成功
output splice不支持
```

可以有几种方案。

最简单、最安全：

```text
treat connection as fatal
```

因为已经有 buffered data。

如果真的需要 fallback：

必须：

```text
read() from pipe
→ write() to fd_out
```

把 pending data先恢复到 userspace forwarding。

复杂度明显增加。

---

# 16. ATPD 当前最适合哪种

因为目标平台是：

```text
modern Android/Linux
```

且 session datapath就是 socket/pipe forwarding，

如果 output splice出现：

```text
EINVAL/ENOSYS/ESPIPE
```

更合理的是：

```text
session fatal + telemetry
```

而不是在中途做复杂 fallback。

前提是确认生产 FD 类型。

---

# 17. Return code存在信息丢失

当前如果：

```text
已经成功写出一部分
然后 EPIPE
```

代码倾向：

```text
return positive bytes
```

而不是：

```text
EOF
```

这样 caller只知道：

```text
made progress
```

不知道：

```text
peer已经关闭
```

---

# 18. 结果模型最好区分 progress 与 terminal condition

单个 ssize_t 很难同时表达：

```text
moved 4096 bytes
AND
peer closed
```

长期推荐：

```c
typedef enum {
    SPLICE_FLOW_OK,
    SPLICE_FLOW_WOULD_BLOCK,
    SPLICE_FLOW_EOF,
    SPLICE_FLOW_NOTSUP,
    SPLICE_FLOW_ERROR
} splice_flow_status_t;

typedef struct {
    size_t bytes_forwarded;
    splice_flow_status_t status;
    int sys_errno;
} splice_result_t;
```

这样可以：

```text
bytes_forwarded = 4096
status = EOF
```

---

# 19. 第一阶段不必马上改 public API

如果最终决定删除 `splice.c`：

没必要投资 typed result。

如果保留 standalone module：

建议尽快改。

---

# 20. `0` 的语义也不够清楚

当前：

```text
ATPD_SPLICE_OK = 0
```

但 syscall：

```text
splice() == 0
```

通常被解释为 EOF。

因此上层必须区分：

```text
函数返回0 = no data / no progress
```

和：

```text
-1 = EOF
```

可以工作，但 API可读性较差。

---

# 21. `max_len=0` 文档与实现不完全一致

header写：

```text
max_len = 0 → no limit per call
```

但实现会：

```text
remaining_limit = SIZE_MAX
↓
cap to ATPD_SPLICE_MAX_PER_EVENT
↓
4 MiB
```

所以实际语义：

```text
0 = use per-event default upper bound
```

而不是：

```text
无限
```

---

# 22. 推荐修改文档

改成：

```text
max_len = 0
→ no caller-specified limit;
implementation still applies fairness/event budget
```

这样准确。

---

# 23. 4 MiB per callback是否过大

当前：

```text
ATPD_SPLICE_MAX_PER_EVENT = 4 MiB
```

在单线程 reactor：

```text
一个高速 session
```

可能一次 callback持续搬 4 MiB，

导致：

```text
UDS
Netlink
timers
其他sessions
```

延迟增加。

---

# 24. 建议 benchmark fairness

不要凭感觉改数字。

测试：

```text
1 high-throughput session
+
100 idle/light sessions
+
status/UDS requests
```

比较：

```text
64 KiB
256 KiB
1 MiB
4 MiB
```

观察：

```text
throughput
p99 reactor callback latency
status latency
timer delay
```

---

# 25. 初始建议

对于单线程 control daemon：

```text
256 KiB–1 MiB/event
```

通常比 4 MiB更保守。

但必须 benchmark决定。

---

# 26. Pipe capacity是假定值风险

如果：

```c
F_SETPIPE_SZ
```

失败，

代码尝试：

```c
F_GETPIPE_SZ
```

这很好。

但如果：

```text
F_GETPIPE_SZ也失败
```

当前直接：

```text
actual = requested = 64 KiB
```

这不是实际 capacity，只是猜测。

---

# 27. 不要把 guessed value叫 `pipe_capacity`

如果无法查询：

可以：

```text
capacity_unknown
```

然后 chunk使用保守值。

或者：

```text
pipe_capacity = 0
```

表示 unknown。

不能把一个假的 64 KiB作为 kernel事实。

---

# 28. 为什么虽然不一定造成 corruption，仍要修

kernel最终会限制实际 pipe写入量。

所以：

```text
不会因为这个字段直接越界kernel buffer
```

但：

```text
debug invariant
chunk calculation
telemetry
```

都会基于错误事实。

---

# 29. F_SETPIPE_SZ 只需要调用一个端

pipe容量属于整个 pipe。

当前 standalone state只对：

```text
read end
```

调用一次，是合理的。

session.c则对两个 pipe fd都调用：

```text
F_SETPIPE_SZ
```

后者是重复操作。

可以在 session优化时清掉一份。

---

# 30. Pipe size failure 不应是 fatal

普通 kernel默认 pipe size通常仍可工作。

所以：

```text
WARN + continue
```

合理。

但最好记录实际：

```text
F_GETPIPE_SZ
```

---

# 31. Debug `FIONREAD` invariant不错

当前 DEBUG：

```text
FIONREAD(pipe_read)
==
state->pipe_pending
```

这个检查非常有价值。

如果 standalone splice保留：

继续保留。

---

# 32. 生产测试也应该验证 pending invariant

通过 instrumentation/fault tests：

```text
bytes_in == bytes_out + pipe_pending
```

每次 pump后都应成立。

---

# 33. 但 session.c 当前使用另一份 accounting

这再次说明：

```text
同一 invariant
```

被维护两套。

最终应该只剩一份。

---

# 34. Drain phase的 `splice()==0`

当：

```text
pipe_pending > 0
```

却：

```text
pipe -> fd_out splice == 0
```

这是异常/terminal condition。

当前返回：

```text
EOF
```

但没有修正：

```text
pipe_pending
```

这意味着 state仍认为数据存在。

如果 caller随后 cleanup session：

数据会被丢弃。

---

# 35. 这里应明确语义

如果 destination已经不可写/关闭：

```text
remaining pending data无法交付
```

session应该：

```text
mark closing
record dropped_pending bytes
cleanup
```

不要假装：

```text
clean EOF
```

---

# 36. Source EOF 与 destination failure不能用一个 EOF概念

两种情况：

```text
fd_in splice == 0
→ source EOF

pipe -> fd_out EPIPE/zero
→ destination terminal/failure
```

对 TCP half-close来说，它们的行为不同。

---

# 37. 建议 typed terminal reason

如果保留 module：

```text
SOURCE_EOF
DEST_CLOSED
WOULD_BLOCK
NOT_SUPPORTED
IO_ERROR
```

而不是全部：

```text
ATPD_SPLICE_EOF
```

---

# 38. Half-close必须有明确策略

TCP场景可能：

```text
input方向 EOF
```

不代表：

```text
整个 bidirectional connection应该立即 close
```

如果 ATPD session是单向桥接对象：

当前可以 close。

如果一个 session同时代表双向 TCP：

必须：

```text
shutdown(fd_out, SHUT_WR)
```

并保留反方向。

---

# 39. Codex 必须确认 session的方向模型

搜索：

```text
atpd_session_create(fd_in, fd_out)
```

确认一个 session到底代表：

```text
one direction
```

还是：

```text
whole connection
```

不能凭 `fd_in/fd_out` 名字猜。

---

# 40. 如果是单向 session

那么：

```text
source EOF
→ drain pending
→ direction complete
```

而不是立即丢 pending。

这一点非常重要。

---

# 41. Source EOF 时必须先看 pipe_pending

standalone实现因为每次先 drain：

通常 source EOF出现时 pipe应为空。

但跨 callback情况下必须保持 invariant。

session.c也必须确认：

```text
EOF不会发生时还有 previous pending未 drain
```

---

# 42. `session_in_cb` 当前先 drain PIPE_DIRTY

这一方向总体正确：

```text
pending优先
→ 再读 input
```

应保持。

---

# 43. session 的 `reactor_modify_fd` 返回值仍是关键风险

遇到 output EAGAIN：

```c
reactor_modify_fd(... WRITE ...)
```

当前返回值没处理。

如果失败：

```text
pipe_pending > 0
state = PIPE_DIRTY
但没有 EPOLLOUT
```

session可能永久卡住。

这个问题已经在 session计划里提出。

splice收敛时必须一起处理。

---

# 44. 所以 standalone splice不能单独解决 backpressure

真正 invariant是：

```text
pending > 0
→ writable notification一定存在
```

这属于：

```text
session + reactor transaction
```

进一步支持 session作为 authoritative owner。

---

# 45. Legacy API应该优先删除

当前：

```c
atpd_bridge_splice(
    fd_in,
    fd_out,
    pipe_fds,
    max_len)
```

会构造临时：

```text
local_state.pipe_pending = 0
```

它不知道 pipe里是否已经有之前 callback留下的数据。

这正是 header自己也承认：

```text
不适用于 EPOLLET/backpressure
```

---

# 46. 如果全仓没有 caller

直接删除：

```text
atpd_bridge_splice
deprecated attribute
runtime warning
```

不要为了兼容不存在的外部用户继续留。

---

# 47. `warned` static int也没有线程同步

虽然 ATPD大概率单线程，

但如果 API已经废弃：

不值得修。

直接删除更好。

---

# 48. `atpd_splice_pipe_init/cleanup` 也要审计

如果只有 legacy/standalone module使用：

一起删除。

不要留下：

```text
裸 pipe helper
```

让未来代码重新绕开 session state。

---

# 49. Header最终应更小

如果 production only session：

整个：

```text
include/splice.h
```

都可能删除。

错误码如果 session仍使用：

迁移到：

```text
session internal result enum
```

或专门小型 flow result header。

---

# 50. 现在 session.c 使用的错误码来源要审计

它返回：

```text
ATPD_SPLICE_ERROR
ATPD_SPLICE_EOF
ATPD_SPLICE_NOTSUP
ATPD_SPLICE_VPN_NOT_READY
```

需要检查：

```text
这些 enum/macro到底定义在哪里
```

如果删除 splice.h：

保留真正属于 session的数据通路 result定义。

---

# 51. 不要让 session public API暴露 Linux splice implementation

长期 public API应该表达：

```text
session pump / forwarding result
```

而不是：

```text
splice syscall result
```

因为未来即便更换：

```text
io_uring
read/write
sendfile-style
```

session caller也不应改变。

---

# 52. 推荐命名

如果 result属于 session：

```c
typedef enum {
    SESSION_IO_PROGRESS,
    SESSION_IO_WOULD_BLOCK,
    SESSION_IO_SOURCE_EOF,
    SESSION_IO_DEST_CLOSED,
    SESSION_IO_VPN_LOST,
    SESSION_IO_FATAL
} session_io_status_t;
```

内部仍可用 splice。

---

# 53. 不要过度抽象

本项目当前不需要：

```text
generic transport interface
virtual io backend
function pointer vtable
```

如果只支持 Linux/Android：

session内部直接 splice是完全可以接受的。

---

# 54. SIGPIPE

`splice()` 到 socket遇到关闭通常会：

```text
EPIPE
```

还要确认 process SIGPIPE policy。

如果未忽略：

某些 write-like operation可能导致 process signal。

Codex应全仓检查：

```text
SIGPIPE
MSG_NOSIGNAL
signal(SIGPIPE, SIG_IGN)
```

---

# 55. 如果 daemon未全局忽略 SIGPIPE

要确认 Linux `splice(pipe, socket)` 在目标场景是否可能产生 SIGPIPE。

不要只依赖 errno handling。

推荐 daemon startup明确：

```text
ignore/block SIGPIPE
```

如果其他代码也有 socket writes。

---

# 56. Error logging频率

在高流量路径：

```text
EPIPE
ECONNRESET
```

很可能是普通 peer关闭。

不应每次都 ERROR。

建议：

```text
normal disconnect → DEBUG
unexpected invariant/system error → ERROR
```

避免日志风暴。

---

# 57. `EINVAL` 不总等于“不支持 splice”

可能来自：

```text
bad flag
bad descriptor combination
invalid offset/use
```

所以 telemetry最好记录：

```text
fd types
errno
operation direction
```

不要一概写：

```text
splice not supported
```

---

# 58. ENOSYS

目标现代 Android/Linux基本不应出现。

若出现：

```text
platform unsupported
```

可以作为 capability/fatal platform error。

不需要反复 per-session探测。

---

# 59. ESPIPE

代表 descriptor不适合指定 offset/seek语义等。

当前使用 offset NULL。

如果出现：

应该记录 FD type。

---

# 60. 还有一些 errno应该考虑

例如：

```text
EBADF
ENOMEM
ENFILE/EMFILE（pipe create）
```

默认 ERROR即可。

不需要为每个 errno建立复杂枚举。

---

# 61. Pipe init失败时 state安全

`atpd_splice_state_init()` 先：

```text
fds = -1
initialized=false
```

再 pipe2。

失败后 state仍可 cleanup。

这一点正确。

---

# 62. Cleanup幂等性

当前：

```text
fd >= 0 → close → -1
```

是幂等的。

保留。

---

# 63. 但是 cleanup与正在执行 pump不能并发

如果：

```text
另一个线程 cleanup pipe
```

而 reactor callback正在 splice，

就会发生：

```text
FD reuse race
```

和 session review一样。

必须明确：

```text
datapath FD ownership = reactor thread
```

---

# 64. 如果 standalone module保留

header要写：

```text
splice_state_t is not thread-safe.
All pump/cleanup operations must be serialized by owner.
```

不要因为 fields不是 atomic就含糊。

---

# 65. session.c 当前原子字段并不等于 datapath可多线程调用

同样应该：

```text
reactor thread only
```

原子只用于特定 lifecycle/observer，不能支持并发 splice。

---

# 66. Work loop可能形成 busy loop吗

standalone pump：

```text
while drain
one input splice
while output
return
```

有 byte cap。

总体不会无界。

session pump：

```text
while remaining > 0
```

由 max_len控制。

也合理。

---

# 67. `max_len` 必须永远有合理上限

session caller目前：

```text
ATPD_SESSION_PIPE_SIZE
```

所以 session路径更公平。

这一点实际上优于 standalone 4 MiB默认。

---

# 68. 推荐保持 per-callback bounded work

即使 throughput略降：

控制 daemon更重要的是：

```text
reactor fairness
```

---

# 69. Test：EINTR input

注入：

```text
fd_in → pipe returns EINTR
```

预期：

```text
retry
not fatal
```

---

# 70. Test：EINTR output

同样：

```text
pipe → fd_out EINTR
```

retry。

---

# 71. Test：input NOTSUP before consuming

```text
first splice(fd_in→pipe) = EINVAL
```

预期：

```text
NOTSUP
pipe_pending=0
fallback safe
```

---

# 72. Test：output NOTSUP after input consumed

模拟：

```text
input splice N bytes success
output splice EINVAL
```

必须证明：

```text
不会返回一个让 caller直接 read(fd_in) fallback 的状态
```

选择：

```text
fatal
or
drain via fallback
```

但不能丢数据。

---

# 73. Test：EAGAIN after partial write

例如：

```text
pipe has 64K
output accepts 8K
then EAGAIN
```

验证：

```text
bytes_out += 8K
pipe_pending = 56K
```

且 session：

```text
EPOLLOUT enabled
```

---

# 74. Test：EPOLLOUT registration failure

当 pending存在：

```text
reactor_modify_fd fail
```

session必须：

```text
close/fail session
```

不能留 PIPE_DIRTY zombie session。

---

# 75. Test：destination closes with pending data

```text
pipe_pending > 0
EPIPE
```

验证：

```text
terminal reason准确
pending accounting可观测
session最终关闭
```

---

# 76. Test：source EOF

```text
fd_in EOF
```

验证：

```text
任何已pending数据先处理
然后方向终止
```

如果 session支持 half-close：

验证对应 shutdown。

---

# 77. Test：1-byte fragmentation

大量：

```text
1 byte
```

写入。

确认：

```text
ordering
no loss
no duplicate
```

---

# 78. Test：large stream checksum

例如：

```text
1 GiB deterministic stream
```

最终：

```text
SHA-256 source == destination
```

同时施加：

```text
output throttling
EAGAIN
```

这是最重要的数据正确性测试之一。

---

# 79. Test：randomized backpressure

随机：

```text
writer pause
reader pause
socket close
signals/EINTR
```

运行数万轮。

验证：

```text
no loss
no duplication
no hang
```

---

# 80. Test：peer reset

随机：

```text
ECONNRESET
EPIPE
```

不能：

```text
daemon crash
SIGPIPE terminate
```

---

# 81. Test：pipe size permission failure

mock：

```text
F_SETPIPE_SZ EPERM
```

仍正常 forwarding。

---

# 82. Test：F_GETPIPE_SZ failure

确认代码不会：

```text
把假的 capacity当成真实 invariant
```

---

# 83. Test：fairness

同时：

```text
1 heavy flow
100 small flows
status queries
Netlink events
```

观察：

```text
status p99
timer jitter
throughput
```

决定 per-event byte cap。

---

# 84. Test：10k session cycles

配合 session方案：

```text
create
backpressure
close
GC
```

检查：

```text
pipe FD baseline
RSS stable
0 stuck PIPE_DIRTY
```

---

# 85. Test：VPN drop while pipe dirty

当前 session已有：

```text
emergency_drain
```

重点测试：

```text
pending > 0
VPN becomes not ready
```

确认：

```text
session closes deterministically
pending discard被统计/明确
no reuse race
```

---

# 86. “emergency drain”实际是 discard

正如 session方案：

```text
close pipe
→ kernel drops pending data
```

所以名字不要暗示：

```text
把数据发送出去
```

内部建议改：

```text
discard_pending_and_close
```

或类似。

---

# 87. Telemetry建议

如果 datapath保留：

```text
bytes_forwarded
would_block_count
source_eof
dest_closed
splice_errors
pending_discarded_bytes
not_supported_count
```

但不必全塞 global context。

属于：

```text
session aggregate stats
```

---

# 88. 不要每 connection保留过多 counters

ATPD目标低内存。

真正需要：

```text
bytes
pending
last active
state
```

即可。

全局 error分类可以 aggregate。

---

# 89. Legacy test应该删除或改 production API

如果：

```text
tests只覆盖 atpd_bridge_splice()
```

而 production走：

```text
session_splice_pump()
```

那测试价值很低。

应把测试转向真实 production path。

---

# 90. Codex必须检查 Makefile

如果 `splice.c`：

```text
编译进 binary
```

但没有 caller，

它可能因为静态链接方式仍增加体积或被链接器GC。

无论体积如何：

```text
dead source
```

应删除，降低维护成本。

---

# 91. 推荐 Commit 1

```text
splice: audit datapath callsites and select one owner
```

先生成调用图。

不要一开始改 behavior。

---

# 92. 如果 production path只有 session

Commit 2：

```text
splice: remove obsolete standalone splice engine
```

删除：

```text
src/splice.c
legacy APIs
standalone state
pipe helpers
```

并把必要 result code迁到 session。

---

# 93. 然后 Commit 3

```text
session: harden splice error and backpressure semantics
```

内容：

- EINTR
- output EAGAIN
- reactor modify failure
- terminal reasons
- pending invariant

---

# 94. 如果 standalone splice仍有真实 caller

则不要删。

Commit 2改为：

```text
splice: make stateful pump the single implementation
```

然后：

```text
session.c
```

调用 low-level pump，

删除 session内部重复 splice loops。

---

# 95. 但这个重构需要小心

因为 session需要：

```text
VPN mid-pump check
reactor WRITE event
state transition
```

如果 standalone API不支持这些 semantics：

不要为了“复用”而强行抽象复杂 callback。

这种情况下：

> 反而应该让 session保留实现，并删除 standalone。

---

# 96. 判断标准

问：

```text
除 session 外，是否还有第二个真实 production consumer？
```

如果：

```text
NO
```

答案几乎一定是：

```text
delete splice.c
```

---

# 97. 不要因为“zero-copy模块看起来专业”而保留

当前 `splice.c` 顶部甚至写：

```text
Production Ready - Release Approved
```

这种注释不应该替代：

```text
tests
ownership
single implementation
```

建议删除此类自我认证注释。

---

# 98. Release readiness必须来自测试

真正依据应该是：

```text
checksum stress
random backpressure
peer reset
VPN drop
FD leak
session lifecycle
24h soak
```

不是源码注释。

---

# 99. 与 session方案联动的关键 invariant

```text
S1:
pipe_pending > 0
→ WRITE interest must be armed

S2:
pending data is always drained before new input is consumed

S3:
no terminal state may transition back to ACTIVE/PIPE_DIRTY

S4:
pipe FDs are closed by one lifecycle owner

S5:
pending bytes are never silently lost when switching I/O modes

S6:
every successful input-to-pipe byte is either:
    delivered to fd_out
    or explicitly counted as discarded on terminal close
```

---

# 100. 与 reactor方案联动

因为使用 EPOLLET：

每个 callback要：

```text
drain until EAGAIN
```

或者明确：

```text
为什么 work budget停止后还会获得下一次 event
```

如果由于 byte budget主动提前停止但 FD仍 readable：

ET 模式下可能不会再出现新的 edge。

---

# 101. 这是必须仔细检查的一点

standalone实现：

```text
4 MiB cap
```

session实现：

```text
ATPD_SESSION_PIPE_SIZE cap
```

如果在 EPOLLET callback里：

```text
因为自定义 byte cap而停止
```

但 socket仍然持续 readable，

可能：

```text
没有下一次 EPOLLIN edge
```

导致 stall。

---

# 102. EPOLLET + fairness cap不能简单靠“少读一点”

正确方案通常是：

```text
drain to EAGAIN
```

或者：

```text
如果因为 fairness budget主动停止
→ 主动 schedule continuation
```

例如 reactor post/timer/deferred work。

---

# 103. 当前源码宣称 EPOLLET兼容，但必须用测试证明

尤其测试：

```text
input一次塞入 > per-event cap
之后不再产生新写事件
```

ATPD是否最终能全部读完？

如果不能：

这是 P0 datapath stall。

---

# 104. 这一点优先级非常高

Codex必须建立测试：

```text
single edge
large already-buffered input
per-event limit smaller than input
```

验证：

```text
全部数据最终forward
```

如果当前做不到：

要么：

```text
drain to EAGAIN
```

要么：

```text
deferred continuation
```

---

# 105. 对 control-plane fairness更推荐 continuation

例如：

```text
pump up to budget
↓
仍可能有 input ready
↓
reactor_schedule_immediate(session_continue)
```

这样：

```text
不会依赖新 EPOLL edge
```

又不会一个 callback独占4 MiB。

---

# 106. 不要用 busy loop continuation

continuation应该进入：

```text
reactor下一轮
```

让其他 ready callbacks先运行。

不是：

```text
while has_budget batches
```

立刻循环。

---

# 107. Test：single-edge > budget

例如：

```text
preload 8 MiB
event budget 256 KiB
EPOLLET
```

之后 producer停止。

预期：

```text
8 MiB全部最终到达
```

这是上线前必须有的 test。

---

# 108. Test：pipe dirty + no new input edge

output先阻塞：

```text
pending
```

后来 only output变 writable。

预期：

```text
EPOLLOUT callback drain
```

完成后如果 input早已还有数据：

必须继续 pump或 schedule continuation，

不能等一个不会来的 EPOLLIN edge。

---

# 109. 这正是 session/EPOLLET设计最危险的边界

所以 splice模块不能只看 syscall正确性。

真正 correctness是：

```text
kernel readiness edge
+
userspace pipe state
+
reactor interest
+
continuation
```

四者一致。

---

# 110. 最终建议的 implementation direction

基于当前结构，优先推荐：

```text
session.c owns production datapath
```

原因：

```text
它已经拥有 reactor
state machine
VPN lifecycle
pipe_pending
GC
FD ownership
```

因此：

```text
splice.c
```

更可能应该删除，而不是继续独立演化。

---

# 111. 最终 Invariants

Codex最终必须保证：

```text
I1:
ATPD has exactly one production splice pump implementation

I2:
EINTR never becomes a false fatal I/O error

I3:
NOTSUP is never returned after consuming input unless buffered bytes are safely preserved/drained

I4:
bytes_in == bytes_out + pipe_pending + explicitly_discarded_terminal_bytes
(as appropriate to the selected accounting model)

I5:
pipe_pending > 0 always has a viable drain continuation

I6:
EPOLLET forwarding never relies on a future edge after voluntarily stopping before EAGAIN

I7:
peer close cannot terminate the ATPD daemon via SIGPIPE

I8:
pipe/session cleanup is serialized with datapath execution

I9:
legacy stateless splice cannot remain in production paths

I10:
all data-forwarding stress tests verify byte-for-byte integrity
```

---

# 112. 最终验收

## Call graph

证明：

```text
only one authoritative production pump
```

## Integrity

```text
large randomized stream
SHA-256 equal
```

## Backpressure

```text
partial write
EAGAIN
EPOLLOUT
resume
```

无 loss。

## EINTR

input/output：

```text
retry
```

## EPOLLET

```text
single edge + input > budget
→ no stall
```

## NOTSUP

```text
no data lost during fallback/error
```

## Lifecycle

```text
10k sessions
0 leaked pipe FD
0 stuck PIPE_DIRTY
```

## Fairness

高流量下：

```text
status/timers remain responsive
```

---

# 113. 最终结论

`splice.c` 现在最重要的问题不是代码写得差。

相反，它单独看已经有相当成熟的：

```text
pipe_pending
backpressure
O_NONBLOCK
O_CLOEXEC
per-event accounting
```

真正的问题是：

> ATPD 已经在 `session.c` 中重新实现了另一套 production splice state machine。

两套同时存在，会让 datapath correctness逐渐分叉。

因此 Codex 应先做调用图审计：

```text
如果 production只有 session consumer
→ 删除 standalone splice模块

如果确实有多个 production consumer
→ 选 splice.c作为唯一 low-level engine，并删除 session中的重复 pump
```

结合当前架构，更推荐第一种：

> 让 `session.c` 成为唯一 datapath owner，`splice.c` 在确认无独立 caller 后删除。

无论最终选哪种，都必须优先解决：

```text
EINTR
NOTSUP after input consumed
EPOLLET fairness-cap stall
pending → EPOLLOUT invariant
terminal peer-close semantics
byte-for-byte integrity tests
```

这些比继续增加更多“zero-copy helper”更重要。
