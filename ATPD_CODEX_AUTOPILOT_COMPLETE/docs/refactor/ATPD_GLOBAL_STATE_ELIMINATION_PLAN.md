# ATPD `atpd_global.c / atpd_global.h` 全局状态收敛方案

## 1. 模块结论

当前：

```text
src/atpd_global.c      9 lines
include/atpd_global.h  29 lines
```

源码极小，但它的架构影响很大。

当前 `atpd_global_t` 保存：

```c
atp_config_t config;
api_ctx_t api_ctx;
reactor_t *reactor;
service_ctx_t *svc;

volatile sig_atomic_t running;
volatile sig_atomic_t reload;
volatile sig_atomic_t show_status;
```

然后通过宏暴露：

```c
#define g_config      g_atpd.config
#define g_api_ctx     g_atpd.api_ctx
#define g_reactor     g_atpd.reactor
#define g_svc         g_atpd.svc
#define g_running     g_atpd.running
#define g_reload      g_atpd.reload
#define g_show_status g_atpd.show_status
```

因此真正的问题不是代码量，而是：

> ATPD 用一个全局 struct + 一组宏，把多个 subsystem ownership 和 runtime command flags 隐藏成了普通变量。

这会直接弱化前面已经建立的：

```text
config ownership
service ownership
reactor ownership
API ownership
runtime state machine
```

---

# 2. 当前还有一个明确问题：`main.c` 又重复定义了一遍这些宏

`atpd_global.h` 已经定义：

```text
g_config
g_api_ctx
g_reactor
g_svc
g_running
g_reload
g_show_status
```

但 `main.c` 顶部又重复：

```c
#define g_config g_atpd.config
#define g_api_ctx g_atpd.api_ctx
#define g_reactor g_atpd.reactor
#define g_svc g_atpd.svc
#define g_running g_atpd.running
#define g_reload g_atpd.reload
#define g_show_status g_atpd.show_status
```

这说明这组宏已经开始成为：

```text
implicit global ABI
```

而不是清晰的 ownership boundary。

第一步就应该删除重复宏定义。

---

# 3. 宏 alias 会隐藏真实依赖

调用点：

```c
config_reload(&g_config);
```

表面看：

```text
g_config 是一个独立全局 config
```

实际上是：

```text
g_atpd.config
```

同理：

```text
g_svc
g_reactor
g_api_ctx
```

都是一个 root struct里的字段。

这会让：

```text
grep dependency
code review
ownership audit
```

更难。

---

# 4. 首要原则：全局变量要么显式，要么消失

不要继续：

```text
g_atpd.field
↓
#define g_field g_atpd.field
```

二选一：

### 方案 A

显式使用：

```text
g_atpd.xxx
```

让所有 global access visible。

### 方案 B

让 subsystem拥有自己的 state/API，

最终：

```text
删除这些字段
```

本项目推荐 B。

---

# 5. `config` 不应该由 `atpd_global` 持有

我们已经确定 config未来需要：

```text
transactional reload
generation
source path
explicit presence
last reload status
serialization
```

因此：

```text
atp_config_t config
```

应该由：

```text
config subsystem
```

管理。

---

# 6. 为什么不能继续裸暴露 `g_config`

因为任何文件都能：

```c
g_config.foo = ...
```

这样 config transaction的：

```text
prepare
commit
rollback
generation
snapshot consistency
```

全部可以被绕过。

最终应该只允许：

```text
config_get_snapshot()
config_reload_transaction()
config_apply...
```

---

# 7. `g_config` 是 transactional config 的最大后门

只要这个 macro存在：

```text
atomic reload
```

就无法真正 enforce。

因此它应该最终被删除。

---

# 8. 第一阶段不一定立刻把 config private化

先全仓：

```text
grep g_config
```

分类：

```text
read-only
mutation
startup init
reload apply
status
path lookup
policy lookup
```

然后逐步迁移。

---

# 9. `api_ctx` 也不应该由 root global struct拥有

当前：

```text
api_ctx_t api_ctx;
```

本身是 embedded object。

但 API subsystem有自己的：

```text
init
cleanup
policy state
native API transport
```

它应该成为：

```text
API owner state
```

而不是让所有模块：

```text
&g_api_ctx
```

直接访问。

---

# 10. 尤其当前 `api.c` 自己还直接 include `atpd_global.h`

为了读取：

```text
g_config
```

这形成循环式耦合：

