# ATPD `api.c / api.h` 控制边界与 Native API 解耦方案

## 1. 模块结论

当前：

```text
src/api.c      ~172 lines
include/api.h  ~31 lines
```

模块很小，不需要拆文件。

它本质上是：

```text
ATPD control-side wrapper
        ↓
singbox_api.c
```

所以它最应该保持：

```text
薄
无阻塞
无重复状态
无全局配置依赖
不伪装 async
```

而当前主要问题恰好集中在这些边界。

---

# 2. 当前职责

`api.c` 目前包含：

```text
singbox_api init/cleanup wrapper
health wrapper
Clash mode get/set
VPN state → Clash mode automation
version/status wrapper
goroutine count wrapper
```

这其中真正有 ATPD policy 意义的是：

```text
VPN state → Clash mode switching
```

其余大部分只是：

```text
singbox_api.c 的薄转发
```

---

# 3. 已确认问题：`*_async` 实际同步阻塞

例如：

```c
int api_check_health_async(...) {
    int ret = api_check_health_sync(ctx);
    ...
}
```

以及：

```c
int api_set_mode_async(...) {
    int ret = singbox_api_set_clash_mode(...);
    ...
}
```

所以：

```text
async
```

只是：

```text
同步调用完成后再 callback
```

并不异步。

---

# 4. 这不是命名小问题

如果 caller看到：

```text
api_check_health_async()
```

很自然会在 reactor callback里调用。

但它内部可能执行：

```text
connect
poll
HTTP/gRPC-Web
read
```

于是：

```text
reactor thread被阻塞
```

这是非常危险的 API contract。

---

# 5. 第一原则：不能叫假 async

二选一。

### 方案 A：改名为 sync

例如：

```c
api_check_health_sync()
api_set_mode_sync()
```

如果确实允许同步调用。

### 方案 B：实现真正异步

例如：

```text
reactor-driven transport
or
cached state + deferred command
```

当前阶段推荐：

> 不新增复杂异步网络层，优先让 `singbox_api` snapshot/cache 负责观察，控制命令做明确 bounded sync 或真正 reactor command。

---

# 6. `api_get_version_sync()` 最坏约阻塞 1.9 秒

当前逻辑：

```text
最多20次
每次失败后 sleep 100 ms
```

所以仅 sleep：

```text
19 × 100 ms = 1900 ms
```

还没算每次：

```text
singbox_api_get_version()
```

自身网络 timeout。

因此实际最坏可能远大于 1.9 秒。

---

# 7. `api_get_status_sync()` 同样

它也：

```text
20 attempts
100 ms retry delay
```

这正是前面 status review已经确认的问题来源。

如果：

```text
status_show()
→ api_get_status_sync()
```

Native API故障时 CLI/UDS可能卡很久。

---

# 8. 重试不应该藏在 getter

命名：

```text
get_status
get_version
```

应该意味着：

```text
快速读取
```

不应该偷偷执行：

```text
startup retry loop
```

---

# 9. Startup retry应该属于 readiness/service层

例如：

```text
service child started
↓
Native API readiness
↓
bounded startup state machine
```

而不是任何 caller执行：

```text
api_get_status_sync()
```

时都重新做20次 retry。

---

# 10. 推荐删除 getter内部 retry

最终：

```c
int api_get_status_snapshot(...);
int api_get_version_snapshot(...);
```

读取：

```text
singbox_api cached snapshot
```

立即返回。

如果 stale：

```text
返回 stale metadata
```

而不是现场发20个请求。

---

# 11. Version应该按 service generation缓存

sing-box version在：

```text
同一个 child generation
```

内不会变化。

因此：

```text
child generation N
→ GetVersion once
→ cache
```

完全不需要每次 status查询重新访问网络。

---

# 12. Status同样读 snapshot

`singbox_api.c` 方案已经建议：

```text
SubscribeStatus / bounded polling
↓
maintained snapshot
```

`api.c` 不应再加第二层 retry/cache。

---

# 13. `api_get_goroutines_count()` 是多余 wrapper

当前：

```text
get_goroutines
→ api_get_status_sync
→ remote query/retry
```

如果 status snapshot已有：

