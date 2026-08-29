# ATPD `singbox_api.c` Native API 可靠性与状态缓存加固方案

## 1. 模块结论

当前 `singbox_api.c` 约 624 行，已经实现：

- Native API 基础 lifecycle
- TCP health probe
- `SubscribeStatus`
- sing-box memory / goroutine / connections / traffic telemetry
- `GetVersion`
- `GetClashModeStatus`
- `SetClashMode`
- reload bridge
- 手工 gRPC-Web / protobuf framing 和解析

当前模块的主要问题不是“功能缺失”，而是：

> ATPD 现在把 Native API 同时当成同步查询接口、health probe、状态数据源和控制接口使用，但没有统一 transport state、deadline、cache 和 error semantics。

本轮优先级：

```text
P0/P1
1. `singbox_api_reload()` 不应按进程名找 PID 后直接发信号
2. `singbox_api_exec_cli()` 空实现不能返回成功
3. `get_status()` / unary RPC 会阻塞调用线程约 1–2 秒
4. status/health 不应每次重新建立 TCP + gRPC-Web stream
5. `connected` 当前只表示 TCP port 是否可连，不等于 Native API healthy

P1
6. `timeout_sec` 字段基本没有真正成为统一 deadline
7. invalid API host 会静默 fallback 到 127.0.0.1
8. `setsockopt()` 失败被忽略
9. HTTP status / gRPC status 没有完整验证
10. 手工 HTTP chunk / gRPC-Web parser 边界和错误语义需要测试
11. API failure 缺少状态机、连续失败计数、last_error、snapshot age

P2
12. 长期建议使用 daemon-maintained cached status
13. 可考虑 persistent SubscribeStatus stream
14. transport/parser 复杂度继续增长时再拆文件
```

---

# 2. 当前架构里最关键的问题：同步 Native API 进入 hot path

`singbox_api_get_status()` 当前流程：

```text
socket()
↓
nonblocking connect
↓
poll(connect, up to 1000 ms)
↓
切回 blocking socket
↓
send HTTP headers
↓
send gRPC-Web request frame
↓
blocking recv
↓
parse first SubscribeStatus frame
↓
close
```

也就是说一次 status sample 都会建立一条新的 TCP connection。

当前 socket read/write timeout：

```text
1 秒
```

connect：

```text
1 秒
```

因此 failure path 可能明显阻塞 caller。

---

# 3. 这和 ATPD single-thread reactor 架构冲突

如果调用发生在 reactor callback：

```text
reactor
↓
singbox_api_get_status()
↓
poll/connect/recv
↓
1–2 秒内其他 ATPD event 无法运行
```

可能影响：

```text
UDS
service supervision
Netlink
session
timers
shutdown
```

所以必须建立硬规则：

> Reactor callback 不得直接执行同步 Native API RPC。

---

# 4. `status` 必须只读 cache

最终目标：

```text
sing-box Native API
        ↓
background/reactor telemetry producer
        ↓
singbox_api_snapshot_t
        ↓
status snapshot
        ↓
human/json renderer
```

`atpd status`：

```text
不得 connect
不得 poll
不得 recv
不得 retry
```

只能：

```text
copy cached snapshot
```

---

# 5. 推荐 Native API snapshot

```c
typedef enum {
    SINGBOX_API_STOPPED,
    SINGBOX_API_CONNECTING,
    SINGBOX_API_HEALTHY,
    SINGBOX_API_DEGRADED,
    SINGBOX_API_RETRYING,
    SINGBOX_API_UNAVAILABLE
} singbox_api_state_t;

typedef struct {
    singbox_api_state_t state;

    bool transport_reachable;
    bool rpc_healthy;

    singbox_status_t status;

    char version[64];

    singbox_clash_mode_status_t clash_mode;

    uint64_t updated_at_ms;
    uint64_t last_success_ms;
    uint64_t last_failure_ms;

    uint64_t successful_queries;
    uint64_t failed_queries;
    uint32_t consecutive_failures;

    int last_errno;
    int last_http_status;
    int last_grpc_status;

    char last_error[128];
} singbox_api_snapshot_t;
```

---

# 6. Snapshot freshness

status 必须显示：

```text
Native API: HEALTHY
Snapshot age: 340 ms
```

或者：

```text
Native API: DEGRADED
Last good sample: 8.2 s ago
```

