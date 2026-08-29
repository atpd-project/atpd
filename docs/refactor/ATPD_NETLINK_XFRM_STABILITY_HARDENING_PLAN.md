# ATPD `netlink.c` / XFRM 稳定性加固方案

## 1. 结论

当前 `netlink.c` 大约 600 行，虽然同时承担：

- Route Netlink socket
- async link/address/route event
- synchronous RTM_GETLINK query
- interface statistics
- VPN interface detection
- XFRM SA listener
- debounce / settle timer
- VPN state transition

但目前仍不建议为了行数立即大拆文件。

优先目标应该是：

> 修正 Netlink/XFRM 注册状态、ET/read-drain 语义、消息边界、timer ownership、同步查询错误处理和可观测状态。

建议继续保持：

```text
netlink.c
netlink.h
```

第一轮完成稳定性加固。

后续如果 XFRM 逻辑继续增长，再单独抽：

```text
netlink_xfrm.c
```

但不是本次前置条件。

---

# 2. 当前模块的主要风险

按优先级：

```text
P0/P1
1. XFRM reactor 注册失败仍标记 registered
2. XFRM 已有 fd 的重新注册路径同样忽略 reactor_add_fd 返回
3. async Netlink/XFRM callback 每次只 recv 一次，可能没有 drain socket
4. debounce timer 创建失败无处理

P1
5. recvmsg / recv 没有检查 truncation
6. Netlink message payload 长度验证不足
7. setsockopt(SO_RCVBUF/SO_RCVTIMEO) 返回值未处理
8. 同步 query socket drain 语义较粗
9. callback / userdata API 目前基本没有真正使用
10. global registration 状态与真实 reactor 状态可能分叉

P2
11. VPN interface detection 过度依赖名称前缀
12. `/proc/net/dev` 与 Netlink stats 两套路径语义应统一
13. 缺少显式 Netlink/XFRM health snapshot
14. cleanup / retry / degraded 状态不够明确
```

---

# 3. P0/P1：XFRM reactor 注册不能“假成功”

当前类似：

```c
reactor_add_fd(
    r,
    fd,
    REACTOR_EVENT_READ,
    netlink_xfrm_event_cb,
    NULL
);

g_xfrm_reactor = r;
atomic_store(&g_xfrm_registered, 1);
```

问题：

```text
reactor_add_fd() 失败
        ↓
g_xfrm_registered = 1
        ↓
status / retry 逻辑认为 listener 已注册
        ↓
实际 reactor 永远收不到 XFRM event
```

这是必须修复的问题。

---

# 4. 正确注册顺序

必须：

```c
if (reactor_add_fd(
        r,
        fd,
        REACTOR_EVENT_READ,
        netlink_xfrm_event_cb,
        NULL) != 0) {

    LOG_ERROR(...);

    atomic_store(&g_xfrm_registered, 0);
    g_xfrm_reactor = NULL;

    return -1;
}

g_xfrm_reactor = r;
atomic_store(&g_xfrm_registered, 1);
```

核心原则：

> registered 只能描述“reactor 注册已经成功”，不能描述“我们尝试注册过”。

---

# 5. 已存在 `g_xfrm_fd` 的路径也要修

当前：

```text
g_xfrm_fd >= 0
+
新 reactor
+
!registered
```

时也会重新：

```text
reactor_add_fd()
```

该路径必须和第一次 init 使用同一 helper。

建议新增内部函数：

```c
static int xfrm_register_reactor(
    reactor_t *r
);
```

统一处理：

```text
fd validity
already registered
reactor_add_fd
state update
logging
```

避免 `netlink_xfrm_init()` 和 `netlink_set_reactor()` 各写一套。

---

# 6. XFRM 状态不要只用一个 bool

当前：

```text
registered = 0/1
```

信息不足。

建议：

```c
typedef enum {
    NETLINK_MONITOR_STOPPED,
    NETLINK_MONITOR_OPEN,
    NETLINK_MONITOR_REGISTERED,
    NETLINK_MONITOR_RETRYING,
    NETLINK_MONITOR_DEGRADED,
    NETLINK_MONITOR_FAILED
} netlink_monitor_state_t;
```

