# ATPD `include/atp.h` 公共头文件与核心定义收敛方案

## 1. 结论

当前：

```text
include/atp.h 149 lines
```

它表面上不大，但承担了过多完全不同的职责：

```text
libc / POSIX umbrella includes
产品名 / 产品版本
build time
默认安装路径
运行目录 / PID / socket / log path
sing-box binary/path
API 默认值
service 默认值
netlink timeout
command runner limits
eBPF probe limits
proxy mode
root method
config compatibility macros
```

这使得任何 include `atp.h` 的 `.c` 文件都间接得到大量：

```text
系统 header
config struct
error enum
pthread/socket/netlink 类型
全局常量
兼容宏
```

建议最终：

> 删除 `atp.h` 作为万能公共头文件，或将它压缩到极少量真正跨模块的 product constants；其他定义回收到各自 owner。

---

# 2. P1：`_FORTIFY_SOURCE` 定义位置无效/不可靠

当前：

```c
#include <stdio.h>
#include <stdlib.h>
...
#include <pthread.h>

#define _FORTIFY_SOURCE 3
```

`_FORTIFY_SOURCE` 是 libc feature/compiler hardening macro。

它必须在：

```text
系统头文件处理之前
```

由编译参数或翻译单元最前部定义。

当前在大量 libc header **之后**才：

```c
#define _FORTIFY_SOURCE 3
```

因此不能可靠开启 fortify。

---

# 3. 正确做法

不要放在公共 header。

放到 build flags：

```make
CPPFLAGS += -D_FORTIFY_SOURCE=3
```

并配合：

```text
-O1 或更高优化
```

以及 toolchain 支持。

如果目标 libc/toolchain 不支持 3：

CI/build probe 决定：

```text
2 或 3
```

---

# 4. 不要在 header里偷偷决定编译 hardening policy

同类 policy：

```text
_FORTIFY_SOURCE
stack protector
PIE
RELRO
NOW
```

应该统一在：

```text
Makefile / toolchain configuration
```

管理。

---

# 5. 当前产品版本定义重复

`atp.h`：

```c
#define ATP_VERSION_MAJOR 2
#define ATP_VERSION_MINOR 0
#define ATP_VERSION_PATCH 0
```

前一轮已经确认：

```text
Makefile = 2.0.0
versions.env = 1.0.0
version.h = v0.<commit>-dirty
```

---

# 6. 这些宏应全部从 `atp.h` 删除

版本唯一来源方案：

```text
/VERSION
→ generated version metadata
→ version.c API
```

其他模块不直接 include product-version macro。

---

# 7. `ATP_BUILD_TIME __TIME__` 也应删除

当前：

```c
#define ATP_BUILD_TIME __TIME__
```

会让：

```text
相同源码
相同 toolchain
不同构建时间
```

产生不同 binary。

这与 reproducible build目标冲突。

---

# 8. `ATP_NAME` 是否保留

```c
#define ATP_NAME "atpd"
```

这是少数可以作为真正 product constant 保留的定义。

但更推荐放到：

```text
product.h
```

或者 version/product metadata module。

不要为了一个字符串保留整个 umbrella header。

---

# 9. Path constants职责混杂

当前：

```text
ATP_DEFAULT_DIR
ATP_CONF_FILE
ATP_RUN_DIR
ATP_PID_FILE
ATP_LOG_FILE
ATP_COMMAND_SOCKET
ATP_RUNTIME_CONF
PROXY_PID_FILE
PROXY_LOG_FILE
TRAFFIC_STATE_FILE
PROXY_BIN_PATH
```

这里混了至少三类：

```text
install defaults
runtime paths
service/sing-box paths
```

---

# 10. 推荐 owner 化

例如：

```text
config/path defaults
→ config_defaults.h or config.c private constants

PID / UDS runtime paths
→ runtime/control owner

sing-box binary/log/pid
→ service owner
```

---

# 11. 不建议为了 constants 新建很多 header

原则：

```text
只有多个独立模块真的需要同一个 constant
才放 public header。
```

否则：

```text
static const / #define
```

留在 owner `.c` 文件。

---

# 12. `PROXY_BIN_NAME` / `PROXY_BIN_PATH`

这些明显属于：

```text
service / sing-box supervisor
```

不属于 ATPD core public API。

移到：

```text
service.c
service_config defaults
```

---

# 13. `PROXY_PID_FILE`

前面的 service review已经要求：

```text
ATPD自己持有 child PID/generation
```