不要失败后把最后一次正常数据立即清零。

---

# 7. Stale data 策略

推荐：

```text
fresh < 3s
→ normal

3–15s
→ stale / DEGRADED

>15s
→ unavailable
```

具体阈值可以配置或按采样周期调整。

关键原则：

> API 临时失败时保留 last-known-good data，同时明确 freshness。

---

# 8. P0/P1：`singbox_api_reload()` 不应自行按名字杀进程

当前逻辑：

```c
int pid = get_pid_by_name("sing-box");
if (pid > 0)
    return kill(pid, SIGHUP);
```

问题：

```text
它绕过 service supervisor 的 child ownership
```

可能存在：

- 多个 sing-box 实例
- PID reuse
- 找到其他用户/其他服务的 sing-box
- service 当前 child PID 与 name lookup PID 不一致

对于 root daemon：

> destructive signal 必须使用 supervisor 已确认 ownership 的 child identity。

---

# 9. Reload 应由 service 层负责

推荐：

```c
int service_request_reload(service_ctx_t *svc);
```

内部：

```text
确认 owned child
确认 generation/starttime
↓
signal owned child
↓
记录 result
```

`singbox_api.c` 不应该自行寻找进程。

---

# 10. 如果 Native API 已提供 Reload RPC

优先考虑：

```text
Native API typed control
```

如果 sing-box Native API 没有符合 ATPD需求的 reload RPC：

再由：

```text
service supervisor
```

发送 `SIGHUP`。

关键是：

```text
process ownership 属于 service
API transport 属于 singbox_api
```

不要混层。

---

# 11. P0/P1：`singbox_api_exec_cli()` 不能空实现返回 0

当前：

```c
int singbox_api_exec_cli(...) {
    ...
    return 0;
}
```

实际：

```text
没有执行任何操作
```

但 caller 会认为：

```text
success
```

这是典型假成功 API。

---

# 12. 两种正确处理

### 推荐 A

如果已经不需要 CLI bridge：

```text
删除 API
```

同时删除所有调用点。

### B

如果暂时要保留 ABI：

```c
errno = ENOTSUP;
return -1;
```

并在 header 标明：

```text
unsupported
```

绝不能 no-op success。

---

# 13. P1：`connected` 命名/语义错误

当前 health check：

```text
TCP connect 到 API port
→ connected = 1
```

它并没有：

```text
发送 RPC
验证 HTTP
验证 gRPC
验证 StartedService
```

因此：

```text
connected == 1
```

只表示：

> TCP listener reachable。

不等于：

> Native API healthy。

---

# 14. 拆分 transport 与 RPC health

至少：

```c
bool transport_reachable;
bool rpc_healthy;
```

health：

```text
transport_reachable
→ TCP connect成功

rpc_healthy
→ lightweight valid Native API RPC成功
```

status 不应把两者混成一个 `connected`。

---

# 15. Health probe 推荐

不要每次调用：

```text
SubscribeStatus
```

来做最轻量 health。

如果：

```text
GetVersion
```

足够轻且稳定：

可作为 RPC health probe。

或者用真正定义的 health RPC（如果 upstream API 存在并适合）。

---

# 16. `last_check` 使用 wall clock 不够理想

当前：

```text
time(NULL)
```

用于 health timestamp。

runtime age/deadline 更适合：

```text
CLOCK_MONOTONIC
```

wall clock 可因为：

```text
NTP
用户改时间
时区/RTC变化
```

跳变。

建议：

```text
monotonic updated_at_ms
```

用户显示需要 wall time时再单独保存。

---

# 17. `timeout_sec` 当前不是统一真实配置

ctx 包含：

```c
int timeout_sec;
```

init 设置：

```text
2
```

但实际：

```text
connect = 1000 ms
recv/send = 1 s
```

多个地方直接 hardcode。

因此这个字段现在具有误导性。

---

# 18. 建立统一 deadline

推荐：

```c
typedef struct {
    int connect_timeout_ms;
    int rpc_timeout_ms;
} singbox_api_timeouts_t;
```

例如：

```text
connect 300–500 ms
RPC 500–1000 ms
```

本机 loopback 不需要几秒钟才判定失败。

---

# 19. 使用 absolute monotonic deadline

比：

```text
每个 syscall 都重新获得完整 1s timeout
```

更可靠。