至少内部区分：

```text
socket opened
reactor registered
listener operational
retry pending
```

---

# 7. XFRM ownership

必须明确：

```text
netlink module owns g_xfrm_fd
```

reactor：

```text
只监听
不负责 close
```

cleanup：

```text
reactor_remove_fd
        ↓
close(g_xfrm_fd)
        ↓
fd = -1
registered = false
reactor = NULL
```

并与 reactor ownership contract 对齐。

---

# 8. P1：异步 callback 必须 drain 到 `EAGAIN`

当前：

```text
netlink_handle_event()
→ recv() 一次

netlink_xfrm_event_cb()
→ recvmsg() 一次
```

如果一次 socket readable 中积累了多批 Netlink datagram：

```text
callback 只消费第一批
```

对于 edge-triggered reactor 尤其危险：

> 没有 drain 到 `EAGAIN`，可能剩余消息长期留在 socket buffer 中，却没有新的 edge 唤醒。

因此 callback 必须：

```text
for (;;) {
    recvmsg(... MSG_DONTWAIT)

    >0
        parse

    EINTR
        continue

    EAGAIN/EWOULDBLOCK
        break

    other error
        record failure
        break
}
```

Route Netlink 与 XFRM 都一样。

---

# 9. 不要在一个 callback 中无限工作

虽然需要 drain 到 `EAGAIN`，但也要避免 storm 情况下 monopolize reactor。

可增加软预算，例如：

```text
max_datagrams_per_callback = 64
```

如果达到预算仍可能有数据：

```text
reactor/event loop 下一轮继续处理
```

是否需要预算可根据压力测试决定。

第一版可以先完整 drain。

---

# 10. P1：检查 `MSG_TRUNC`

XFRM 使用：

```c
recvmsg()
```

但当前没有检查：

```text
msg.msg_flags & MSG_TRUNC
```

如果 8 KiB buffer 不够：

```text
消息可能被截断
```

此时继续解析是不安全的。

必须：

```c
if (msg.msg_flags & MSG_TRUNC) {
    stats.truncated++;
    LOG_WARN(...);
    trigger full refresh / resync;
    continue;
}
```

不要解析 truncated Netlink datagram。

---

# 11. Route async path也建议改为 `recvmsg()`

当前 route event 使用：

```c
recv()
```

为了统一：

```text
sender metadata
MSG_TRUNC
message flags
```

建议改成：

```c
recvmsg()
```

Route/XFRM 两条监听路径共享类似 receive helper。

例如：

```c
static ssize_t netlink_recv_datagram(
    int fd,
    void *buf,
    size_t size,
    struct sockaddr_nl *addr,
    int *msg_flags
);
```

---

# 12. 验证 Netlink sender

对 kernel event listener：

应检查：

```text
sockaddr_nl.nl_pid == 0
```

即：

```text
消息来自 kernel
```

如果不是：

```text
ignore / reject
```

除非当前模块明确需要 userspace Netlink sender。

ATPD 的 route/XFRM monitor 应默认只信 kernel。

---

# 13. Netlink header 最低长度验证

在访问：

```c
struct ifinfomsg
struct xfrm_usersa_info
struct nlmsgerr
```

前必须确认：

```text
nlmsg_len >= NLMSG_LENGTH(required_payload)
```

不能仅依赖：

```text
NLMSG_OK
```

就直接把 payload cast 成目标结构。

---

# 14. XFRM `NEWSA` payload 验证

在：

```c
struct xfrm_usersa_info *sa_info = NLMSG_DATA(h);
```

前：

```text
h->nlmsg_len >= NLMSG_LENGTH(sizeof(struct xfrm_usersa_info))
```

否则：

```text
malformed / truncated
→ ignore + telemetry
```

---

# 15. XFRM attribute 长度验证

对于：

```text
XFRMA_IF_ID
```

必须保证：

```text
RTA_PAYLOAD(rta) >= sizeof(uint32_t)
```

再：

```c
memcpy(&if_id, RTA_DATA(rta), sizeof(if_id));
```

不要默认 attribute 一定完整。

