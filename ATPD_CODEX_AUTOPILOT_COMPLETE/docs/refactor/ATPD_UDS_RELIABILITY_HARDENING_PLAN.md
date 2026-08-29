# ATPD `uds.c` 控制接口可靠性加固方案

## 1. 目标

当前 `uds.c` 整体结构合理，不需要拆文件。

继续保持：

```text
uds.c
uds.h
```

本次重点不是重写 UDS，而是把当前“短连接命令处理”完善为可靠的非阻塞 Unix Stream 控制接口。

核心目标：

- 修复 accept 后 reactor 注册失败导致的 FD leak
- 正确处理 Unix `SOCK_STREAM` 分包
- 增加 client 生命周期状态
- 防止 idle client 耗尽 FD
- 正确处理 partial write / `EAGAIN`
- 限制并发 client 数量
- 安全处理已有 socket path
- 区分 active socket 与 stale socket
- control socket 权限失败时 fail closed
- 提前执行 `SO_PEERCRED`
- 保持 UDS callback 非阻塞
- 增加连接级 telemetry 和测试

---

# 2. 本次不做的事情

不要：

- 拆成大量 UDS 子模块
- 改成 TCP
- 改成 HTTP
- 引入复杂 RPC 框架
- 支持长期 persistent session
- 一个连接连续执行大量命令
- 在 UDS 层重新设计 ATPD 整体控制 API

继续保持：

```text
AF_UNIX
SOCK_STREAM
SOCK_NONBLOCK
SOCK_CLOEXEC
epoll/reactor
SO_PEERCRED
```

协议继续保持简单：

> 一个连接发送一条命令，接收一个响应，然后关闭。

---

# 3. P0：修复 accept FD leak

当前风险：

```text
accept4()
   ↓
client_fd 创建成功
   ↓
reactor_add_fd() 失败
   ↓
client_fd 未关闭
```

必须修改为：

```c
if (reactor_add_fd(
        r,
        client_fd,
        REACTOR_EVENT_READ | REACTOR_EVENT_EDGE,
        uds_client_cb,
        client) != 0) {

    close(client_fd);
    free(client);
    continue;
}
```

具体 cleanup 根据最终 client ownership API 调整。

核心规则：

> `reactor_add_fd()` 失败后，UDS 仍然拥有 client FD，必须负责关闭。

---

# 4. 引入 `uds_client_t`

当前 callback 基本依赖裸 `fd`。

建议增加：

```c
typedef enum {
    UDS_CLIENT_READING,
    UDS_CLIENT_WRITING,
    UDS_CLIENT_CLOSING
} uds_client_state_t;

typedef struct uds_client {
    int fd;

    uds_client_state_t state;

    char input[UDS_BUFFER_SIZE];
    size_t input_len;

    char *output;
    size_t output_len;
    size_t output_off;

    uint64_t accepted_at_ms;
    uint64_t last_activity_ms;

    bool authenticated;
} uds_client_t;
```

如果 response 可以固定最大值，也可以避免动态 output。

但考虑：

```text
status
future status --json
diagnose
```

推荐 output 支持动态长度，同时设置硬上限。

---

# 5. Client ownership

必须明确：

```text
accept4
   ↓
UDS owns fd
   ↓
uds_client_t created
   ↓
reactor registration success
   ↓
client lifecycle managed by UDS/reactor
```

最终关闭统一走：

```c
uds_client_close(...)
```

不要在多个 callback 分支中：

```c
reactor_remove_fd(...)
close(fd)
free(client)
```

重复散落。

---

# 6. 增加统一 `uds_client_close()`

建议：

```c
static void uds_client_close(
    uds_server_t *server,
    uds_client_t *client
);
```

职责：

```text
remove reactor registration
close fd
cancel idle timer（若使用 per-client timer）
free output
free client
decrement active_clients
update stats
```

要求：

```text
single ownership
single cleanup path
```

---

# 7. Unix Stream 必须处理 fragmentation

Unix：

```text
SOCK_STREAM
```

不提供 message boundary。

客户端发送：

```text
status\n
```