```text
goroutines
```

直接由 status collector读取 snapshot即可。

建议全仓审计 caller。

无独立 caller：

```text
删除
```

---

# 14. `base_url` 当前似乎是重复状态

`api_ctx_t` 保存：

```text
native_ctx.host
native_ctx.port
```

同时又保存：

```text
base_url
```

但 `api.c` 当前没有使用 `base_url` 做 transport。

如果全仓无 caller：

```text
删除
```

避免 endpoint双份状态。

---

# 15. `secret` 同样重复

`api_ctx_t`：

```text
native_ctx.secret
```

同时：

```text
ctx->secret
```

如果没有独立 consumer：

```text
删除 ctx->secret
```

否则：

```text
secret轮换/reload
```

很容易一份更新、一份没更新。

---

# 16. `timeout_sec` 也可能重复

`api_ctx_t.timeout_sec = 2`

而 `singbox_api_ctx_t` 自己已有：

```text
timeout_sec
```

如果 wrapper层没有自己执行 transport：

这个字段没有意义。

应删除。

---

# 17. `g_api_reactor` 当前实际上没被使用

`api_start_with_reactor()`：

```c
g_api_reactor = r;
```

但当前文件没有后续使用。

`api_cleanup()`：

```c
g_api_reactor = NULL;
```

说明：

```text
API “绑定 reactor”
```

目前只是日志/占位状态。

---

# 18. 假 reactor binding 应删除

如果 API transport当前仍是同步：

不要暴露：

```text
api_start_with_reactor()
```

让人误以为 API是 reactor-driven。

二选一：

```text
真正 attach reactor
```

或者：

```text
删掉 API
```

当前推荐删掉，直到真有 reactor transport。

---

# 19. `api_init()` 忽略 cfg NULL

当前：

```c
if (!ctx) return -1;
singbox_api_init(&ctx->native_ctx, cfg);
```

如果：

```text
cfg == NULL
```

行为完全取决于下层。

public wrapper应明确：

```text
ctx && cfg
```

否则 fail。

---

# 20. `singbox_api_init()` 返回值也应传播

当前：

```c
singbox_api_init(...);
...
return 0;
```

如果下层未来/当前可能失败：

API层却无条件成功。

这违反：

```text
init success = object usable
```

---

# 21. `api_cleanup()` 应幂等

当前主要调用：

```text
singbox_api_cleanup
g_api_reactor=NULL
```

需要确认：

```text
double cleanup
partial init cleanup
```

都安全。

如果 `api_ctx_t` 最终只是 native ctx wrapper：

甚至可以减少这个模块自己的 lifecycle。

---

# 22. 最大结构问题：`api.c` 直接读 `g_config`

VPN callback：

```c
if (!g_config.interface.vpn_auto_mode)
...
g_config.interface.vpn_target_mode
g_config.interface.vpn_fallback_mode
```

这让 API policy层直接依赖：

```text
global mutable config
```

---

# 23. 为什么这和 transactional reload 冲突

reload transaction希望：

```text
old config
candidate config
commit point
```

但 callback随时：

```text
直接读取 g_config
```

如果字段正在 reload：

可能看到：

```text
不一致/旧新混合 semantics
```

尤其以后 config struct不再裸暴露时。

---

# 24. 推荐把 policy config copy进 `api_ctx`

例如：

```c
typedef struct {
    bool vpn_auto_mode;
    char target_mode[SINGBOX_CLASH_MODE_SIZE];
    char fallback_mode[SINGBOX_CLASH_MODE_SIZE];
} api_vpn_policy_t;
```

由：

```text
config transaction commit
```

一次性更新。

callback只读：

```text
ctx->vpn_policy
```

---

# 25. 更好的做法是 policy owner独立于 transport wrapper

当前 `api.c` 同时：

```text
Native API wrapper
+
VPN policy
```

文件虽小，但职责不同。

暂时不拆文件也可以。

长期如果 policy增长：

```text
vpn_mode_controller.c
```

会比继续把业务规则放 API transport wrapper更清楚。

但现在不用提前拆。

---

# 26. `api_vpn_mode_callback()` 当前同步访问 Native API

它先：