正确：

```text
deadline = now + rpc_timeout

connect
send
recv
EINTR
...
```

所有阶段共享同一个 deadline。

这样一次 RPC 最大耗时有硬上界。

---

# 20. P1：invalid host 不要静默改成 loopback

当前：

```c
if (inet_pton(AF_INET, host, &addr) <= 0)
    addr = 127.0.0.1;
```

如果配置：

```text
API_HOST=127.0.0.x typo
```

或者：

```text
localhost
```

解析失败后会静默连接 loopback。

这是配置错误被隐藏。

---

# 21. 正确策略

如果当前明确只支持 IPv4 literal：

```text
inet_pton fail
→ init/config validation fail
```

不要 fallback。

如果要支持 hostname：

使用：

```text
getaddrinfo()
```

并明确 IPv4/IPv6 policy。

---

# 22. 建议 ATPD 本地部署优先固定 transport

如果 sing-box Native API 与 ATPD 总是在同设备：

优先：

```text
127.0.0.1
```

甚至后续：

```text
Unix Domain Socket
```

若 upstream Native API listener 支持。

这样避免：

```text
DNS
external bind
network exposure
```

---

# 23. Secret 处理

当前 secret 会被写入：

```text
HTTP Authorization header
```

这是合理的。

需要保证：

```text
不在 debug/error log 打印 secret
```

config/status 也应：

```text
redact
```

---

# 24. 不允许把 Authorization request 打进日志

未来 transport debug 时：

不要直接：

```text
LOG_DEBUG("%s", req)
```

否则 secret 泄漏。

要求：

```text
headers redacted
```

---

# 25. `setsockopt()` 返回值不能全部忽略

当前：

```c
setsockopt(SO_RCVTIMEO)
setsockopt(SO_SNDTIMEO)
```

返回未检查。

如果 timeout 设置失败：

后面的 blocking `recv()` 可能与预期不同。

---

# 26. 如果继续 blocking implementation

至少：

```text
setsockopt fail
→ close
→ RPC fail
```

因为 deadline 是 correctness boundary。

但更推荐：

```text
保持 nonblocking
poll + deadline
```

不要把 fd切回 blocking。

---

# 27. 不建议 nonblocking connect 后再切 blocking

当前：

```text
SOCK_NONBLOCK
connect
poll
fcntl(clear O_NONBLOCK)
blocking send/recv
```

这会形成两套 I/O model。

推荐统一：

```text
socket 始终 nonblocking
```

然后 helper：

```text
connect_with_deadline
send_all_with_deadline
recv_with_deadline
```

---

# 28. 统一 transport helper

当前 `get_status()` 与 `grpc_web_unary_call()` 都重复：

```text
socket
connect
fcntl
timeout
send
recv
close
```

应该集中。

例如：

```c
static int api_connect(...);
static int api_send_all(...);
static int api_recv(...);
static int api_http_request(...);
```

---

# 29. 先减少复制，不一定立即拆文件

本轮可以在 `singbox_api.c` 内部重构 helper。

如果完成后文件继续增长到：

```text
800–1000 LOC
```

再拆：

```text
singbox_api.c
singbox_api_transport.c
singbox_api_proto.c
singbox_api_internal.h
```

---

# 30. 推荐未来拆分边界

### `singbox_api.c`

负责：

```text
public API
state
snapshot
refresh/retry
control semantics
```

### `singbox_api_transport.c`

负责：

```text
connect
deadline
HTTP/1.1
chunked framing
gRPC-Web frame
```

### `singbox_api_proto.c`

负责：

```text
protobuf varint
Status parse
Version parse
ClashMode encode/decode
```

但不是本轮强制。

---

# 31. HTTP response status 必须验证

当前 parser 主要寻找：

```text
\r\n\r\n
chunked body
gRPC frame
```

但没有建立可靠：

```text
HTTP status code == 200
```

contract。

应该解析 status line：

```text
HTTP/1.1 200
```

否则：

```text
401
404
500
```

必须明确返回不同错误。

---

# 32. 401/403 要区分认证失败

如果 secret 不正确：

```text
HTTP 401/403
```

状态应：

```text
AUTH_FAILED
```

而不是 generic：

```text
Native API unavailable
```

这样用户能直接知道配置问题。

---

# 33. 404/UNIMPLEMENTED 要区分 API version mismatch