---

# 16. Route `RTM_NEWLINK` payload 验证

`parser_link_sync()` 在：

```c
struct ifinfomsg *ifi = NLMSG_DATA(h);
```

前验证：

```text
NLMSG_PAYLOAD(h, 0) >= sizeof(struct ifinfomsg)
```

---

# 17. `IFLA_STATS64` 长度验证

当前看到：

```text
rta_type == IFLA_STATS64
→ cast rtnl_link_stats64
```

必须确认：

```text
RTA_PAYLOAD(rta) >= sizeof(struct rtnl_link_stats64)
```

否则跳过该 attribute。

---

# 18. `IFLA_IFNAME`

现有 `safe_copy_ifname()` 方向正确。

继续保持：

```text
bounded copy
NUL terminate
```

并拒绝：

```text
zero-length attribute
```

即可。

---

# 19. `NLMSG_ERROR` 长度验证

同步 query 收到：

```text
NLMSG_ERROR
```

前必须确认至少：

```text
sizeof(struct nlmsgerr)
```

然后才读取：

```c
err->error
```

---

# 20. `NLMSG_OVERRUN`

监听 callback 应显式处理：

```text
NLMSG_OVERRUN
```

这意味着消息丢失。

正确动作不是仅 log：

```text
mark monitor degraded
        ↓
schedule full state refresh
```

因为 event stream 已不再完整。

---

# 21. ENOBUFS

Netlink socket 在 buffer overflow 时可能出现：

```text
ENOBUFS
```

这同样意味着事件可能丢失。

处理：

```text
stats.overruns++
state = DEGRADED
schedule resync
```

而不是简单 return。

---

# 22. Full resync

需要把：

```text
event-driven incremental update
```

和：

```text
authoritative full scan
```

分开。

推荐：

```text
Netlink event
→ debounce
→ netlink_get_active_vpn()
→ authoritative state transition
```

当前其实已经有这个方向。

要正式把它定义成：

> Netlink event 只是“状态可能变化”的提示，最终状态由 refresh/resync 决定。

这样即使丢一个 event，也可恢复。

---

# 23. XFRM 事件同样不要直接当最终事实

当前：

```text
XFRM NEWSA
→ PREDICTING
→ delayed refresh

XFRM DELSA
→ TEARDOWN
→ delayed refresh
```

这个思路是好的。

继续保持：

```text
XFRM = hint
refresh = authority
```

不要让单个 SA event 直接永久决定 VPN state。

---

# 24. P1：debounce timer 创建失败

当前：

```c
g_debounce_timer =
    reactor_add_timer(...);
```

返回未检查。

失败后：

```text
refresh silently lost
```

结果：

```text
Netlink/XFRM event 收到了
但最终 VPN state 没有刷新
```

必须：

```c
reactor_timer_t *timer = reactor_add_timer(...);

if (!timer) {
    LOG_ERROR(...);

    /* fallback */
    ...
}
```

---

# 25. Timer failure fallback

不要 timer 创建失败后什么都不做。

可选策略：

### A 推荐

立即执行一次：

```text
netlink_refresh_now
```

如果该函数会 blocking reactor，则不能这么做。

### B

标记：

```text
refresh_pending = true
```

由 reactor 下一可用机会或全局 reconciliation 执行。

结合当前同步 Netlink query 可能最多阻塞数百 ms，建议避免直接在 reactor callback 中做同步 full query。

因此推荐：

```text
timer allocation fail
→ mark degraded
→ request coordinator reconciliation
```

---

# 26. Debounce timer ownership

当前使用：

```text
g_debounce_timer
g_debounce_reactor
g_debounce_lock
```

必须统一规则：

```text
timer callback fires
→ g_debounce_timer = NULL

cancel
→ cancel timer
→ g_debounce_timer = NULL

cleanup
→ cancel if present
→ reactor = NULL
```

结合 reactor timer ownership 加固方案。

---

# 27. 不建议在 single-thread reactor 中滥用 pthread mutex

当前模块包含：

```text
g_nl_mutex
g_debounce_lock
```

需要先确认哪些调用真的会跨线程。