```c
singbox_api_get_clash_mode_status(...)
```

然后可能：

```c
singbox_api_set_clash_mode(...)
```

这都是网络操作。

而这个 callback是由：

```text
VPN state transition
```

触发的。

如果 transition来自 reactor callback：

就会阻塞 reactor。

---

# 27. 这是 P0/P1 reactor latency风险

典型：

```text
Netlink/XFRM event
↓
VPN state transition
↓
api_vpn_mode_callback
↓
TCP connect/poll/read
↓
reactor卡住
```

网络异常时尤其明显。

---

# 28. VPN callback必须变成“请求”，而不是现场RPC

推荐：

```text
VPN snapshot update
↓
mode controller records desired_mode
↓
schedule/defer reconcile
↓
Native API controller applies
```

callback本身只做：

```text
update desired state
wake/schedule
return
```

---

# 29. 最简单实现可以不用线程池

例如 reactor有：

```text
deferred task / timer
```

则：

```text
callback
→ mark mode_sync_pending
→ 0ms/next-tick timer
```

但如果 deferred task里面仍做同步网络：

还是阻塞。

所以真正 transport仍需：

```text
bounded nonblocking
```

或：

```text
singbox_api已有 maintained connection/snapshot
```

---

# 30. 第一阶段最低成本做法

如果暂时还没重构 transport：

至少不要在高频 state callback里做多次 RPC。

可改成：

```text
callback记录 desired_mode
```

由：

```text
已有低频 health/update task
```

统一 reconcile。

仍需严格 deadline。

---

# 31. `default_mode` 其实是 policy state

当前用于：

```text
VPN READY前保存当前 Clash mode
VPN IDLE时恢复
```

这个逻辑是有意义的。

但字段名字更准确应类似：

```text
pre_vpn_mode
```

---

# 32. `default_mode` 可能长期 stale

时序：

```text
VPN READY
→ 保存 mode A
→ 切 target
→ API失败/daemon reload/policy变化
→ 某些 transition没走 IDLE
```

`default_mode` 可能保留。

下次 VPN lifecycle：

```text
if !default_mode[0]
```

就不会重新 snapshot当前 mode。

---

# 33. 应绑定 VPN generation

推荐：

```c
uint64_t vpn_generation;
bool restore_mode_valid;
char restore_mode[];
```

只有：

```text
进入新的 READY edge
```

时保存一次。

退出 cycle后无论：

```text
restore成功
restore失败
teardown/cancel
```

都要明确处理 valid flag。

---

# 34. `TEARDOWN` 当前直接忽略

callback：

```text
PREDICTING / TEARDOWN
→ return
```

然后只在后续：

```text
IDLE
```

恢复 mode。

如果系统永远没产生稳定 IDLE：

```text
restore_mode
```

可能一直保留。

这需要和 Netlink FSM一起测试。

---

# 35. 不要在 context state callback里隐藏复杂副作用

我们的 context方案建议：

```text
context commit snapshot
→ notify
```

observer最好：

```text
fast / nonblocking
```

当前 VPN callback违反这个原则。

---

# 36. `is_clash_mode_supported()` 是合理纯 helper

保留即可。

如果 modes list可能：

```text
mode_count > array capacity
```

应由 `singbox_api` parser保证。

API层不需重复 defensive clamp，除非 struct contract不清。

---

# 37. target/fallback string ownership

当前每次从：

```text
g_config
```

取 pointer。

改成 ctx-owned policy后：

```text
reload commit
→ bounded copy
```

更安全。

---

# 38. Clash mode“不存在”不一定是 ERROR

如果配置 target mode在当前 sing-box config中不存在：

这是：

```text
configuration/policy mismatch
```

应：

```text
WARN + last policy error
```

不应该让 daemon失败。

---

# 39. fallback也不存在

当前日志后 return。

合理。

但 status应能看到：

```text
VPN auto-mode degraded
last reason: target/fallback mode unavailable
```

而不是只在日志里。

---

# 40. Mode reconcile最好幂等

理想：

```text
desired_mode == observed_mode
→ no RPC
```

当前已经做了这个判断。

保留。

---

# 41. 但 observed mode应来自 cache

当前每次：