server 可能收到：

```text
"sta"
```

然后下一次 readable：

```text
"tus\n"
```

因此：

> 不能把一次 read cycle 当成一条完整 command。

---

# 8. Input buffering

每个 client 保存：

```c
char input[UDS_BUFFER_SIZE];
size_t input_len;
```

READ callback：

```text
read
 ↓
append input buffer
 ↓
search '\n'
```

只有找到：

```text
newline
```

才执行 command。

---

# 9. Command framing

明确协议：

```text
COMMAND "\n"
```

例如：

```text
status\n
ping\n
version\n
stop\n
```

server：

```text
收到 newline
→ command complete
→ strip CR/LF
→ dispatch
```

兼容：

```text
\n
\r\n
```

---

# 10. 最大请求长度

保持：

```text
UDS_BUFFER_SIZE ≈ 4096
```

如果：

```text
input_len >= limit
```

仍然没有 newline：

返回：

```text
ERR request too large
```

然后关闭连接。

禁止：

```text
无限动态扩展 input
```

控制协议不需要。

---

# 11. 一连接一命令

推荐明确：

```text
one connection
=
one command
```

即使 input 中出现：

```text
status\nping\n
```

也只执行第一条，或者直接拒绝 trailing data。

不要把 UDS 做成交互 shell。

这样：

- 状态机简单
- ownership 简单
- timeout 简单
- 攻击面更小
- CLI 行为明确

---

# 12. P1：增加并发 client 上限

建议默认：

```c
#define UDS_MAX_CLIENTS 32
```

或：

```text
64
```

对于 ATPD CLI 足够。

server 保存：

```c
size_t active_clients;
size_t max_clients;
```

accept 时：

```text
active_clients >= max_clients
→ close/reject
```

不要继续注册 reactor。

---

# 13. Client limit 的目的

防止：

```text
connect
connect
connect
...
不发送数据
```

耗尽：

```text
ATPD FD
reactor handler
memory
```

虽然 UDS 权限较严格，这仍然是 root daemon 应有的资源边界。

---

# 14. P1：Idle timeout

推荐：

```text
2–5 秒
```

默认可取：

```text
5000 ms
```

CLI command 不应该需要数十秒才能发完。

client 保存：

```c
last_activity_ms
```

每次成功：

```text
read
write
```

更新。

---

# 15. Idle timeout 实现

不要为了每个 client 都创建复杂 timer，如果 reactor timer 数量管理不方便。

可选择：

### 方案 A

每 client 一个 timer。

优点：

```text
简单直接
```

缺点：

```text
client 多时 timer 多
```

### 方案 B（推荐）

server 一个周期性 cleanup timer：

```text
1 秒
```

扫描最多：

```text
32/64 clients
```

检查：

```text
now - last_activity > timeout
```

然后 close。

ATPD client 数量很小，这种方案简单可靠。

---

# 16. Server context

如果当前没有 server context，建议：

```c
typedef struct uds_server {
    reactor_t *reactor;

    int listen_fd;

    uds_client_t *clients;

    size_t active_clients;
    size_t max_clients;

    uint64_t idle_timeout_ms;

    reactor_timer_t *cleanup_timer;

    uds_stats_t stats;
} uds_server_t;
```

不建议继续增加大量 UDS global variable。

---

# 17. P1：Partial write

当前：

```text
send
→ EAGAIN
→ response failed
→ close
```

可能截断 response。

必须支持：

```text
output_len
output_off
```

---

# 18. Response state

command 完成：

```text
READING
 ↓
process command
 ↓
create response
 ↓
WRITING
```

设置：

```c
client->output = response;
client->output_len = len;
client->output_off = 0;
```

---

# 19. Write loop

```text
send(output + output_off)
```

成功：

```text
output_off += n
```

如果：

```text
output_off == output_len
```

则：

```text
close client
```

如果：

```text
EAGAIN / EWOULDBLOCK
```

则：

```text
enable EPOLLOUT
return
```

下次 writable 继续。

---

# 20. Reactor event mask

READING：

```text
EPOLLIN
```

WRITING：