如果实际所有：

```text
async event
debounce
state refresh
```

都只从 reactor thread 运行，那么部分 mutex 可能是历史遗留。

但第一轮不要贸然删除。

先记录：

```text
每个 public API 的调用线程
```

后续再判断。

---

# 28. 同步 `g_sync_fd` 的并发模型

`g_sync_fd` 被多个同步查询共用：

```text
netlink_get_active_vpn
netlink_get_iface_stats
```

使用：

```text
g_nl_mutex
```

串行化，这个方向是合理的。

但 contract 必须明确：

> 所有使用共享 sync fd 的 request/response transaction 必须持有 `g_nl_mutex`。

不能未来新增 query 忘记加锁。

---

# 29. 不要在获取 mutex 前 drain 共享 fd

任何：

```text
drain
send
recv
```

共享 `g_sync_fd` 的动作都必须在同一 transaction lock 内。

当前主要路径已经基本如此，应通过 helper 固化。

---

# 30. 建议抽一个 sync transaction helper

例如：

```c
static int netlink_query(
    const void *req,
    size_t req_len,
    uint32_t seq,
    nl_msg_parser_t parser,
    void *userdata,
    int timeout_ms
);
```

内部统一：

```text
lock
drain
send
recv
unlock
```

这样不会每个 query 重复 ownership/error handling。

---

# 31. `netlink_drain_socket()` 语义需要加强

当前简单：

```text
recv(MSG_DONTWAIT)
直到 <=0
```

应该区分：

```text
EAGAIN → drained
EINTR → retry
other error → report
```

并统计：

```text
discarded stale messages
```

---

# 32. Sequence number

当前：

```text
atomic_uint g_seq
```

可以继续。

但建议：

```text
seq = atomic_fetch_add(...) + 1
```

避免长期使用 0 作为普通 request seq（虽然内核通常可接受）。

wrap-around 可以自然允许。

---

# 33. Sync recv 必须处理 EINTR

当前 `recv()` error path里重点看 timeout，但要明确：

```text
EINTR
→ continue
```

不要把 signal interruption 当 Netlink failure。

---

# 34. Sync recv multipart 结束条件

对于：

```text
NLM_F_DUMP
```

正常：

```text
NLMSG_DONE
```

对于非 multipart 请求：

可能只有：

```text
single RTM_NEWLINK
```

如果代码一律等待 `NLMSG_DONE`，需要确认不会导致不必要 timeout。

建议 recv helper 根据：

```text
NLM_F_MULTI
request type
```

决定 transaction completion。

尤其 `get_iface_stats()` 使用：

```text
RTM_GETLINK
NLM_F_REQUEST
```

必须验证 kernel reply 后是否按当前逻辑可靠结束。

---

# 35. `setsockopt()` 不能完全忽略

当前：

```text
SO_RCVBUF
SO_RCVTIMEO
```

设置失败均没有处理。

建议：

### `SO_RCVBUF`

失败：

```text
WARN
继续
```

但记录实际 buffer。

### `SO_RCVTIMEO`

同步 query timeout 是 correctness contract。

失败：

```text
最好直接返回 error
```

或者改用：

```text
poll + monotonic deadline
```

而不是依赖 socket-level timeout。

---

# 36. 推荐用 poll + deadline 做 sync timeout

相比每次：

```text
setsockopt(SO_RCVTIMEO)
```

更推荐：

```text
poll()
+
monotonic absolute deadline
+
recvmsg()
```

这样：

- timeout 不污染共享 socket option
- EINTR 后剩余 timeout 可精确计算
- transaction 逻辑更清楚

但这是 P1/P2，可在基础注册修复后实施。

---

# 37. 注意同步查询可能阻塞 reactor

`netlink_get_active_vpn()` fallback 会：

```text
send Netlink dump
recv up to ~500 ms
```

`debounce_timer_cb()` 又直接调用：

```text
netlink_get_active_vpn()
```

也就是说：

> debounce timer callback 可能阻塞 reactor 数百毫秒。

这与 ATPD 的 single-thread reactor 目标不完全一致。

---

# 38. 优先减少 reactor 中的同步 Netlink query