如果 path 不存在：

```text
404
```

或者 gRPC：

```text
UNIMPLEMENTED
```

可能意味着：

```text
sing-box 版本/API schema不匹配
```

应该记录：

```text
API_INCOMPATIBLE
```

而不是当普通网络故障不断 retry。

---

# 34. 必须解析 gRPC status

gRPC-Web 成功不能只看：

```text
拿到一个 data frame
```

最终 RPC status 在 trailers 中。

对于 streaming status：

可以在第一帧之后继续保持 stream。

对于 unary：

最好验证：

```text
grpc-status: 0
```

或 trailer frame内 status。

---

# 35. 当前 unary parser 的不足

它收到第一个完整 data frame就：

```text
copy response
close
return success
```

没有完整消费：

```text
trailers
```

因此可能漏掉最终 gRPC error。

需要测试真实 sing-box 行为，然后明确：

```text
是否必须读 trailer
```

推荐 unary 必须验证最终 RPC status。

---

# 36. `SubscribeStatus` streaming 的正确方向

当前：

```text
每次 get_status()
→ 新建 stream
→ 读取第一帧
→ 主动 close
```

能工作，但开销和 failure latency 都不理想。

长期更合理：

```text
一个长期 SubscribeStatus stream
```

sing-box 定期推送：

```text
memory
goroutines
connection
traffic
```

ATPD只更新 cache。

---

# 37. Persistent stream 架构

```text
singbox_api manager
        ↓
CONNECTING
        ↓
send SubscribeStatus
        ↓
STREAMING
        ↓
reactor EPOLLIN
        ↓
decode frames
        ↓
update snapshot
```

断开：

```text
DEGRADED
↓
backoff
↓
reconnect
```

---

# 38. Persistent stream 优点

- `status` 零网络查询
- 不重复 TCP handshake
- 不重复 HTTP headers
- telemetry 天然周期更新
- failure 状态立即可见
- sing-box RSS/goroutine trend 更容易采样
- 减少临时 socket churn

---

# 39. Persistent stream 的实现原则

不要一次性写成复杂通用 gRPC client。

只需要：

```text
one known server-streaming RPC
```

实现：

```text
connect
send fixed request
incremental HTTP/chunk parser
incremental gRPC-Web frame parser
reconnect
```

仍可保持 C 体积较小。

---

# 40. 但是第一阶段可以先做 periodic cached polling

如果 persistent stream 改动过大：

第一阶段：

```text
timer every 1s
→ perform one bounded get_status
→ update cache
```

但注意：

> 不要在 reactor callback 中执行 blocking RPC。

可：

- 把 transport 完全 nonblocking纳入 reactor
- 或专门 worker thread
- 或先保持同步但只在非-reactor控制路径调用

ATPD目前整体偏 single-thread，因此优先 reactor-native async transport。

---

# 41. 不建议为了这个引入通用线程池

当前 ATPD核心价值之一是：

```text
single-thread epoll reactor
```

不要为了一个 API client突然引入：

```text
thread pool
complex locks
```

Native API transport本质上很适合非阻塞 socket状态机。

---

# 42. Native API state machine

推荐：

```text
STOPPED
↓
CONNECTING
↓
SUBSCRIBING
↓
STREAMING
↓
DEGRADED
↓
BACKOFF
↓
CONNECTING
```

另有 terminal/config：

```text
AUTH_FAILED
INCOMPATIBLE
UNAVAILABLE
```

---

# 43. Backoff

不要每个 loop疯狂 reconnect。

建议：

```text
250 ms
500 ms
1 s
2 s
5 s
max 10 s
```

成功收到 frame后 reset。

---

# 44. sing-box service lifecycle联动

当 `service` 明确：

```text
sing-box STOPPED
```

Native API manager：

```text
立即停止 reconnect
state = STOPPED
```

当：

```text
service STARTING/RUNNING
```

再启动 Native API连接。

避免：

```text
sing-box明确没运行
Native API仍每秒 connect失败
```

---

# 45. service 与 API 状态职责

```text
service.c
→ child process lifecycle

singbox_api.c
→ RPC transport + observed telemetry
```

二者不互相夺 ownership。

---

# 46. Startup readiness

service 启动 sing-box后：

不要通过 blocking：

```text
TCP probe 3s
API probe 2s
```

最终可以：