```text
global
→ api_ctx type

api.c
→ global config
```

这不是清晰分层。

---

# 11. API方案完成后的目标

`api.c`：

```text
不 include atpd_global.h
不读 g_config
```

API policy通过：

```text
config commit
```

显式传入。

此后：

```text
g_api_ctx
```

也没有必要作为全局 macro。

---

# 12. `reactor` 不应该藏在 generic global container

`reactor_t *reactor`
真正 owner应该是：

```text
daemon runtime / main lifecycle
```

这个 pointer是否可以保留为 root runtime state，要看最终设计。

但不应通过：

```text
g_reactor
```

让所有 subsystem随时拿。

---

# 13. 更推荐依赖在 init 时显式传入

例如：

```c
service_attach_reactor(service, reactor);
netlink_attach_reactor(reactor);
uds_init(reactor, ...);
```

模块保存自己需要的引用。

之后不再：

```text
到 g_atpd 里找 reactor
```

---

# 14. `g_reactor` 会把 root object变成 service locator

任何新模块以后都很容易：

```c
#include "atpd_global.h"
reactor_add_fd(g_reactor,...)
```

这绕过：

```text
init dependency
shutdown ordering
test injection
```

应该禁止继续扩张。

---

# 15. `service_ctx_t *svc` 同样

service是独立 subsystem owner。

main/daemon lifecycle可以持：

```text
service pointer
```

但 status/UDS/API不要通过 global macro直接拿。

应该：

```text
显式参数
or
service_get_snapshot()
```

---

# 16. 当前 `main.c` 大量直接操作 `g_svc`

例如：

```text
service_apply_config
service_sigchld_cb
status_show(...g_svc...)
service_stop_sync(g_svc)
```

而且 `main.c` 还直接改 service internals。

这些都已经在 service/init方案里要求收敛。

完成后：

```text
service pointer只由 daemon lifecycle层持有
```

即可。

---

# 17. Signal flags 是目前最合理保留的全局状态

```c
volatile sig_atomic_t running;
volatile sig_atomic_t reload;
volatile sig_atomic_t show_status;
```

它们用于：

```text
signal callback
→ main/reactor event loop
```

这类字段确实有：

```text
process-global control intent
```

的属性。

---

# 18. 但字段语义仍可更清晰

当前：

```text
running
reload
show_status
```

更准确可以是：

```text
shutdown_requested
reload_requested
status_requested
```

因为：

```text
running=0
```

其实代表：

```text
request shutdown
```

不等于 daemon资源已经停止。

---

# 19. 这和 runtime state方案必须一致

我们已经指出：

```text
STOPPED不能在真正 cleanup前设置
```

所以 signal flag应该表达：

```text
intent/request
```

runtime state表达：

```text
actual lifecycle
```

不要混用。

---

# 20. 推荐 signal flags

```c
typedef struct {
    volatile sig_atomic_t shutdown_requested;
    volatile sig_atomic_t reload_requested;
    volatile sig_atomic_t status_requested;
} atpd_signal_requests_t;
```

这三个可以继续全局。

---

# 21. 是否需要整个 `atpd_global_t`

如果最终只剩：

```text
3个 signal request flags
```

那 `atpd_global_t` 已经没有存在必要。

可以直接：

```c
static volatile sig_atomic_t ...
```

放在：

```text
main.c / runtime.c
```

---

# 22. 最终可能直接删除 `atpd_global.c/h`

这是本轮最推荐的长期结果。

不是把它改得更复杂。

而是：

```text
config → config owner
api → api owner
reactor → daemon runtime owner
service → service owner
signal flags → main/runtime
```

全部迁走后：

```text
atpd_global
```

自然为空。

---

# 23. 这和 `atpd_context` 不冲突

不要把：

```text
atpd_global
```

里的东西全部搬去：

```text
atpd_context
```

那只是：

```text
换一个万能 global struct
```

没有解决问题。

---

# 24. 两个模块的职责应该完全不同

`atpd_context`：

```text
daemon global observed lifecycle state
VPN high-level snapshot
uptime
```

`atpd_global`：

理想状态：

```text
不存在
```

---

# 25. 绝对不要做

不要未来：

```c
typedef struct {
    atpd_context_t context;
    atp_config_t config;
    api_ctx_t api;
    reactor_t *reactor;
    service_ctx_t *service;
    netlink_ctx_t netlink;
    session_manager_t sessions;
    ...
} atpd_global_t;
```