```text
GetClashModeStatus RPC
```

长期推荐：

```text
singbox_api snapshot.clash_mode
```

先读 cache。

只有控制命令真正需要时才发：

```text
SetClashMode
```

---

# 42. Set后应等待/观察 authoritative update

不要仅因为：

```text
SetClashMode RPC return 0
```

就永远认为 mode已切换。

snapshot下一次更新应确认：

```text
observed == desired
```

---

# 43. 所以 mode controller应有状态

例如：

```text
IDLE
PENDING
APPLYING
SYNCED
DEGRADED
```

但如果当前需求简单：

不必单独 enum。

至少有：

```text
desired_mode
last_apply_result
last_apply_at
```

---

# 44. Callback `code/body` 模拟 HTTP语义不太合适

`api_callback_t`：

```c
(int code, const char *body, void *userdata)
```

然后：

```text
200
503
500
```

但这里不是 HTTP server接口。

这是内部 C API。

---

# 45. 内部 API 应使用 typed result

例如：

```c
typedef enum {
    API_OK = 0,
    API_UNAVAILABLE,
    API_TIMEOUT,
    API_INVALID_ARGUMENT,
    API_REMOTE_ERROR,
    API_INTERNAL_ERROR
} api_result_t;
```

而不是：

```text
HTTP 200/500/503
```

---

# 46. 如果 UDS/HTTP renderer需要 HTTP code

应该在：

```text
adapter/render layer
```

做映射。

不要让 core control API携带 transport-specific status code。

---

# 47. `body = "OK"` / `"Failed"` 信息量也太低

真正需要：

```text
last_errno
grpc status
reason
```

由下层 snapshot/error structure提供。

不要把错误压成：

```text
500 Failed
```

---

# 48. `api_check_health_sync()` 只是转发

如果没有独立 policy：

caller可以直接调用：

```text
singbox_api_health...
```

但为了 boundary稳定可以保留。

关键是：

```text
API层要增加真实语义
```

否则这层只是重复。

---

# 49. 推荐重新定义 API层角色

`api.c` 应该是：

> ATPD 对 sing-box control capabilities 的稳定内部 facade。

这样未来：

```text
singbox gRPC path
message details
transport implementation
```

变化，

上层：

```text
UDS
status
VPN policy
service
```

不用跟着改。

---

# 50. 所以可以保留 facade，但要去掉重复 transport state

`api_ctx_t` 最终类似：

```c
typedef struct {
    singbox_api_ctx_t native;

    api_vpn_policy_t vpn_policy;

    char restore_mode[SINGBOX_CLASH_MODE_SIZE];
    bool restore_mode_valid;

    char desired_mode[SINGBOX_CLASH_MODE_SIZE];

    api_policy_status_t policy_status;
} api_ctx_t;
```

不再保留：

```text
base_url
secret duplicate
timeout duplicate
fake reactor pointer
```

---

# 51. `api_start_with_reactor()` 如果无真实作用就删除

配合 init方案：

当前 API phase：

```text
api_init
api_start_with_reactor
```

给人一种：

```text
API依赖 reactor
```

的错觉。

如果实际没有：

删除 attach phase。

---

# 52. 如果将来 Native API transport变 reactor-driven

那时再增加：

```text
singbox_api_attach_reactor()
```

并由真正 transport owner持有 reactor。

不要由 facade层持一个未使用 global pointer。

---

# 53. `g_api_reactor` 是第二个 global singleton

同时整个程序已经有：

```text
g_reactor
```

再存：

```text
g_api_reactor
```

会造成 duplicated lifecycle。

直接删除更好。

---

# 54. `api.c` include `atpd_global.h` 主要是为了 `g_config`

等 policy copy完成后：

```text
api.c
```

应该能删除：

```c
#include "atpd_global.h"
```

这是很好的验收指标。

---

# 55. `api.h` 也不应依赖 `atpd_context.h`

当前只是为了：

```text
vpn_state_t
```

如果 context提供：

```text
atpd_vpn_snapshot_t
```

callback signature可能放在 controller层。

长期尽量减少 header横向 include。

---

# 56. Header依赖建议

最终：