```text
singbox_api state becomes STREAMING/HEALTHY
```

作为 readiness signal之一。

这样 `service_health.c` 可读取 cached API state。

---

# 47. `GetVersion` 不需要每次 query

sing-box version 在一个 process generation 内基本不变。

推荐：

```text
service generation changed
→ query once
→ cache version
```

不要 status 每次调用 `GetVersion`。

---

# 48. Clash mode cache

`GetClashModeStatus` 同样可以：

```text
startup/reconnect
reload
set mode success后
```

刷新。

无需每次 status建立 unary RPC。

---

# 49. `SetClashMode` 是控制操作，可以同步吗？

控制命令允许比 telemetry稍慢，但仍建议 bounded deadline。

可以保留 request/response：

```text
caller explicitly requests mode change
→ RPC
→ result
```

但不要在普通 status hot path触发。

---

# 50. Set 后更新 cache

`SetClashMode` 成功后：

不要单纯假设 mode一定改变。

推荐：

```text
RPC success
→ schedule GetClashModeStatus refresh
```

或 server state event更新。

---

# 51. `reload` 与 API control 要统一错误语义

例如：

```c
typedef enum {
    SINGBOX_API_OK = 0,
    SINGBOX_API_ERR_TRANSPORT,
    SINGBOX_API_ERR_TIMEOUT,
    SINGBOX_API_ERR_AUTH,
    SINGBOX_API_ERR_HTTP,
    SINGBOX_API_ERR_GRPC,
    SINGBOX_API_ERR_PROTOCOL,
    SINGBOX_API_ERR_UNSUPPORTED,
    SINGBOX_API_ERR_NOT_RUNNING
} singbox_api_error_t;
```

不要所有东西只：

```text
0 / -1
```

内部至少保存详细原因。

Public API可以暂时保留 int以减少破坏。

---

# 52. Parser：`read_varint()` 方向基本正确

当前有：

```text
shift < 64
pos < len
```

并在未完成时返回失败。

应保留。

测试：

```text
unterminated varint
10-byte overflow
oversized field
```

---

# 53. Protobuf key field number需要校验

当前：

```text
field = key >> 3
wire = key & 7
```

建议拒绝：

```text
field == 0
```

protobuf field number 0 非法。

---

# 54. Signed int64字段语义要核对 protobuf schema

当前：

```text
uplink
downlink
total
```

直接：

```c
(int64_t)value
```

需要确认 upstream proto是：

```text
int64
uint64
sint64
```

如果是 `sint64`：

需要 zig-zag decode。

Codex必须对照当前 sing-box Native API proto定义确认。

---

# 55. 不要靠注释长期手写 schema

当前代码注释：

```text
field 1 memory
field 2 goroutines
...
```

这是脆弱点。

至少增加：

```text
schema version / upstream commit reference
```

并测试 fixture。

如果 upstream提供 `.proto` 可作为 build/test input：

优先从 schema生成或校验常量。

---

# 56. API 兼容性测试

需要对目标 sing-box版本：

```text
1.14.x（以及项目实际支持范围）
```

保存真实 response fixture。

测试：

```text
Status
GetVersion
GetClashModeStatus
SetClashMode
```

防止 upstream字段变化后 ATPD静默解析错。

---

# 57. 当前 gRPC-Web是实现细节，不要泄漏到 public API

Public header 应保持：

```text
get_status
get_version
get_clash_mode
set_clash_mode
```

不要公开：

```text
HTTP
chunk
protobuf frame
```

这样未来改 standard gRPC/UDS时上层不用改。

---

# 58. HTTP header parser要大小写无关

当前 status path：

```text
strcasestr
```

unary path只匹配两种：

```text
transfer-encoding: chunked
Transfer-Encoding: chunked
```

不够规范。

统一成：

```text
case-insensitive header parser
```

不要硬编码两种大小写。

---

# 59. Chunk extension

HTTP chunk size允许：

```text
1A;extension=value
```

当前：

```text
strtoul
要求 endptr == '\0'
```

会拒绝 chunk extension。

如果 sing-box永远不使用，可以接受。

但 parser contract应明确。

更稳妥支持：

```text
hex digits
optional `;...`
```

---

# 60. Chunk CRLF 边界

解析 chunk后必须确认：

```text
chunk data 后有 \r\n
```