不要依赖 name/PID-file discovery作为 primary ownership。

所以这个 constant是否还需要要重新 audit。

---

# 14. `PROXY_LOG_FILE`

如果 sing-box日志仍由 ATPD配置：

放 service owner。

如果 sing-box自己通过 config输出：

甚至可以删除。

---

# 15. `TRAFFIC_STATE_FILE`

需要全仓 grep。

如果只是旧 dataplane/runtime state残留：

直接删除。

不要因为曾经存在就保留。

---

# 16. 默认 API 参数应由 API/config owner拥有

当前：

```c
#define DEFAULT_API_PORT 9080
#define DEFAULT_API_HOST "127.0.0.1"
```

更合适放：

```text
config defaults
```

或：

```text
singbox_api configuration owner
```

---

# 17. Service 默认值同样

当前：

```text
SERVICE_DEFAULT_START_TIMEOUT_SEC
SERVICE_DEFAULT_STOP_TIMEOUT_SEC
SERVICE_DEFAULT_GRACE_PERIOD_SEC
SERVICE_DEFAULT_MAX_FAILURES
SERVICE_DEFAULT_CIRCUIT_THRESHOLD
SERVICE_DEFAULT_CIRCUIT_COOLDOWN_SEC
SERVICE_DEFAULT_HEALTH_CHECK_INTERVAL_MS
```

这些属于：

```text
service config defaults
```

不是 project-wide constants。

---

# 18. 推荐 single source

这些值现在很可能同时出现在：

```text
atp.h
config_set_defaults()
tests
README/example config
```

应统一由：

```text
config/service default initializer
```

拥有。

测试读 effective defaults，不复制数值。

---

# 19. `SERVICE_STOP_RETRY_COUNT / INTERVAL / TIMEOUT`

这些更明显是旧 `main.c service_stop_sync()` 实现细节。

前面已经确定：

```text
main-owned service_stop_sync 删除
```

因此这些 macro很可能可以直接删除。

---

# 20. 不要把 implementation constants放公共 core header

例如：

```text
retry count
poll interval
buffer length
```

只要只有一个模块使用：

```text
private static enum / const
```

即可。

---

# 21. eBPF probe constants全部删除

当前：

```c
#define EBPF_PROBE_TIMEOUT_SEC 10
#define EBPF_PROBE_RETRY_COUNT 3
#define EBPF_PROBE_RETRY_DELAY_SEC 2
```

ATPD 已确定不再负责：

```text
BPF capability probing
program/map lifecycle
kernel dataplane
```

因此这些都属于 obsolete。

---

# 22. `proxy_mode_t` 也已失去意义

当前：

```c
typedef enum {
    MODE_AUTO = 0,
    MODE_EBPF = 4
} proxy_mode_t;
```

当前 ATPD架构：

```text
sing-box ebpf-in 是 dataplane
```

并不是 ATPD在：

```text
AUTO / EBPF
```

之间选择 dataplane implementation。

---

# 23. 建议删除 `proxy_mode_t`

如果 config仍需要某个：

```text
routing/control policy mode
```

重新用与真实语义一致的 enum命名。

不要保留：

```text
MODE_EBPF
```

这种 owner错误的模式。

---

# 24. `atp_config.h` 同时还保留 eBPF config

当前：

```c
typedef struct {
    bool enabled;
    bool ready;
} ebpf_config_t;
```

并进入：

```c
atp_config_t {
    ...
    ebpf_config_t ebpf;
}
```

---

# 25. 这两个字段语义也不应该在 config里

```text
enabled
```

如果 dataplane由 sing-box config决定：

ATPD不应重复维护。

```text
ready
```

更明显是 runtime state，

根本不应该放 config struct。

---

# 26. `ready` 进入 config 是 state/config 混淆

正确：

```text
configuration
≠
observed runtime readiness
```

sing-box dataplane readiness来自：

```text
Native API/service snapshot
```

---

# 27. 所以随 eBPF removal：

删除：

```text
ebpf_config_t
atp_config_t.ebpf
cfg_ebpf_enabled
cfg_ebpf_ready
MODE_EBPF
EBPF_PROBE_*
ATP_ERR_EBPF
```

---

# 28. `atp_error.h` 仍保留 `ATP_ERR_EBPF`

当前：

```c
ATP_ERR_EBPF = -10
```

也应删除/重分类。

如果外部 numeric ABI不存在：

可以重新整理 enum。

---