```text
api.h
├─ atp.h/config types if needed
└─ singbox_api.h (or opaque)
```

甚至可以把 native ctx opaque化。

当前不用一次做到。

---

# 57. Secret安全

虽然当前日志只打印：

```text
host:port
```

没有 secret。

这很好。

继续保持：

```text
任何 error/status/debug
```

都不输出 secret。

---

# 58. Reload secret更新

当前：

```text
api_init()
```

可能被 reload路径直接再次调用。

如果 ctx active：

```text
memset(ctx,0)
```

会抹掉：

```text
default_mode
policy runtime state
native transport state
```

这和 transactional reload冲突。

---

# 59. 不允许 active ctx上直接 `api_init()` 重置

应该提供：

```c
api_prepare_config(...)
api_commit_config(...)
```

或简单：

```c
api_update_config(...)
```

明确哪些字段热更新。

不要：

```text
reload
→ api_init(active_ctx)
```

---

# 60. 这是与 init/config方案的重要联动

Startup：

```text
api_init
```

一次。

Reload：

```text
api_apply_config_delta
```

不是 re-init。

---

# 61. API endpoint改变

例如：

```text
host
port
secret
```

需要：

```text
singbox_api reconfigure
snapshot invalidation
reconnect generation
```

由 `singbox_api` owner实现。

facade只提交 new endpoint。

---

# 62. VPN policy改变

例如：

```text
vpn_auto_mode
target_mode
fallback_mode
```

只更新：

```text
api_ctx.policy
```

不需要重建 transport。

所以 config delta应分：

```text
transport config
policy config
```

---

# 63. Disable vpn_auto_mode

如果当前正处于：

```text
VPN READY
```

并已切 target mode，

reload把：

```text
vpn_auto_mode=false
```

需要明确：

```text
是否立即恢复 pre-VPN mode
```

还是：

```text
停止未来自动切换，但保持当前
```

必须定义。

---

# 64. 推荐语义

用户关闭 auto mode：

```text
不再主动管理 Clash mode
```

为了减少意外副作用：

建议：

```text
立即停止 reconcile
clear restore state
不主动再切一次
```

除非产品明确要求 restore。

---

# 65. Target mode reload during active VPN

如果：

```text
VPN READY
target A → target B
```

合理行为：

```text
desired_mode=B
schedule reconcile
```

不需要等待下一次 VPN transition。

这个应该由 config commit主动触发。

---

# 66. Fallback mode reload

只影响：

```text
restore fallback
```

不必立即执行。

---

# 67. Test：假 async API不能残留

最终搜索：

```text
_async
```

任何 API：

```text
必须真正 deferred/nonblocking
```

否则改名。

---

# 68. Test：status读取无网络

instrument：

```text
5000 status queries
```

期望：

```text
0 new Native API sockets
```

或近似0，仅后台维护连接。

不能每次 status：

```text
connect
```

---

# 69. Test：Native API down

```text
status
version
goroutines
```

都必须：

```text
立即返回 stale/unavailable snapshot
```

不能 sleep 1.9s+。

---

# 70. Test：VPN transition callback latency

Native API完全不可达。

触发：

```text
VPN READY
```

callback本身应在：

```text
<1 ms / bounded local work
```

返回。

不能等待 TCP timeout。

---

# 71. Test：VPN READY target already active

预期：

```text
0 SetClashMode
```

---

# 72. Test：VPN READY save restore mode

```text
observed Rule
→ VPN READY
→ target Google VPN
```

保存：

```text
restore=Rule
```

只保存一次 per VPN generation。

---

# 73. Test：repeated READY

```text
READY
READY
READY
```

不能覆盖原：

```text
restore mode
```

为 target mode。

否则 IDLE恢复时会恢复错。

---

# 74. Test：READY → IDLE

确认：

```text
restore
clear restore_valid
```

---

# 75. Test：restore mode已不存在

fallback存在：

```text
restore fallback
```

当前逻辑已有。

继续测试。

---

# 76. Test：restore和fallback都不存在

应：

```text
degraded
no invalid SetClashMode
```

---

# 77. Test：target不存在

应：

```text
no set
policy degraded
```

---

# 78. Test：Native API unavailable during READY

应：