```text
EPOLLOUT
```

必要时保留：

```text
ERR/HUP
```

通过 reactor 的通用事件语义处理。

不要 busy retry send。

---

# 21. Response 最大长度

即使 output 动态分配，也建议设置：

```text
UDS_MAX_RESPONSE_SIZE
```

例如：

```text
64 KiB
```

具体数值根据 status/diagnose 未来需求确定。

目的：

```text
避免异常 status renderer
生成无限 response
```

超过：

```text
return controlled error
```

---

# 22. P1：不要 blind `unlink(path)`

ATPD 是 root daemon。

禁止：

```c
unlink(path);
```

不检查 path 当前是什么。

初始化前：

```c
lstat(path, &st);
```

---

# 23. Path 分类

### 不存在

```text
ENOENT
→ normal
```

### 存在且不是 socket

```text
FAIL
```

并明确日志：

```text
refusing to remove non-socket path
```

### 存在且是 socket

继续判断：

```text
active
or
stale
```

---

# 24. Active socket 检测

如果 path 已存在且是 socket：

创建临时 Unix socket：

```text
connect(path)
```

如果成功：

```text
已有服务正在监听
→ uds_init fail
```

不要删除 pathname。

这样避免第二个 ATPD：

```text
unlink 第一个 ATPD 的 socket
```

---

# 25. Stale socket

如果：

```text
connect → ECONNREFUSED
```

并确认 path 是 socket：

可认为可能 stale。

然后：

```text
unlink stale socket
bind
```

其他异常不要随意 unlink。

例如：

```text
EACCES
```

不等于 stale。

---

# 26. Race 注意

检查：

```text
lstat
connect
unlink
bind
```

之间存在 TOCTOU。

对于本地 root daemon runtime directory，风险通常可控。

仍应：

- runtime dir 权限严格
- 不跟随 symlink
- `lstat`
- 非 socket fail
- bind 后再次确认权限

不要为了完全消除 TOCTOU 引入复杂不兼容方案。

---

# 27. Runtime directory

确保 UDS parent directory：

```text
root-owned
not world-writable
```

如果是 ATPD 自己创建：

明确：

```text
mode
owner
```

不要只保护 socket 文件而忽略目录。

---

# 28. P1：chmod 失败 fail closed

当前 control socket 具有：

```text
status
stop
...
```

权限属于安全边界。

推荐：

```c
if (chmod(path, 0600) < 0) {
    cleanup;
    return -1;
}
```

不要：

```text
WARN + continue
```

除非 Android 特定环境有明确兼容性理由。

---

# 29. SO_PEERCRED 提前

当前 peer credential 检查建议移动到：

```text
accept
 ↓
SO_PEERCRED
 ↓
authorized?
 ↓
yes → allocate/register client
no  → close immediately
```

而不是：

```text
register reactor
 ↓
等 client readable
 ↓
再认证
```

---

# 30. Peer policy

继续保持至少：

```text
uid == 0
or
uid == daemon expected uid
```

不要仅依赖：

```text
socket 0600
```

`0600 + SO_PEERCRED` 是合理的双重边界。

---

# 31. Peer credential failure

如果：

```text
getsockopt(SO_PEERCRED) fails
```

默认：

```text
reject
```

即：

> fail closed。

不要因为 credential 无法读取而允许连接。

---

# 32. Accept loop

Edge-triggered listener callback 必须：

```text
accept4 loop
```

直到：

```text
EAGAIN/EWOULDBLOCK
```

并正确处理：

```text
EINTR
EMFILE
ENFILE
ENOMEM
ENOBUFS
```

---

# 33. EMFILE / ENFILE

如果达到 FD limit：

至少：

```text
log rate-limited error
return
```

不要：

```text
tight accept loop
```

可选后续实现：

```text
reserve fd technique
```

但 ATPD 当前未必需要。

---

# 34. Listener registration failure

listener：

```text
socket
bind
listen
reactor_add_fd
```

任何一步失败：

必须完整 cleanup：

```text
close listen_fd
unlink socket path（仅自己成功 bind 的情况下）
free state
```