# 29. 但先 audit error numeric stability

若：

```text
UDS/API
日志 parser
脚本
```

没有依赖 numeric code：

可安全重排。

否则：

```text
保留显式 value
标记 reserved
```

但不要继续提供“ATPD eBPF error”语义。

---

# 30. `atp_error.h` 与 `atpd_error.h` 是两套 error体系

这是一个值得特别注意的点。

目前 repo同时存在：

```text
atp_error_t / ATP_ERR_*
```

以及前面 review 的：

```text
atpd_error_code_t / atpd_error ring
```

---

# 31. 两者语义不同但命名极接近

`atp_error.h`：

```text
返回码/error status
```

`atpd_error.h`：

```text
historical diagnostics ring
```

这本身不是一定错误。

但命名容易混淆。

---

# 32. 推荐明确区分

例如：

```text
atp_result_t / ATP_OK / ATP_E_*
```

用于函数返回。

```text
atpd_error_report(...)
```

用于诊断事件。

---

# 33. 不要让 diagnostics enum充当函数返回码

也不要让：

```text
ATP_ERR_SERVICE
```

同时意味着：

```text
函数错误类别
历史诊断事件
```

保持两个层次。

---

# 34. `atp.h` include `atp_error.h` 和 `atp_config.h`

这导致任何 include：

```text
atp.h
```

自动得到：

```text
整个 config layout
error API
pthread_mutex_t
```

这就是典型 umbrella header coupling。

---

# 35. 应删除这两个 include

需要 config：

```c
#include "atp_config.h"
```

需要 result code：

```c
#include "atp_error.h"
```

显式依赖。

---

# 36. 当前系统 header include过量

`atp.h` 包含：

```text
stdio
stdlib
string
unistd
errno
fcntl
signal
stdbool
stdint
limits
sys/stat
sys/types
sys/wait
dirent
time
ctype
sys/socket
netinet/in
arpa/inet
net/if
pthread
```

---

# 37. 这掩盖每个 `.c` 文件的真实依赖

例如 source可能使用：

```text
pthread_mutex_t
```

但自己没有：

```c
#include <pthread.h>
```

只是碰巧因为：

```text
#include "atp.h"
```

编译通过。

---

# 38. 一旦未来移除 umbrella header就会暴露大量 hidden include dependency

这是好事。

应逐文件：

```text
include what you use
```

---

# 39. 好处

```text
更快增量编译
更少 macro污染
更容易迁移模块
更容易 fuzz/unit test
更少偶然 include-order依赖
```

---

# 40. `MAX_ARGS / MAX_CMD_LEN / MAX_OUTPUT_LEN`

这些属于：

```text
command execution
```

前面的 utils/process_exec plan已经建议 owner-specific。

移到：

```text
process_exec.c
```

或实际 consumer。

---

# 41. `QUEUE_SIZE`

名称过于泛化。

公共 macro：

```text
QUEUE_SIZE=64
```

完全不知道是哪一个 queue。

---

# 42. 这是公共头文件的典型 anti-pattern

应改为 owner-specific：

```text
REACTOR_TASK_QUEUE_CAPACITY
SESSION_...
```

或者 private constant。

如果已经无 caller：

删除。

---

# 43. `MAX_IFACE_NAME`

Linux已有：

```text
IFNAMSIZ
```

当前 config本身也已经使用：

```text
IFNAMSIZ
```

不要维护另一套：

```text
MAX_IFACE_NAME=32
```

---

# 44. `MAX_IP_STR=64`

IPv6文本缓冲通常可使用：

```text
INET6_ADDRSTRLEN
```

不要自定义 magic 64，除非用途包含：

```text
CIDR/port/zone
```

若只是 IP string：

使用系统标准 macro。

---

# 45. `NETLINK_RECV_TIMEOUT_MS / NETLINK_DEBOUNCE_MS`

属于：

```text
netlink.c
```

如果需要 config：

进入：

```text
netlink config
```

否则 private constant。

---

# 46. `API_RETRY_COUNT / DELAY / MIN_INTERVAL`

前面的 API review已经要求：

```text
移除 main/API同步 sleep retry
cached Native API snapshots
```

这些 constant应随旧 retry path audit。

---

# 47. `CMD_TIMEOUT_SEC`

同样与 `exec_cmd()` ownership有关。

如果 shell runner删除：

这个 macro可能也消失。

---

# 48. `DEFAULT_RESTART_DELAY`

与：