长期推荐：

```text
async Netlink monitor
        ↓
maintained network snapshot
        ↓
VPN detector
```

而不是：

```text
event
→ timer
→ synchronous RTM_GETLINK dump
```

第一轮可以暂时保留，但要测 latency。

---

# 39. 中期方案：维护 link cache

可维护：

```c
typedef struct {
    int ifindex;
    char ifname[IFNAMSIZ];
    unsigned int flags;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
} netlink_link_state_t;
```

通过：

```text
RTM_NEWLINK
RTM_DELLINK
```

更新。

这样：

```text
active VPN detection
```

大部分情况下可以从 cache 完成。

全量 RTM_GETLINK dump 只用于：

```text
startup
resync
overrun recovery
```

---

# 40. 但不要在本次一次性引入完整 cache

这是架构优化，不是 P0 fix。

建议顺序：

```text
先修注册/解析/错误恢复
        ↓
测试 reactor blocking
        ↓
如果确实有影响
        ↓
再做 link snapshot/cache
```

---

# 41. `g_callback` / `g_userdata` 需要整理

`netlink_init()` 接收：

```c
nl_callback_t callback,
void *userdata
```

并保存：

```text
g_callback
g_userdata
```

但当前主要 event path 似乎并没有真正使用它们。

这会造成 public API 误导。

需要 Codex 审计：

```text
是否整个 repo 还有调用用途
```

如果确认完全未使用：

### 方案 A 推荐

移除 callback 参数，简化：

```c
int netlink_init(void);
```

### 方案 B

真正实现规范化 event callback。

不要保留“看起来支持 callback、实际不发 callback”的 API。

---

# 42. 如果保留 callback，应先 normalize event

不要把 raw Netlink message直接传上层。

应该：

```text
RTM_NEWADDR
→ NL_EVENT_ADDR_ADD

RTM_DELADDR
→ NL_EVENT_ADDR_DEL

RTM_NEWROUTE
→ NL_EVENT_ROUTE_ADD
...
```

并尽量提供：

```text
ifindex
iface
family
```

但当前 ATPD如果只需要“network changed”，可直接移除 callback API，减少复杂度。

---

# 43. VPN interface detection 语义

当前通过名称前缀判断：

```text
tun
warp
wg
tailscale
zt
zerotier
utun
vpn
ppp
ipsec
xfrm
```

作为 heuristic 可以接受。

但必须承认：

```text
interface name != authoritative VPN identity
```

可能：

- false positive
- false negative
- vendor naming changed

---

# 44. 将 interface-name detection 明确标记为 heuristic

建议内部命名：

```c
is_vpn_candidate_interface()
```

比：

```c
is_proxy_interface()
```

语义更准确。

它只是：

```text
candidate
```

最终 VPN state 仍通过：

```text
XFRM hint
link state
route state
known interface
```

综合决定。

---

# 45. `"tun"` 不应直接标成 Cloudflare WARP

当前 label：

```text
tun* → "Cloudflare WARP / TUN"
```

这可能误导 observability。

建议：

```text
warp* → Cloudflare WARP
wg* → WireGuard
tailscale* → Tailscale
ipsec/xfrm → IPsec
tun* → TUN
```

除非另有明确 WARP signature。

---

# 46. XFRM if_id → `ipsec%u` 也是 heuristic

当前：

```c
snprintf(ifname, ..., "ipsec%u", if_id - 1);
```

需要确认 Android/目标内核上的约定是否稳定。

不要把：

```text
if_id
```

永久等同：

```text
ipsec index
```

建议：

```text
PREDICTING state 里保存 if_id
```

真正接口名由 delayed refresh 确认。

---

# 47. `XFRM_MSG_DELSA` 不代表 VPN 一定完全断开

一个 VPN 可以有多个 SA。

收到：

```text
DELSA
```

只能说明：

```text
某条 SA 删除
```

不能直接认为：

```text
整个 IPsec VPN 已 teardown
```

当前随后 delayed refresh 可以纠正，这是好事。

建议把状态命名/日志改成：

```text
XFRM change detected
→ re-evaluating
```