当前主要按长度推进，测试必须覆盖：

```text
missing CRLF
partial CRLF
multiple chunks
```

---

# 61. Multiple HTTP chunks

status parser会遍历 chunk。

unary parser当前主要聚焦首 chunk/frame。

要测试：

```text
gRPC frame跨多个 HTTP chunk
```

如果 upstream可能这样发送，当前实现会失败。

正确 parser应把：

```text
HTTP chunk stream
```

先还原为连续 body byte stream，再交给：

```text
gRPC-Web frame parser
```

---

# 62. 分层 parser

推荐：

```text
TCP byte stream
↓
HTTP response parser
↓
HTTP body byte stream
↓
gRPC-Web frame parser
↓
protobuf parser
```

不要让 unary/status各自重复半套解析。

---

# 63. Header size限制

设置：

```text
MAX_HTTP_HEADER = 8 KiB
```

超过：

```text
protocol error
```

避免无限等 header终止。

---

# 64. Frame size限制

设置：

```text
MAX_GRPC_FRAME
```

根据实际 API：

```text
status 可小
clash mode 可能更大
```

例如：

```text
64 KiB
```

不要盲目 malloc protobuf声称的任意长度。

---

# 65. 当前 status buffer 4 KiB

```text
buf[4096]
```

如果：

```text
headers + chunk metadata + first frame > 4095
```

会失败。

目前 Status frame通常很小，所以不是 immediate P0。

但 persistent/incremental parser可自然解决固定 buffer限制。

---

# 66. Unary response 8 KiB同理

不要把：

```text
8192
```

当协议保证。

推荐 bounded dynamic/incremental parser。

---

# 67. Health counters

建议：

```text
connect_attempts
connect_failures
rpc_success
rpc_failures
timeouts
protocol_errors
auth_failures
reconnects
frames_received
last_frame_ms
```

---

# 68. status 应显示的不是底层噪声

人类 status：

```text
Native API       HEALTHY
Telemetry age    0.4s
sing-box memory  ...
Goroutines       ...
Clash mode       rule
```

DEGRADED：

```text
Native API       DEGRADED
Last success     8.2s ago
Reason           timeout
```

---

# 69. 不要让 singbox_api修改全局 status renderer

该模块只提供：

```text
snapshot
```

renderer在：

```text
status.c/status_render.c
```

做。

---

# 70. Cleanup

当前 cleanup只：

```text
connected=0
last_check=0
```

如果未来增加：

```text
stream fd
reactor registration
timer
retry timer
buffers
```

cleanup必须：

```text
cancel timers
remove fd
close fd
free buffers
reset state
```

且幂等。

---

# 71. Init 必须 transactional

未来：

```text
init ctx
attach reactor
create retry timer
...
```

任何步骤失败：

```text
rollback
return error
```

不要返回半初始化 manager。

---

# 72. Test：exec_cli fake success

当前行为必须先加 regression：

```text
call singbox_api_exec_cli
```

在未实现时预期：

```text
-1 / ENOTSUP
```

绝不能 0。

---

# 73. Test：reload ownership

构造：

```text
two sing-box processes
```

确认：

```text
singbox_api_reload()
```

不能靠 name lookup随机选择。

最终应只作用：

```text
service-owned child
```

---

# 74. Test：invalid host

配置：

```text
invalid-host-value
```

当前不可静默变：

```text
127.0.0.1
```

预期：

```text
config/init failure
```

或真实 hostname resolve。

---

# 75. Test：connect timeout

server：

```text
blackhole / delayed accept
```

验证：

```text
absolute deadline
```

例如设置 500ms：

```text
RPC ≤ bounded tolerance
```

---

# 76. Test：server accepts but never responds

验证：

```text
RPC timeout
socket closed
state DEGRADED
reactor不长时间阻塞
```

---

# 77. Test：HTTP 401

验证：

```text
AUTH_FAILED
```

不要：

```text
generic timeout
```

---

# 78. Test：HTTP 404

验证：

```text
INCOMPATIBLE/UNIMPLEMENTED
```

---

# 79. Test：HTTP 500

验证：

```text
server/RPC error
```

并受 backoff控制。

---

# 80. Test：gRPC trailers error

构造：

```text
HTTP 200
data/trailer
grpc-status != 0
```

unary RPC必须失败。

---

# 81. Test：trailers-only response