```text
service/config default
```

重复语义。

归 config/service owner。

---

# 49. Compatibility macros 是最大污染之一

当前：

```c
#define cfg_foreground cfg->core.foreground
#define cfg_verbose cfg->core.verbose
...
#define cfg_service_start_timeout_sec cfg->service.start_timeout_sec
...
#define cfg_api_secret cfg->api.secret
```

---

# 50. 这些 macro依赖调用点恰好有一个名为 `cfg` 的变量

例如：

```c
cfg_foreground
```

实际展开：

```c
cfg->core.foreground
```

这是非常隐式的 lexical coupling。

---

# 51. 它会产生难读代码

看到：

```text
cfg_api_port
```

不像：

```text
cfg->api.port
```

那样明确：

```text
访问的是哪个对象
```

---

# 52. 也会阻碍 refactor

如果 local variable叫：

```text
candidate
runtime_cfg
```

这些 macro就不能使用。

所以整个代码被强迫围绕：

```text
变量名 cfg
```

组织。

---

# 53. 建议全部删除 compatibility macros

直接使用：

```c
cfg->core.foreground
cfg->service.start_timeout_sec
cfg->api.port
```

---

# 54. 不要用 inline getter替代每一个字段

这会产生另一层无价值 wrapper。

只有需要：

```text
validation
derived semantics
locking/snapshot
```

的访问才做函数。

---

# 55. `cfg_config_mutex` 尤其应该删除

```c
#define cfg_config_mutex cfg->mutex
```

让 caller直接知道/锁 config内部 mutex。

---

# 56. 这破坏 config ownership

前面 transactional config方案已经要求：

```text
active config publish/commit由 config owner管理
```

caller不应该：

```text
pthread_mutex_lock(&cfg->mutex)
```

---

# 57. 更进一步：mutex可能不应该存在于 public `atp_config_t`

如果 active config改成：

```text
immutable snapshot
```

读取根本不需要每个 cfg带 mutex。

---

# 58. 当前 config struct混合 CLI/runtime fields

`core_config_t` 包含：

```text
foreground
verbose
no_color
dry_run
log_timestamp
```

其中至少：

```text
foreground
verbose
no_color
```

更像 CLI/runtime presentation options，

不是持久配置数据。

---

# 59. 这与前面 CLI review一致

CLI应该产生：

```text
run mode
verbosity
no-color intent
```

不直接塞进 config。

---

# 60. 建议审计 `atp_config.h`

虽然本轮主目标是 `atp.h`，

但这里已经明显看到：

```text
config model需要再收敛
```

---

# 61. `root_method_t` 不完整

当前：

```c
typedef enum {
    ROOT_UNKNOWN = 0,
    ROOT_KSU = 1,
    ROOT_MAGISK = 2
} root_method_t;
```

但项目目标/README明确支持：

```text
KernelSU
Magisk
APatch
```

所以 enum缺：

```text
ROOT_APATCH
```

---

# 62. 这有两种可能

### A

代码真的检测 root method。

那这是 feature-model bug：

```text
APatch无法被正确表达。
```

### B

当前已经不再需要 root method enum。

那应直接删除。

---

# 63. 先 grep `root_method_t / ROOT_`

如果没有实际 caller：

删除。

不要为了 README支持列表维护一个不用的 enum。

---

# 64. 如果保留

定义至少：

```text
UNKNOWN
KERNELSU
MAGISK
APATCH
```

命名也统一：

```text
ROOT_KERNELSU
```

比：

```text
ROOT_KSU
```

更清楚。

---

# 65. 但 root solution detection是否属于 ATPD core

如果只是安装脚本需要：

```text
daemon运行时不必知道。
```

可能更适合：

```text
scripts/install
```

处理。

---

# 66. `ATP_RUNTIME_CONF`

需要 callsite audit。

前面的 config方案主要谈：

```text
atp.conf
candidate reload
```

如果 runtime_atp.conf已经不再使用：

删除。

---

# 67. Runtime generated config要特别谨慎

如果它仍存在：

```text
谁生成
谁读取
何时删除
是否包含 secret
mode/permissions
atomic write
```

都应明确。

---

# 68. 不要让 `atp.h` 使 dead feature显得仍是核心 contract

这也是本轮最大的清理价值。

---

# 69. 头文件 guard命名

当前：

```c
#ifndef ATP_H
#define ATP_H
```

不算错误。

但如果最终只剩：

```text
product constants
```

建议更明确：