而不是过强的“VPN disconnected”语义。

---

# 48. XFRM group coverage

当前订阅：

```text
XFRMNLGRP_SA
```

需要验证 ATPD 所需事件是否仅 SA 足够。

如果未来要判断：

```text
policy changes
```

可能还需要：

```text
XFRMNLGRP_POLICY
```

但不要无需求增加事件源。

当前维持 SA 监听即可。

---

# 49. Socket receive buffer

当前目标：

```text
NL_DUMP_SIZE = 32768
```

但：

```text
SO_RCVBUF
```

内核可能自动调整/加倍或受 sysctl 限制。

建议 init 后：

```text
getsockopt(SO_RCVBUF)
```

记录实际值到 stats/status。

---

# 50. Async listener buffer可以适当提高

在 Android 网络快速变化时：

```text
link/address/route event burst
```

32 KiB 可能偏保守。

可测试：

```text
64 KiB
128 KiB
256 KiB
```

不要凭感觉直接定大值。

指标：

```text
ENOBUFS
NLMSG_OVERRUN
memory
```

---

# 51. Cleanup 必须幂等

`netlink_cleanup()` 应保证：

```text
第一次 cleanup 正常
第二次 cleanup 无副作用
partial init cleanup 安全
```

检查：

```text
sync fd
async fd
xfrm fd
reactor registration
debounce timer
callback/userdata
state flags
```

全部回 baseline。

---

# 52. Cleanup 时清 callback state

如果继续保留：

```text
g_callback
g_userdata
```

cleanup：

```text
set NULL
```

否则潜在 stale pointer。

---

# 53. `g_atpd_ctx.xfrm_fd` 也必须同步清理

init 时：

```text
g_atpd_ctx.xfrm_fd = fd
```

cleanup 时必须：

```text
g_atpd_ctx.xfrm_fd = -1
```

否则全局 context 可能显示 stale fd。

---

# 54. 不要让 status 用 fd>=0 推断 monitor ACTIVE

正确：

```text
fd >= 0
```

只说明：

```text
socket exists
```

不是：

```text
reactor registered
listener healthy
```

status 应读取显式 monitor state。

---

# 55. 建议增加 Netlink status snapshot

例如：

```c
typedef struct {
    netlink_monitor_state_t route_state;
    netlink_monitor_state_t xfrm_state;

    int route_fd;
    int xfrm_fd;

    bool route_registered;
    bool xfrm_registered;

    uint64_t route_events;
    uint64_t xfrm_events;

    uint64_t parse_errors;
    uint64_t recv_errors;
    uint64_t overruns;
    uint64_t truncated;

    uint64_t refresh_requested;
    uint64_t refresh_completed;
    uint64_t refresh_failed;

    uint64_t last_event_ms;
    uint64_t last_refresh_ms;

    int last_errno;
} netlink_status_t;
```

---

# 56. Status API

```c
int netlink_get_status(
    netlink_status_t *out
);
```

要求：

```text
non-blocking
read-only
no socket query
no `/proc`
```

供新的 `status_snapshot_t` 使用。

---

# 57. 错误状态

推荐至少记录：

```text
last_errno
last_error_stage
```

stage：

```text
socket
bind
register
recv
parse
timer
resync
```

这样 Android 真机排障很有价值。

---

# 58. Retry

当前如果 XFRM init/register 失败：

应明确是否：

```text
永久 disabled
```

还是：

```text
later retry
```

推荐：

```text
socket/bind 不支持
→ DEGRADED / feature unavailable

reactor_add_fd transient fail
→ RETRYING
```

不要所有失败都等价。

---

# 59. 不支持 XFRM 是可降级能力

某些目标环境：

```text
NETLINK_XFRM 不可用
权限受限
kernel capability 不支持
```

不应导致 ATPD 整体启动失败。

应：

```text
route monitor ACTIVE
xfrm DEGRADED/UNAVAILABLE
ATPD continues
```

除非产品明确要求 XFRM 是硬依赖。

---

# 60. Route Netlink 是更核心的能力

如果：

```text
g_async_fd route monitor
```

无法建立：