当前代码已有部分识别。

保留 regression。

---

# 82. Test：fragmented TCP

把：

```text
HTTP headers
chunk size
gRPC 5-byte header
protobuf
```

分别按单字节/小块发送。

parser必须正确。

---

# 83. Test：gRPC frame跨 HTTP chunk

例如：

```text
chunk 1: first 3 bytes
chunk 2: rest
```

parser仍应成功。

---

# 84. Test：multiple status frames

Persistent SubscribeStatus：

```text
frame1
frame2
frame3
```

snapshot应：

```text
总是更新到最新
```

不会内存增长。

---

# 85. Test：stream disconnect/reconnect

```text
healthy
↓
server close
↓
DEGRADED
↓
backoff
↓
reconnect
↓
HEALTHY
```

最后：

```text
snapshot age恢复
```

---

# 86. Test：sing-box restart

service generation改变：

```text
old stream disconnect
new child start
new API reconnect
version/clash/status重新刷新
```

确保不会继续使用旧 child generation的错误 state。

---

# 87. Test：API unavailable while sing-box process alive

这是很重要的 degraded场景：

```text
service = RUNNING
API = DEGRADED
```

ATPD不应因此误认为：

```text
sing-box process dead
```

service health policy另行决定是否需要 restart。

---

# 88. Test：status latency

Native API完全不可用：

```text
atpd status
```

仍应：

```text
<100–250 ms
```

理想：

```text
<20 ms
```

因为只读 cache。

---

# 89. Test：status ×5000

API healthy/unhealthy分别执行：

```text
5000 queries
```

检查：

```text
无 5000 次 API TCP connect
FD stable
RSS stable
status latency stable
```

---

# 90. Test：telemetry sampling resource

长期：

```text
30 min / 24h
```

检查：

```text
API reconnect次数合理
FD baseline
ATPD RSS无 slope
sing-box goroutines无因 ATPD stream反复创建而增长
```

---

# 91. Parser fuzz

很适合 fuzz：

```text
parse_status_frame
HTTP chunk parser
gRPC-Web frame parser
parse_string_field
parse_clash_mode_status
```

输入：

```text
arbitrary bytes
```

要求：

```text
never crash
never OOB
bounded CPU
bounded allocation
```

---

# 92. Sanitizers

```text
ASan
UBSan
```

parser stress必跑。

如果 Native API manager完全 reactor-thread：

不必额外引入 TSan复杂度。

---

# 93. 推荐提交顺序

## Commit 1

```text
singbox-api: remove false-success control paths
```

内容：

- `exec_cli` 返回 ENOTSUP或删除
- reload移出按名称 PID lookup
- tests

---

## Commit 2

```text
singbox-api: unify transport deadlines and errors
```

内容：

- invalid host fail
- unified timeout
- nonblocking transport
- check syscall failures
- absolute deadline

---

## Commit 3

```text
singbox-api: harden HTTP and gRPC-Web parsing
```

内容：

- HTTP status
- case-insensitive headers
- gRPC status
- chunk/frame bounds
- parser tests/fuzz

---

## Commit 4

```text
singbox-api: add cached runtime snapshot
```

内容：

- API state
- freshness
- last-good data
- error counters
- status reads snapshot

---

## Commit 5

```text
singbox-api: decouple health from TCP reachability
```

内容：

- transport vs RPC healthy
- explicit DEGRADED/AUTH/INCOMPATIBLE

---

## Commit 6

```text
singbox-api: maintain SubscribeStatus stream
```

内容：

- reactor-native stream
- reconnect/backoff
- incremental parser
- update cached telemetry

如果这一 commit 风险较高：

可以作为第二阶段。

---

## Commit 7

```text
singbox-api: cache version and clash mode by service generation
```

---

# 94. Codex 修改前必须先做调用图

搜索：

```text
singbox_api_init
singbox_api_cleanup
singbox_api_health_check
singbox_api_get_status
singbox_api_get_version
singbox_api_get_goroutines
singbox_api_get_clash_mode
singbox_api_get_clash_mode_status
singbox_api_set_clash_mode
singbox_api_reload
singbox_api_exec_cli
```

列出：

```text
caller
是否 reactor callback
是否 UDS/status hot path
最大允许 latency
失败后的上层行为
```

尤其检查：

```text
status.c
service.c / service_health
api.c
config reload
UDS
```