```text
ATPD_PRODUCT_H
```

---

# 70. 更推荐直接删除 `atp.h`

如果清理后没有足够内容。

例如：

```text
ATP_NAME
ATP_DEFAULT_DIR
```

完全可以放：

```text
config defaults / version API
```

则：

```text
include/atp.h
```

本身就没有存在必要。

---

# 71. 是否能直接删除取决于 include callsite数量

Codex先：

```bash
grep -R '#include "atp.h"' src include tests
```

按模块改成直接 include。

---

# 72. 推荐 migration方式

不要一次巨大 commit。

可以：

```text
1. 移 build/version定义
2. 移 eBPF definitions
3. 移 owner constants
4. 删除 compatibility macros
5. 逐 source direct includes
6. 删除 atp.h
```

---

# 73. Header self-containment test

每个 public header都应该能单独：

```c
#include "xxx.h"
int main(void) { return 0; }
```

编译通过。

---

# 74. 为什么重要

目前很多 header可能依赖：

```text
atp.h先 include了系统类型
```

导致单独 include失败。

---

# 75. CI增加 header compile smoke test

对：

```text
include/*.h
```

逐个生成 tiny TU 编译。

可以发现：

```text
missing stdint
missing size_t
missing bool
missing pthread
```

---

# 76. 这对 C 项目非常值得

成本很低。

---

# 77. Include-what-you-use 不需要引入外部工具

手工/编译器就够。

无需引入：

```text
IWYU
clang tooling
```

作为强依赖。

---

# 78. Dependency direction

最终应该类似：

```text
module .c
→ own public header
→ minimal standard headers
→ explicit dependent module headers
```

而不是：

```text
everything
→ atp.h
→ everything else
```

---

# 79. 与 Go rewrite的关系

这次清理即使未来重写 Go也有价值。

因为它明确了：

```text
哪些是产品 contract
哪些是 C implementation detail
哪些已经是 legacy。
```

---

# 80. 测试：`_FORTIFY_SOURCE`

build log / preprocessor：

```text
确认 compile command中有 -D_FORTIFY_SOURCE=...
```

不要通过 header猜。

---

# 81. 测试：header self-contained

每个：

```text
include/*.h
```

单独编译。

---

# 82. 测试：no eBPF public core state

完成删除后：

```bash
grep -R 'MODE_EBPF\|EBPF_PROBE_\|cfg_ebpf_\|ATP_ERR_EBPF\|ebpf_config_t' include src
```

应该只有：

```text
允许的 sing-box dataplane文案/Native API字段
```

而没有 ATPD ownership定义。

---

# 83. 测试：no compatibility cfg macros

```bash
grep -R '#define cfg_' include
```

目标：

```text
0
```

---

# 84. 测试：atp.h include removal

最终若删除：

```bash
grep -R '#include "atp.h"' src include tests
```

：

```text
0
```

---

# 85. 测试：version definitions

```bash
grep -R 'ATP_VERSION_MAJOR\|ATP_VERSION_MINOR\|ATP_VERSION_PATCH\|ATP_BUILD_TIME' .
```

目标：

```text
0
```

除 generated/version API允许内容。

---

# 86. 测试：root enum

如果保留：

```text
APatch必须可表达
```

如果没有 caller：

enum删除。

---

# 87. 测试：public defaults single owner

例如 service timeout：

```text
grep
```

确保不会同时定义：

```text
atp.h
config.c
service.c
tests
```

多个 authoritative值。

---

# 88. 测试：build without umbrella header

逐 `.c` 编译，

缺什么标准 header就：

```text
source/header显式 include
```

不要再建新的 umbrella header替代。

---

# 89. 推荐 Commit 1

```text
build: move fortify policy out of atp.h
```

---

# 90. Commit 2

```text
version: remove product version and build time from atp.h
```

与 version方案合并实施。

---

# 91. Commit 3

```text
ebpf: remove legacy core config, mode and error definitions
```

包括：

```text
MODE_EBPF
EBPF_PROBE_*
ebpf_config_t
cfg_ebpf_*
ATP_ERR_EBPF
```

---

# 92. Commit 4

```text
config: remove cfg compatibility macros
```

callsite改为显式 field access。

---

# 93. Commit 5

```text
core: move module-specific constants to their owners
```

service/API/netlink/process exec/path。

---

# 94. Commit 6

```text
headers: replace atp.h umbrella includes with direct dependencies
```

逐文件。

---

# 95. Commit 7