这会变成：

```text
god object / service locator
```

---

# 26. Header include耦合很明显

当前 `atpd_global.h` include：

```c
#include "atp.h"
#include "api.h"
#include "reactor.h"
#include "service.h"
```

一个只有29行的 header，却把四个大型模块类型传播给所有 include它的文件。

这会导致：

```text
build dependency扩大
header coupling
circular include风险
```

---

# 27. 删除 embedded object可以减少 include

如果暂时保留 root struct：

至少可以 forward declare：

```c
typedef struct reactor reactor_t;
typedef struct service_ctx service_ctx_t;
```

但：

```text
atp_config_t
api_ctx_t
```

是 embedded value，需要完整类型。

这进一步说明：

> embedded config/api 不适合放这里。

---

# 28. 最终删除 `atpd_global.h` 会显著改善 include graph

很多模块将不再因为：

```text
想拿 g_config
```

就间接 include：

```text
api
service
reactor
```

---

# 29. 当前 `main.c` 重复 macro 是一个很好的 code smell

既然 header已经提供：

```text
#define g_config...
```

main还重复定义一遍，

说明：

```text
global alias体系缺乏单一边界
```

第一阶段至少先删重复定义。

---

# 30. 不要只是把 macro换成 inline getter

例如：

```c
atp_config_t *atpd_global_get_config();
service_ctx_t *atpd_global_get_service();
```

这只是：

```text
service locator from macros
→ service locator from getters
```

没有改善 ownership。

---

# 31. Getter只有在真正 root state时才合理

例如：

```text
atpd_runtime_get_state()
atpd_runtime_get_uptime()
```

这些属于真实 daemon-global state。

而：

```text
get_service_ptr
get_reactor_ptr
get_config_mutable
```

不应该存在。

---

# 32. Config read-only访问怎么处理

如果大量模块需要配置：

不要每个地方都：

```text
config_get_global()
```

更好的模式是：

```text
init/apply时copy module-owned config subset
```

例如：

```text
service owns service config
api owns API/policy config
netlink owns netlink settings
UDS owns socket config
```

---

# 33. 这样 reload也更清楚

config transaction：

```text
candidate
↓
diff
↓
service_prepare
api_prepare
netlink_prepare
...
↓
commit
```

每个 module拿自己那部分。

不需要运行时到：

```text
g_config
```

查询。

---

# 34. Global config snapshot仍可存在

为了：

```text
status
save
reload comparison
```

config subsystem内部当然可以保留 authoritative snapshot。

但它应该：

```text
private
```

不是：

```text
extern mutable struct
```

---

# 35. API state同理

API subsystem可以有 singleton：

```text
static api_ctx_t g_api;
```

如果项目明确采用 subsystem singleton。

但不应再：

```text
嵌套进 atpd_global
```

然后通过 macro访问。

---

# 36. 更推荐 daemon-owned explicit pointer

由于项目规模不大：

```text
main/daemon_state
```

可以持：

```text
reactor*
service*
api*
```

并把指针显式传给需要的 orchestration函数。

这和：

```text
全局任意 access
```

不同。

---

# 37. 可以引入一个很小的 `daemon_state_t`

但要非常克制。

例如只用于：

```text
main.c runtime orchestration
```

```c
typedef struct {
    reactor_t *reactor;
    service_ctx_t *service;
    api_ctx_t api;
} daemon_state_t;
```

而且：

```text
static in main/runtime.c
```

不放 public header。

---

# 38. 这比 `atpd_global_t` 好在哪里

因为：

```text
scope仅限 lifecycle/orchestration implementation
```

不是：

```text
extern global public ABI
```

其他 module拿不到。

---

# 39. 但如果 api ctx可以由 api subsystem自己拥有

甚至：

```text
daemon_state
```

只需要：

```text
reactor
service
```

越小越好。

---

# 40. signal flags可以单独 static

```text
static volatile sig_atomic_t shutdown_requested;
```

signal handler和event loop都在：

```text
main/runtime.c
```

就完全不需要 public global。

---

# 41. 当前 `g_show_status`

SIGUSR1：

```text
show status
```

然后 on_idle：

```text
status_show(&g_config, g_svc, &g_api_ctx)
```

未来 status snapshot重构以后：

```text
status_show()
```

不应该需要这三个 mutable global对象。

---

# 42. 这会进一步消除 global dependency

最终：

```text
SIGUSR1
→ request status
→ status_collect()
```