当前 `netlink_init()` 返回失败是合理的。

但需要上层明确：

```text
是否 daemon startup failure
```

不要 silently continue。

---

# 61. Test：XFRM registration failure

故障注入：

```text
socket success
bind success
reactor_add_fd fail
```

验证：

```text
registered == false
reactor == NULL
status != ACTIVE
无 fd leak
```

---

# 62. Test：set_reactor registration failure

已有：

```text
g_xfrm_fd
```

再：

```text
netlink_set_reactor
```

模拟 add_fd fail。

同样验证：

```text
不假标 registered
```

---

# 63. Test：event drain

一次制造多个 Netlink datagram。

单次 callback 后：

```text
socket 应 drain 到 EAGAIN
```

所有事件均被统计/处理。

---

# 64. Test：fragmented/malformed Netlink payload

构造：

```text
short nlmsghdr
short ifinfomsg
short xfrm_usersa_info
short XFRMA_IF_ID
bad rta_len
```

验证：

```text
ignore/error count
不 crash
不越界
```

---

# 65. Test：MSG_TRUNC

制造大 datagram或 mock：

```text
MSG_TRUNC
```

验证：

```text
不解析
truncated++
schedule resync
```

---

# 66. Test：NLMSG_OVERRUN / ENOBUFS

模拟：

```text
event loss
```

验证：

```text
state DEGRADED
refresh requested
```

随后 full refresh 成功：

```text
state 恢复 ACTIVE
```

---

# 67. Test：debounce coalescing

快速：

```text
100 network events
```

500 ms 内。

预期：

```text
不是 100 次 full refresh
```

而是：

```text
≈ 1 次最终 refresh
```

---

# 68. Test：debounce timer failure

故障注入：

```text
reactor_add_timer == NULL
```

验证：

```text
不会 silent loss
状态记录 DEGRADED / refresh pending
```

---

# 69. Test：cleanup during pending debounce

```text
schedule debounce
immediately cleanup
```

验证：

```text
timer cancelled
callback 不访问清理后的 Netlink state
无 UAF
```

---

# 70. Test：re-init

```text
init
cleanup
init
cleanup
```

重复例如：

```text
1000 次
```

检查：

```text
FD stable
RSS stable
state reset
```

---

# 71. Test：Netlink flap storm

结合已有 benchmark：

```text
dummy interface
add/delete/up/down
route add/delete
```

扩大到：

```text
200–1000 cycles
```

验证：

```text
reactor responsive
debounce working
no fd leak
no timer leak
VPN state eventually correct
```

---

# 72. Test：VPN transition scenarios

至少：

```text
no VPN
TUN appears
TUN disappears
WireGuard appears
IPsec XFRM NEWSA
IPsec XFRM DELSA
multiple SA delete/add
network flap during VPN
```

重点验收：

> 最终 observed state 正确，而不是要求每个中间 event 都精确映射。

---

# 73. Test：sync query timeout

模拟 kernel/no reply。

验证：

```text
bounded timeout
mutex released
下一次 query 可继续
```

不能：

```text
一次 timeout 后 sync fd 永久不可用
```

---

# 74. Test：sync query EINTR

在 wait/recv 期间触发 signal。

验证：

```text
继续等待剩余 deadline
```

不是立即失败。

---

# 75. Test：parallel sync query

如果项目确实允许跨线程：

```text
get_active_vpn
get_iface_stats
```

并发。

验证：

```text
sequence/reply 不串
mutex 正确
```

如果项目实际上 single-thread，则可省略此测试并在 API contract 中明确。

---

# 76. Sanitizer / fuzz

Host：

```text
ASan
UBSan
```

重点：

```text
Netlink parser
rtattr length
XFRM payload
cleanup/timer
```

建议对纯 parser 抽 helper 后加入 fuzz：

```text
arbitrary byte buffer
→ parser
→ never OOB/crash
```

---

# 77. 推荐提交顺序

## Commit 1

```text
netlink: make XFRM reactor registration truthful
```

内容：

- check `reactor_add_fd`
- shared registration helper
- only set registered on success
- cleanup state correction
- tests

---

## Commit 2