这部分与 reactor transactional init 原则一致。

---

# 35. Client callback 错误处理

统一：

```text
EPOLLERR
EPOLLHUP
EPOLLRDHUP
fatal read error
fatal write error
protocol error
idle timeout
```

最终进入：

```c
uds_client_close(...)
```

不要多个 cleanup 分支。

---

# 36. Read loop

对于 ET：

```text
read until EAGAIN
```

必须保留。

处理：

```text
n > 0
→ append

n == 0
→ peer EOF

errno == EINTR
→ retry

EAGAIN
→ return

other
→ close
```

---

# 37. EOF 语义

如果 peer：

```text
write("status\n")
shutdown(SHUT_WR)
```

server 已经拿到完整命令：

应允许继续：

```text
process
write response
```

不要看到 EOF 就无条件丢弃已完成 command。

---

# 38. Command normalization

建议仅做：

```text
strip trailing \n
strip optional \r
```

不要：

```text
arbitrary whitespace normalization
shell-like tokenization
```

现有简单命令协议保持精确。

---

# 39. Command dispatch

现有命令可以继续：

```text
status
stop
ping
sessions
version
stats
help
```

继续使用精确匹配。

不要：

```text
system()
popen()
shell command interpolation
```

---

# 40. `stop` command 架构清理

当前 UDS 层直接操作：

```text
runtime state
session drain
global running
reactor_stop
```

建议后续提供统一：

```c
int atpd_request_shutdown(
    atpd_context_t *ctx,
    atpd_shutdown_reason_t reason
);
```

UDS：

```text
parse "stop"
 ↓
send response / mark shutdown
 ↓
request_shutdown()
```

UDS 不应该知道完整 shutdown implementation。

这项列 P2，可在核心可靠性修复之后做。

---

# 41. Stop response 顺序

注意：

```text
收到 stop
```

不要先：

```text
reactor_stop
```

导致客户端收不到确认响应。

推荐语义：

```text
prepare "OK stopping"
 ↓
flush response
 ↓
request shutdown
```

或者设置：

```text
shutdown_after_write
```

response 写完后触发 shutdown。

---

# 42. Status callback 必须保持快速

`handle_status()` 最终应：

```text
read status snapshot
 ↓
render
 ↓
response
```

禁止在 UDS callback 内：

```text
同步 Native API retry
长时间 poll
sleep
blocking network operation
```

这与已有 `status.c` observability 重构计划配套。

---

# 43. UDS telemetry

建议增加：

```c
typedef struct {
    uint64_t accepted;
    uint64_t completed;

    uint64_t rejected_peer;
    uint64_t rejected_limit;

    uint64_t idle_timeouts;
    uint64_t protocol_errors;

    uint64_t read_errors;
    uint64_t write_errors;

    uint64_t bytes_read;
    uint64_t bytes_written;

    size_t active_clients;
    size_t peak_clients;
} uds_stats_t;
```

---

# 44. Stats 不应造成副作用

读取：

```text
uds stats
```

必须：

```text
read-only
non-blocking
```

不要为了统计再扫描 `/proc`。

---

# 45. 日志

不要为每个正常 CLI connection 打高等级日志。

建议：

```text
DEBUG:
accept/close/command

WARN:
unauthorized
protocol error
client limit
idle timeout（可 rate limit）

ERROR:
listener failure
permission failure
reactor registration failure
```

---

# 46. 防日志洪泛

以下事件：

```text
unauthorized client
client limit reached
EMFILE
protocol error
```

应考虑 rate limit。

避免本地异常进程：

```text
制造连接
→ ATPD log 无限增长
```

---

# 47. 测试：accept registration failure

故障注入：

```text
accept4 succeeds
reactor_add_fd fails
```

验证：

```text
client fd closed
active_clients unchanged
no heap leak
```

---

# 48. 测试：fragmented command

客户端分段：

```text
"sta"
sleep
"tus\n"
```

必须：

```text
正确执行 status
```

不能返回：

```text
Unknown command: sta
```

---

# 49. 测试：byte-by-byte command

最严格：