```text
desired mode保留
```

如果设计有 reconcile：

API恢复后再尝试。

不要丢掉 state transition意图。

---

# 79. 这比当前实现更可靠

当前：

```text
READY event
↓
API unavailable
↓
return
```

以后 API恢复：

如果没有新的 VPN event，

就永远不会再切 target mode。

这是实际功能缺陷。

---

# 80. 推荐 desired-state reconcile

保存：

```text
desired_mode
desired_generation
```

Native API health恢复时：

```text
if desired != observed
→ reconcile
```

这样不依赖：

```text
再来一个 Netlink event
```

---

# 81. 同理 restore也应该是 desired state

IDLE时：

```text
desired = restore/fallback
```

如果 API暂时 down：

意图仍保留。

恢复后 apply。

---

# 82. 需要避免 stale desired mode

VPN generation改变时：

旧 desired必须被新 generation覆盖。

例如：

```text
READY target pending
↓
VPN变 IDLE
↓
旧 target RPC晚到
```

如果有真正 async控制：

必须有：

```text
generation check
```

---

# 83. 所以未来 async SetMode callback需要 request id

例如：

```text
policy_generation
request_generation
```

completion只更新：

```text
当前 generation
```

否则忽略 stale completion。

---

# 84. 当前同步 SetMode没有这个 race

但代价是阻塞 reactor。

一旦真正异步化：

generation必须一起加。

---

# 85. Test：API恢复 reconcile

```text
READY
API down
target not applied
API becomes healthy
```

预期：

```text
target eventually applied
```

无需新的 VPN event。

---

# 86. Test：stale generation

```text
READY target request A
↓
IDLE desired Rule
↓
A completion late
```

最终：

```text
desired/observed state不能被 A重新标成 synced
```

---

# 87. Test：reload endpoint

运行中更新：

```text
host/port/secret
```

验证：

```text
old snapshot invalidated
new connection established
no memset active api_ctx
```

---

# 88. Test：reload VPN policy

```text
target mode A → B
```

VPN active时：

```text
desired becomes B
```

---

# 89. Test：secret redaction

错误日志/status：

```text
never contains secret
```

---

# 90. Test：api_init failure propagation

mock：

```text
singbox_api_init fail
```

期望：

```text
api_init fail
```

而不是无条件0。

---

# 91. Test：double cleanup

```text
api_cleanup
api_cleanup
```

安全。

---

# 92. Test：no duplicate endpoint state

完成后检查：

```text
base_url
ctx->secret
ctx->timeout_sec
g_api_reactor
```

如果无 owner语义：

全部不存在。

---

# 93. 推荐 Commit 1

```text
api: remove blocking retry from status and version getters
```

内容：

- 删除20×100ms retry
- 改 snapshot getter
- status latency tests

---

# 94. Commit 2

```text
api: remove fake async and unused reactor binding
```

内容：

- async命名修正/真实实现
- 删除 g_api_reactor
- 删除 no-op api_start_with_reactor

---

# 95. Commit 3

```text
api: remove duplicated native transport state
```

删除：

```text
base_url
duplicate secret
duplicate timeout
```

前提：全仓 caller审计。

---

# 96. Commit 4

```text
api: isolate vpn mode policy from global config
```

内容：

- api_vpn_policy
- 不再读 g_config
- reload policy apply API

---

# 97. Commit 5

```text
api: make vpn mode synchronization desired-state driven
```

内容：

- desired mode
- restore mode generation
- API unavailable后恢复 reconcile
- no blocking in VPN callback

---

# 98. Commit 6

```text
api: replace transport-style callback codes with typed results
```

可稍后做。

如果全仓影响较大：

不是第一阶段必须。

---

# 99. Codex修改前必须审计 caller

搜索：

```text
api_init(
api_cleanup(
api_start_with_reactor(
api_check_health_async(
api_check_health_sync(
api_get_mode_sync(
api_set_mode_async(
api_get_version_sync(
api_get_status_sync(
api_get_goroutines_count(
api_vpn_mode_callback(
```

输出：

```text
caller
thread/reactor context
may block?
expected callback semantics
replacement
```

---

# 100. 还必须审计 `g_api_ctx`