```text
netlink: drain asynchronous sockets correctly
```

内容：

- recv loop until EAGAIN
- EINTR
- recvmsg
- sender validation
- MSG_TRUNC

---

## Commit 3

```text
netlink: harden message parsing
```

内容：

- payload length checks
- rtattr length checks
- NLMSG_ERROR checks
- NLMSG_OVERRUN / ENOBUFS

---

## Commit 4

```text
netlink: harden debounce and resync lifecycle
```

内容：

- timer failure handling
- refresh pending/degraded state
- cleanup race tests

---

## Commit 5

```text
netlink: centralize synchronous transactions
```

内容：

- shared lock/send/recv helper
- EINTR
- monotonic deadline/poll
- multipart/single-reply semantics

---

## Commit 6

```text
netlink: expose monitor health snapshot
```

内容：

- route/XFRM state
- errors
- event counts
- overrun/truncation
- refresh stats

---

## Commit 7（可选）

```text
netlink: maintain lightweight link state cache
```

仅在测试证明同步 refresh 会明显阻塞 reactor 后做。

---

# 78. 是否拆 `netlink_xfrm.c`

当前不强制。

如果完成上述加固后：

```text
netlink.c > 800–900 LOC
```

或者 XFRM 加入：

```text
POLICY events
multiple SA tracking
recovery state machine
XFRM-specific telemetry
```

再拆：

```text
netlink.c
netlink_xfrm.c
netlink_internal.h
```

职责：

```text
netlink.c
    route/link/address
    sync query
    link snapshot

netlink_xfrm.c
    XFRM socket
    SA events
    XFRM state
```

现在不要为了 600 行机械拆。

---

# 79. Public `netlink.h` 清理

当前 public API 应重新审计：

```text
netlink_init(callback, userdata)
netlink_get_fd
netlink_handle_event
netlink_set_reactor
netlink_xfrm_init
netlink_xfrm_event_cb
```

理想状态：

外部只需要：

```c
int netlink_init(...);
int netlink_attach_reactor(...);
void netlink_cleanup(...);

int netlink_get_active_vpn(...);
int netlink_get_iface_stats(...);

int netlink_get_status(...);
```

内部 callback：

```text
netlink_handle_event
netlink_xfrm_event_cb
```

最好不要长期暴露为 public API。

---

# 80. 最终设计原则

该模块应遵循：

```text
Netlink event = change hint
full refresh/cache = observed truth

socket open != registered
registered != healthy

XFRM SA event != entire VPN lifecycle

parser never trusts payload length

event loss => resync
timer failure => observable degradation
```

---

# 81. 最终验收标准

## Registration

```text
reactor_add_fd failure
→ registered=false
→ status 不显示 ACTIVE
```

## Event loop

```text
Route/XFRM callback drain to EAGAIN
无 ET stuck event
```

## Parsing

```text
malformed/truncated message
→ no crash/OOB
```

## Event loss

```text
MSG_TRUNC / ENOBUFS / NLMSG_OVERRUN
→ DEGRADED
→ full resync
→ 可恢复 ACTIVE
```

## Timer

```text
debounce timer failure
→ no silent state loss
```

## Cleanup

```text
all fd/timer/state return baseline
g_atpd_ctx.xfrm_fd = -1
```

## Stress

```text
200–1000 network flap cycles
FD stable
RSS stable
timer stable
reactor responsive
```

## State correctness

```text
最终 VPN state 与系统真实状态一致
```

---

# 82. 最终结论

`netlink.c` 当前的问题不是“代码太长”，而是：

> 网络事件监听属于典型的 eventually-consistent subsystem，必须明确区分 socket、reactor registration、event hints、authoritative refresh 和 degraded recovery。

本轮不需要大拆。

先把以下几件事做正确：

```text
truthful registration
drain-to-EAGAIN
message bounds
overrun recovery
debounce ownership
sync transaction semantics
explicit health snapshot
```

完成后，`netlink.c` 会从“能检测网络变化”提升为：

> 可以在 Android/Linux 长期网络抖动、VPN 切换和事件丢失情况下自动恢复的网络状态监控模块。