```text
s
t
a
t
u
s
\n
```

每个 byte 单独 write。

结果仍应正确。

---

# 50. 测试：oversized request

发送：

```text
> UDS_BUFFER_SIZE
```

且无 newline。

验证：

```text
controlled error
connection closed
no overflow
no memory growth
```

---

# 51. 测试：partial write

缩小 socket send buffer或让 client 暂停读取。

制造：

```text
send → EAGAIN
```

验证：

```text
server 注册 EPOLLOUT
response 最终完整
无 busy loop
```

---

# 52. 测试：slow reader

客户端：

```text
send status
非常慢地读取 response
```

验证：

```text
其他 ATPD reactor events 仍可处理
```

---

# 53. 测试：idle client

```text
connect
不发送
```

超过 timeout：

```text
server 自动 close
active_clients 回落
```

---

# 54. 测试：client exhaustion

建立：

```text
UDS_MAX_CLIENTS
```

个 idle client。

再建立一个：

```text
必须被拒绝
```

释放旧 client 后：

```text
新 client 可正常连接
```

---

# 55. 测试：unauthorized peer

在 host test 能模拟时：

验证：

```text
非允许 UID
→ accept 后立即 reject
→ 不注册 reactor
```

Android 环境可补 integration test。

---

# 56. 测试：non-socket path

预先创建：

```text
regular file
```

到 UDS path。

启动 ATPD：

```text
必须失败
```

并验证：

```text
原文件仍存在
内容未改变
```

这是 root safety 的重要测试。

---

# 57. 测试：active socket

先启动：

```text
server A
```

再启动：

```text
server B
```

B：

```text
必须失败
```

同时：

```text
A socket path 仍然存在
A 仍然可接受连接
```

---

# 58. 测试：stale socket

创建 stale Unix socket path。

确认没有 listener。

ATPD：

```text
识别 stale
unlink
bind success
```

---

# 59. 测试：chmod failure

故障注入：

```text
chmod fails
```

验证：

```text
uds_init fails
listener closed
socket path cleanup
```

---

# 60. 测试：disconnect during response

客户端：

```text
send command
close immediately
```

server：

```text
处理 EPIPE/ECONNRESET
不 crash
不泄漏
```

SIGPIPE 必须不会杀死 daemon。

---

# 61. 测试：FD churn

循环：

```text
connect
ping
disconnect
```

例如：

```text
10,000 次
```

检查：

```text
FD 回到 baseline
RSS 无持续增长
active_clients == 0
```

---

# 62. 测试：parallel clients

例如：

```text
16
32
64
```

并发：

```text
ping
status
version
```

验证：

```text
无错误响应串线
无 crash
无 FD leak
reactor responsive
```

---

# 63. 测试：stop command

验证：

```text
client 收到完整 stop acknowledgement
```

然后：

```text
ATPD 正常 shutdown
socket path 被清理
所有 client fd 被关闭
```

---

# 64. Sanitizer

Host 测试建议：

```text
ASan
UBSan
```

重点检查：

```text
uds_client_t UAF
output double free
callback/remove interaction
oversized input
shutdown cleanup
```

---

# 65. 第一阶段提交

## Commit 1

```text
uds: fix accepted client fd ownership
```

只做：

- check `reactor_add_fd`
- failure close
- 对应 fault test

这是最小 P0 修复。

---

# 66. 第二阶段提交

## Commit 2

```text
uds: add explicit client state
```

增加：

```text
uds_client_t
input buffering
central close path
active_clients
```

先解决：

```text
fragmented request
ownership
```

---

# 67. 第三阶段提交

## Commit 3

```text
uds: support asynchronous response writes
```

增加：

```text
output buffer
output offset
EPOLLOUT
partial write
```

---

# 68. 第四阶段提交

## Commit 4

```text
uds: enforce client limits and idle timeout
```

增加：

```text
max_clients
idle cleanup
stats
```

---

# 69. 第五阶段提交

## Commit 5

```text
uds: harden control socket creation
```

增加：

```text
lstat
non-socket refusal
active socket detection
stale socket handling
chmod fail closed
directory checks
```