搜索：

```text
g_api_ctx
```

确认有没有模块：

```text
直接访问 native_ctx
default_mode
secret
base_url
```

不要只改 api.c 自己。

---

# 101. `atpd_global` 联动

当前：

```c
#define g_api_ctx g_atpd.api_ctx
```

这意味着 api context也是全局裸对象。

后续 `atpd_global.c` review时应该进一步收缩。

但本轮不用一次消灭。

---

# 102. 一个重要验收目标

完成 policy copy后：

```text
src/api.c
```

应该不再：

```c
#include "atpd_global.h"
```

也不再读取：

```text
g_config
```

这是非常明确的边界改善。

---

# 103. 与 `singbox_api.c` 方案联动

`api.c` 不要自己重新实现：

```text
retry
cache
freshness
transport timeout
grpc status
```

这些全部属于：

```text
singbox_api
```

---

# 104. 与 `status.c` 方案联动

status：

```text
singbox_api snapshot
```

不是：

```text
api_get_status_sync
```

所以后者可能最终完全删除。

---

# 105. 与 `context.c` 方案联动

VPN callback必须：

```text
fast
nonblocking
snapshot-driven
```

不能在 context transition中现场做远程 I/O。

---

# 106. 与 `config.c` 方案联动

reload：

```text
prepare api transport config
prepare vpn policy
commit
```

不能：

```text
api_init(active_ctx)
```

重新 memset。

---

# 107. 与 init/shutdown方案联动

如果 API不再真实依赖 reactor：

startup phase不需要：

```text
api_start_with_reactor
```

减少 dependency。

cleanup也更简单。

---

# 108. 与 service方案联动

service generation change时：

```text
singbox_api snapshot invalid
```

由 Native API owner处理。

api facade只看到：

```text
UNAVAILABLE/STALE
```

不负责 child lifecycle。

---

# 109. 不建议拆 `api.c`

172行太小。

即使加：

```text
VPN desired-state policy
```

也可以控制在：

```text
200–300 LOC
```

如果未来 mode policy继续扩大：

再拆：

```text
vpn_mode_controller.c
```

现在不需要。

---

# 110. 最终 Invariants

Codex最终应保证：

```text
I1:
no function named async performs synchronous blocking RPC

I2:
status/version getters never contain startup retry sleeps

I3:
api.c never performs long remote I/O from VPN/context callbacks

I4:
api.c does not read mutable global config directly

I5:
Native API transport state has one owner: singbox_api

I6:
API endpoint/secret/timeout are not duplicated across wrapper structs

I7:
VPN mode policy retains desired state while Native API is temporarily unavailable

I8:
restore mode is scoped to one VPN lifecycle generation

I9:
reload does not reinitialize/memset an active api_ctx

I10:
internal API errors are not falsely represented as generic HTTP codes unless crossing an HTTP adapter
```

---

# 111. 最终验收标准

## Reactor responsiveness

Native API down：

```text
VPN transition callback
status
```

不阻塞 reactor。

## Status

```text
5000 status queries
```

不产生5000个 Native API同步连接。

## Retry

源码中：

```text
api_get_status/version
```

无：

```text
usleep retry loop
```

## Policy

API down期间：

```text
VPN READY desired target
```

恢复后能自动 reconcile。

## Global dependency

`api.c`：

```text
no g_config
```

## Reload

endpoint/policy变化：

```text
atomic config apply
no active ctx memset
```

---

# 112. 最终结论

`api.c` 当前不需要重写，也不需要拆文件。

它的问题主要是控制边界不够严格：

```text
假 async
同步 retry
global config dependency
VPN callback blocking RPC
duplicated transport fields
unused reactor binding
```

最重要的方向是：

> 让 `api.c` 成为一个真正薄而稳定的 ATPD→sing-box control facade，而不是第二个 transport layer。

最终：

```text
status/read path
→ snapshot only

VPN policy
→ desired state

singbox_api
→ transport + freshness + remote errors

config
→ policy/endpoint commit

service
→ child lifecycle
```

这样 API层会比现在更小、更清楚，而且能彻底解决 status/VPN event路径因为 Native API故障而拖慢整个 reactor 的问题。