collector从各 owner snapshot读取。

main不再拼：

```text
config/service/api pointers
```

---

# 43. `g_reload`

当前 reload：

```text
config_reload(&g_config)
service_apply_config(g_svc,&g_config)
api_init(&g_api_ctx,&g_config)
```

完全依赖 global bundle。

transactional config完成后应变成：

```text
config_reload_transaction()
```

main不需要知道：

```text
g_config
g_svc
g_api_ctx
```

---

# 44. 这说明 `atpd_global` 的主要调用场景会自然消失

前面几个模块方案完成后：

```text
status
reload
service
api
reactor
```

都会各自有清晰边界。

因此不要现在过度设计一套新的 global manager。

---

# 45. 推荐迁移顺序非常重要

不要第一步就删除：

```text
atpd_global.h
```

否则会产生一次巨大机械改动。

正确顺序：

```text
1. 删除重复 macro
2. 建立 global usage map
3. config private化
4. api脱离 g_config
5. service pointer ownership收敛
6. reactor pointer ownership收敛
7. signal flags迁回 runtime/main
8. 删除 atpd_global
```

---

# 46. Codex 第一件事：全仓 global usage map

搜索：

```text
g_atpd
g_config
g_api_ctx
g_reactor
g_svc
g_running
g_reload
g_show_status
atpd_global.h
```

输出表：

```text
file
symbol
read/write
purpose
owner after refactor
replacement API
```

---

# 47. 特别区分 read 与 write

例如：

```text
status read config
```

和：

```text
main reload modifies config
```

迁移方式不同。

不要机械：

```text
全部变 getter/setter
```

---

# 48. 每个 write 必须重点审计

因为：

```text
write global
```

意味着绕过 owner lifecycle/invariant。

最终除了：

```text
signal request flags
```

最好没有跨模块 direct writes。

---

# 49. `volatile sig_atomic_t` 仅适合 signal communication

不要把它扩展到：

```text
general thread synchronization
```

它只保证：

```text
signal-safe atomic access
```

不是 multi-thread memory model替代品。

---

# 50. 如果 signal handler已经改用 signalfd/reactor

当前 reactor本身通过 signalfd接收 signal。

那么 callback实际上运行在：

```text
reactor thread
```

不是 POSIX async signal handler context。

这意味着：

```text
sig_atomic_t
```

甚至可能不是必须。

---

# 51. 但保留简单 flag仍可以

它表达：

```text
deferred intent
```

而不是线程安全。

后续可以改普通 bool/enum，只要调用上下文明确。

---

# 52. 更推荐一个 pending request bitmask

如果未来 command增多：

```c
enum {
    RUNTIME_REQ_SHUTDOWN = 1u << 0,
    RUNTIME_REQ_RELOAD   = 1u << 1,
    RUNTIME_REQ_STATUS   = 1u << 2,
};
```

但当前3个 bool非常清楚，

没必要为抽象而抽象。

---

# 53. Shutdown优先级

如果同时：

```text
reload_requested
shutdown_requested
```

应该：

```text
shutdown wins
```

当前 on_idle先处理 reload，再检查 running。

可能出现：

```text
SIGHUP
SIGTERM
同一轮
↓
先 reload
再 shutdown
```

这没有必要。

---

# 54. 推荐 request处理顺序

```text
if shutdown_requested:
    begin shutdown
    return

if reload_requested:
    process reload

if status_requested:
    emit status
```

这是 runtime request owner应该处理的。

---

# 55. 这不是 `atpd_global` 自己的功能

只是因为我们正在迁 signal flags，

顺便应把语义做对。

---

# 56. `running` 命名还会导致逻辑误判

当前：

```text
if (!g_running)
```

听起来像：

```text
runtime已经不运行
```

实际上只是：

```text
收到停止请求
```

重命名能直接减少 lifecycle错误。

---

# 57. `reload` 同理

```text
g_reload = 1
```

并不代表：

```text
正在 reload
```

而是：

```text
reload requested
```

runtime state：

```text
RELOADING
```

才代表实际执行状态。

---

# 58. `show_status`

同样：

```text
status_requested
```

更准确。

---

# 59. Test：删除 macro alias后行为一致

第一阶段只删：

```text
main.c重复 macro
```

和逐步显式化时，

行为不能改变。

---

# 60. Test：global write audit

CI可以加入：

```text
grep
```

禁止新的：