```text
headers: make every public header self-contained
```

加 CI smoke test。

---

# 96. Commit 8

如果内容清空：

```text
core: remove legacy atp.h umbrella header
```

---

# 97. 不建议做 `common.h`

这是最重要的防回归规则之一。

删除 `atp.h` 后，

不要立刻新建：

```text
common.h
base.h
all.h
```

把所有东西重新塞进去。

---

# 98. 真正 cross-cutting 的定义要极少

例如可能只有：

```text
generic result code
```

甚至 result code本身也可以 owner化。

---

# 99. `atp_error.h` 是否需要进一步重命名

建议后续轻量 review。

当前它是：

```text
generic return status code
```

而 `atpd_error.h` 是：

```text
diagnostic ring
```

长期名称最好区分：

```text
atp_result.h
atpd_error.h
```

避免误读。

---

# 100. `atp_config.h` 也建议作为本轮 companion cleanup

至少先做：

```text
remove ebpf_config_t
remove runtime `ready`
remove CLI-only fields if callsite允许
```

完整 config model重构按之前 config plan实施。

---

# 101. 最终可能结构

```text
include/
    api.h
    async_validate.h
    atpd_context.h
    atpd_error.h
    atp_result.h
    config.h
    logger.h
    netlink.h
    reactor.h
    service.h
    session.h
    singbox_api.h
    status.h
    uds.h
    ui.h
    version.h
```

没有：

```text
atp.h umbrella
ebpf.h
ebpf_common.h
legacy cleanup.h
```

---

# 102. 不必追求命名一次全部统一

例如：

```text
atp_config.h → config.h
atp_error.h → atp_result.h
```

可以分阶段。

优先：

```text
ownership correctness
```

而不是 cosmetic rename。

---

# 103. 最终 Invariants

Codex最终必须保证：

```text
I1:
Build hardening macros are controlled by compiler/build flags, not defined after libc headers.

I2:
Product version has no duplicate definitions in atp.h.

I3:
No ATPD-owned eBPF probe/config/readiness/mode remains in core public headers.

I4:
Runtime readiness is never stored in config structs.

I5:
Module-specific constants live with their owning module.

I6:
No public cfg_* macro depends on a local variable named `cfg`.

I7:
Public headers include the exact standard/module types they use.

I8:
No source file relies on atp.h as an accidental provider of libc/POSIX declarations.

I9:
Service/API/netlink/process-exec implementation constants are not exposed as global project constants.

I10:
Root-method representation either correctly includes APatch or is removed if unused.

I11:
No new umbrella/common header replaces atp.h.

I12:
Every public header is independently compilable.
```

---

# 104. 最终验收标准

## Fortify

```text
compiler command contains intended _FORTIFY_SOURCE
```

## eBPF

```text
MODE_EBPF
EBPF_PROBE_*
ebpf_config_t
cfg_ebpf_*
ATP_ERR_EBPF
→ removed
```

## Version

```text
ATP_VERSION_MAJOR/MINOR/PATCH
ATP_BUILD_TIME
→ removed from core headers
```

## Compatibility macros

```text
#define cfg_
→ 0
```

## Header hygiene

```text
every include/*.h standalone compile
```

## Ownership

```text
service defaults only owned by config/service
API defaults only owned by config/API
netlink timing only owned by netlink
```

## Umbrella

最终：

```text
atp.h deleted
```

或如果暂时保留：

```text
只剩极少 product-wide constants
无 system-header umbrella
无 compatibility macros
无 module implementation details
```

---

# 105. 最终结论

`atp.h` 是目前仓库里一个典型的历史“总头文件”。

它本身没有复杂算法，但会持续制造：

```text
隐藏依赖
重复版本定义
错误 ownership
legacy eBPF暴露
implementation constant泄漏
cfg变量名耦合
```

尤其当前这一行：

```c
#define _FORTIFY_SOURCE 3
```

放在大量 libc header之后，不能作为可靠 hardening配置；而：

```text
MODE_EBPF
EBPF_PROBE_*
cfg_ebpf_*
ebpf_config_t.ready
ATP_ERR_EBPF
```

又与现在的 sing-box-owned ebpf-in 架构直接冲突。

所以推荐最终方向不是“修好 atp.h”，而是：

> 逐步让 `atp.h` 没有存在的必要，然后删除它。

这会是前面 global/config/eBPF/version/service 各轮 ownership 收敛完成后的一个非常好的架构清理点。