---

# 70. 第六阶段提交

## Commit 6

```text
uds: move peer authentication to accept path
```

增加：

```text
SO_PEERCRED immediately after accept
fail closed
rejection telemetry
```

---

# 71. 第七阶段提交

## Commit 7

```text
uds: route stop through runtime shutdown API
```

这是架构清理。

如果统一 runtime shutdown API 尚未准备好：

```text
可以延期
```

不要为了这项阻塞 UDS 核心可靠性修复。

---

# 72. Codex 修改前先输出当前函数归属/职责

先扫描当前：

```text
uds.c
uds.h
```

列出：

```text
函数
职责
FD ownership
userdata ownership
是否 callback
是否可能 blocking
错误路径
```

尤其检查：

```text
uds_init
uds_cleanup
uds_accept_cb
uds_client_cb
send_response_all
process_command
handle_status
handle_stop
```

---

# 73. Codex 必须搜索

```text
accept
accept4
reactor_add_fd
reactor_modify_fd
reactor_remove_fd
close
read
recv
write
send
EAGAIN
EWOULDBLOCK
EINTR
SO_PEERCRED
unlink
lstat
bind
listen
chmod
open_memstream
status_show
reactor_stop
g_running
```

---

# 74. 与 reactor 加固方案的依赖

UDS 加固依赖 reactor API contract：

```text
reactor_add_fd failure ownership
reactor_modify_fd failure semantics
callback self-remove
timer ownership
```

因此推荐实施顺序：

```text
reactor 基础 P0/P1 修复
        ↓
UDS client state
        ↓
UDS async write / idle timeout
```

不要求等 reactor 所有增强全部完成。

---

# 75. 与 status 重构的关系

当前：

```text
UDS status
→ status_show()
```

如果 status 内部 blocking：

```text
UDS callback
→ reactor blocked
```

最终应：

```text
runtime telemetry
        ↓
status snapshot
        ↓
UDS renderer
```

UDS 不负责解决 Native API blocking，但必须确保自己不新增 blocking。

---

# 76. 最终验收标准

### FD

```text
accept/register failure → 0 leak
10,000 connect/disconnect → FD 回 baseline
```

### Protocol

```text
fragmented command 正确
oversized command 安全拒绝
一连接一命令
```

### Write

```text
partial write 不截断
EAGAIN 不 busy loop
slow reader 不阻塞 reactor
```

### Resource limits

```text
max_clients 生效
idle timeout 生效
active_clients 最终回 0
```

### Socket safety

```text
不删除 non-socket path
不破坏 active ATPD socket
stale socket 可恢复
chmod failure fail closed
```

### Authentication

```text
SO_PEERCRED failure → reject
unauthorized peer → reject
```

### Shutdown

```text
stop acknowledgement 可完整发送
daemon clean shutdown
socket path clean
```

### Memory

```text
ASan/UBSan clean
无持续 RSS 增长
无 client/output UAF
```

---

# 77. 最终目标结构

不需要增加大量文件。

仍然：

```text
uds.c
uds.h
```

但内部结构变成：

```text
UDS server
   │
   ├── listener
   │
   ├── client limit
   │
   ├── idle cleanup
   │
   └── stats
   │
   ▼
uds_client_t
   │
   ├── READING
   │      ↓
   │   command framing
   │      ↓
   ├── command dispatch
   │      ↓
   ├── WRITING
   │      ↓
   │   partial send
   │
   └── CLOSE
```

---

# 78. 结论

`uds.c` 不需要大规模结构重构。

最重要的改变是：

> 不再把 client 当成“一个 fd + 一次 read”，而是把它当成一个生命周期很短、资源严格受限的异步 stream connection。

通过一个轻量：

```text
uds_client_t
```

可以同时解决：

```text
stream fragmentation
partial write
idle connection
client limit
ownership
telemetry
```

再配合：

```text
safe socket path handling
SO_PEERCRED
fail-closed permissions
```

即可把 UDS 从“CLI 基本能工作”提升为适合长期 root daemon 的可靠本地控制接口。