```text
g_config.
g_svc->
g_reactor
g_api_ctx.
```

出现在未授权模块。

---

# 61. 可以建立 allowlist过渡

例如：

```text
main.c temporarily allowed
config.c allowed internal
service owner allowed
```

然后逐步缩小。

---

# 62. Test：config direct mutation为0

完成 config refactor后：

```text
grep "g_config\."
```

除了 legacy/migration tests：

```text
0 production occurrences
```

---

# 63. Test：api global dependency为0

完成 API refactor：

```text
src/api.c
```

不包含：

```text
atpd_global.h
g_config
```

---

# 64. Test：service direct internals为0

main/runtime不能：

```text
g_svc->monitor_timer
g_svc->child_pid
g_svc->state
```

只调用 service public lifecycle API。

---

# 65. Test：reactor pointer无 generic global access

subsystem在 attach后使用：

```text
自己的 owner pointer
```

不是：

```text
g_reactor
```

---

# 66. Test：signal requests scoped to runtime

最终：

```text
g_running
g_reload
g_show_status
```

宏不存在。

---

# 67. Test：`atpd_global.h` include count逐步归零

可以：

```text
grep '#include "atpd_global.h"'
```

观察迁移。

最终：

```text
0
```

然后删除文件。

---

# 68. Header include graph测试

删除 `atpd_global.h` 后，

某些源文件应该不再间接依赖：

```text
service.h
api.h
reactor.h
```

有助于降低 compile coupling。

---

# 69. Unit tests更容易

没有 global service locator后：

```text
api tests
config tests
session tests
```

不必初始化：

```text
整个 g_atpd
```

依赖可以显式构造。

---

# 70. Fault injection也更容易

例如 service test可以传：

```text
fake reactor
test config subset
```

不必修改：

```text
global root
```

---

# 71. 不要为了测试引入复杂 DI framework

C项目完全不需要：

```text
container
factory registry
interface inheritance
```

只需要：

```text
explicit function parameters
module-owned state
```

就够。

---

# 72. 与 `atpd_context` 的最终关系

推荐：

```text
atpd_context.c
→ daemon observed state

runtime/main.c
→ lifecycle pointers + pending requests, private

config.c
→ config snapshot

api.c
→ API policy/facade state

service.c
→ supervisor state
```

不再有：

```text
public global bundle
```

---

# 73. 与 `atpd_init` 方案关系

init transaction可以持：

```text
local/private init context
```

里面暂时包含：

```text
reactor
service
api
```

但这个 context：

```text
不 export
不通过 global macro访问
```

startup完成后交给 private runtime state。

---

# 74. 这比 public `g_atpd` 安全很多

因为：

```text
only lifecycle code
```

能访问 ownership pointers。

其他 module只能：

```text
通过自己的 API
```

---

# 75. 与 status方案关系

status collector从：

```text
owner snapshots
```

收集。

不需要：

```text
g_atpd
```

作为数据库。

---

# 76. 与 config方案关系

这是删除 `g_config` 的关键前提。

transactional config如果完成后仍然：

```text
extern mutable g_config
```

等于没有真正封住边界。

---

# 77. 与 API方案关系

这是删除：

```text
api.c → atpd_global.h → g_config
```

的关键前提。

---

# 78. 与 service方案关系

这是删除：

```text
main → g_svc internals
```

的关键前提。

---

# 79. 与 reactor方案关系

这是删除 generic：

```text
g_reactor
```

并明确 attach/detach lifecycle 的关键前提。

---

# 80. 推荐 Commit 1

```text
global: remove duplicate aliases and document ownership
```

内容：

- 删除 main.c重复 `#define`
- 注释 atpd_global为 temporary legacy root
- 不改行为

---

# 81. Commit 2

```text
global: audit and classify all global accesses
```

可以是：

```text
doc/test-only commit
```

生成 usage matrix。

---

# 82. Commit 3

```text
config: remove mutable g_config access
```

配合 config transactional plan。

---

# 83. Commit 4

```text
api: detach api context from global config
```

配合 API plan。

---

# 84. Commit 5

```text
runtime: make service and reactor ownership private
```

main/runtime private state持 pointer。

删除：

```text
g_svc
g_reactor
```

macro。

---

# 85. Commit 6

```text
runtime: replace global lifecycle flags with explicit requests
```

重命名：

```text
shutdown_requested
reload_requested
status_requested
```

并放 runtime/main private scope。