---

# 95. Codex必须确认 upstream schema

对当前项目支持的 sing-box Native API版本确认：

```text
StartedService
SubscribeStatus
GetVersion
GetClashModeStatus
SetClashMode
```

对应：

```text
protobuf field number
wire type
RPC path
request schema
response schema
```

不要仅凭当前 C 代码注释继续复制。

---

# 96. 与 `status.c` 重构方案的关系

这是直接依赖：

```text
singbox_api manager
↓
cached singbox snapshot
↓
status_collect
↓
status render
```

完成后删除 status hot path中的：

```text
sync API retries
```

---

# 97. 与 `service.c` 重构方案的关系

最终：

```text
service
→ owns sing-box process

singbox_api
→ owns API transport

service_health
→ reads API cached health

reload
→ service-owned child or typed RPC
```

不要：

```text
singbox_api
→ search process name
→ kill arbitrary PID
```

---

# 98. 与 Go 未来架构的关系

即使未来 ATPD-Go 使用标准 gRPC：

当前 C 版仍应保持 public semantic boundary：

```text
get snapshot
set clash mode
query version
health state
```

不要让 HTTP/gRPC-Web details泄漏到其他模块。

这样 C baseline也能成为未来 Go实现的行为参考。

---

# 99. 是否需要现在拆文件

第一阶段：

```text
不强制
```

624行仍可管理。

但如果加入：

```text
persistent stream
incremental HTTP parser
retry state machine
snapshot
```

预计会明显增长。

那时推荐拆：

```text
singbox_api.c
singbox_api_transport.c
singbox_api_proto.c
singbox_api_internal.h
```

这是比按“status/version/clash”拆分更合理的边界。

---

# 100. 不建议按 RPC 方法拆文件

不要：

```text
singbox_status.c
singbox_version.c
singbox_clash.c
```

因为真正共享复杂度在：

```text
transport
protocol framing
state
```

不是方法数量。

---

# 101. 最终 Invariants

Codex应落实：

```text
I1:
ordinary status rendering never performs Native API network I/O

I2:
TCP reachable != RPC healthy

I3:
all RPCs have one absolute monotonic deadline

I4:
invalid configured host never silently becomes loopback

I5:
unimplemented operation never returns success

I6:
singbox_api never signals a PID it does not own

I7:
last-known-good telemetry remains available with explicit freshness

I8:
HTTP/gRPC protocol error is distinguishable from transport error

I9:
all stream/socket/timer resources close exactly once

I10:
service process lifecycle and API transport ownership remain separate
```

---

# 102. 最终验收标准

## Correctness

```text
exec_cli no false success
reload cannot hit unrelated sing-box process
```

## Latency

```text
status with API down
→ no second-level blocking
```

## Health

```text
TCP reachable but RPC invalid
→ not HEALTHY
```

## Config

```text
invalid host
→ explicit error
```

## Protocol

```text
401 / 404 / grpc-status != 0
→ correctly classified
```

## Parser

```text
fragmented TCP/chunk/frame
→ correct
malformed input
→ no crash/OOB
```

## Recovery

```text
API disconnect
→ DEGRADED
→ bounded backoff
→ automatic HEALTHY recovery
```

## Snapshot

```text
last-good values retained
freshness visible
```

## Resources

```text
status ×5000
→ no per-query API socket churn

24h telemetry
→ FD/RSS stable
```

---

# 103. 最终结论

`singbox_api.c` 的方向是对的：ATPD已经从旧式命令/文本探测逐步转向 sing-box Native API。

但当前实现仍然偏：

```text
“需要数据时临时打开一条 gRPC-Web连接去拿”
```

下一阶段应该升级成：

```text
“ATPD长期维护 Native API observed state，上层只读取 snapshot”
```

最优先修复的不是重写协议，而是：

```text
1. 消除 fake success
2. reload 回归 service ownership
3. status 不再同步连接 Native API
4. transport/RPC health 分离
5. 所有 RPC bounded deadline
6. parser具备明确 HTTP/gRPC error semantics
```

之后再视复杂度决定是否上 persistent `SubscribeStatus` stream。

完成后，这个模块会真正成为：

> ATPD 与 sing-box 之间稳定、可观测、可恢复的运行时状态桥梁，而不是 status 命令触发的临时网络查询器。