---

# 86. Commit 7

```text
global: remove obsolete atpd_global module
```

删除：

```text
src/atpd_global.c
include/atpd_global.h
```

Makefile同步。

这是最终目标。

---

# 87. 不要在第一阶段删除文件

因为当前 `main.c`、`api.c` 等仍有真实依赖。

应该以：

```text
consumer migration
```

驱动删除。

---

# 88. Codex应该输出一份迁移表

例如：

```text
g_config
owner: config
replacement:
  config snapshot / module-owned config subset

g_api_ctx
owner: api/runtime
replacement:
  explicit api owner / facade

g_reactor
owner: daemon runtime
replacement:
  attach dependency during init

g_svc
owner: service/runtime
replacement:
  service public API

g_running
owner: runtime request
replacement:
  shutdown_requested

g_reload
owner: runtime request
replacement:
  reload_requested

g_show_status
owner: runtime request
replacement:
  status_requested
```

---

# 89. 最终允许的全局状态应该非常少

ATPD是单 daemon进程，

完全消灭所有 global不是目标。

目标是：

```text
global state有明确理由
```

而不是：

```text
方便就放 root struct
```

---

# 90. 如果最终保留一个 process-global config path

例如：

```text
CLI selected config path
```

也最好属于：

```text
options/runtime object
```

不要重新加进 `atpd_global`。

---

# 91. 如果未来做 Go rewrite

这个边界也很有价值。

Go版本自然会变成：

```text
Daemon struct
```

但仍应该：

```text
private ownership
explicit dependencies
```

而不是 package-level globals。

当前 C重构会让未来迁移更顺。

---

# 92. 不要为 Go rewrite牺牲当前 C清晰性

本轮所有改造本身就能降低：

```text
C daemon lifecycle bug
```

不是为了以后才有价值。

---

# 93. CI gate建议

在最终阶段：

```text
grep -R "g_atpd" src include
grep -R "g_config" src include
grep -R "g_api_ctx" src include
grep -R "g_reactor" src include
grep -R "g_svc" src include
```

应该：

```text
0 production matches
```

或者只有明确 intentional symbol。

---

# 94. 另一个 CI gate

禁止新增：

```c
#include "atpd_global.h"
```

一旦迁移开始，可以先 allowlist。

最终删除 header后自然失败。

---

# 95. 最终 Invariants

Codex最终应保证：

```text
I1:
ATPD has no public global service-locator struct

I2:
config state has one authoritative owner: config subsystem

I3:
API state has one authoritative owner: API subsystem

I4:
service lifecycle state has one owner: service supervisor

I5:
reactor lifetime is owned by daemon runtime and passed explicitly to dependents

I6:
signal/request flags represent intent, not actual runtime state

I7:
no macro hides `g_atpd.field` as an independent global variable

I8:
no module can mutate config/service/API internals through generic global aliases

I9:
atpd_context is not used as a replacement god object

I10:
atpd_global.c/h are removable once consumers migrate
```

---

# 96. 最终验收

## Global aliases

```text
g_config
g_api_ctx
g_reactor
g_svc
g_running
g_reload
g_show_status
```

全部移除。

## Public root

```text
extern atpd_global_t g_atpd
```

不存在。

## Config

```text
no mutable global config access
```

## API

```text
api.c no atpd_global.h
```

## Service

main/runtime不读写 service internals。

## Reactor

subsystem通过 attach/init拿 reactor，

不查 global。

## Runtime requests

```text
shutdown/reload/status
```

由 private runtime owner管理。

---

# 97. 最终结论

`atpd_global.c` 本身不是一个“需要优化的9行代码”。

它实际上是：

> ATPD 当前多个 ownership boundary 的总后门。

它通过：

```text
g_config
g_api_ctx
g_reactor
g_svc
```

把 config、API、reactor、service重新捆回了一个 public global bundle。

因此正确方向不是增强它，而是逐步拆掉它。

建议最终架构：

```text
config      owns config
api         owns API policy/control state
service     owns child lifecycle
reactor     owned by private daemon runtime
context     owns only true daemon-global observed state

main/runtime
    ↓
private orchestration references

(no public atpd_global)
```

如果前面各模块的重构按计划完成，`atpd_global.c / atpd_global.h` 最终应该可以整个删除。

这会是一个非常好的架构收敛信号：
ATPD 从“靠全局共享对象协调”转向“靠明确 owner + narrow API 协调”。
