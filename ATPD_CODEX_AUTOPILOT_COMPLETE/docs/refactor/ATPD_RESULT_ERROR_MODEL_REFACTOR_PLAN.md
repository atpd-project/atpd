# ATPD `atp_error.h` 返回码体系与诊断错误边界收敛方案

## 1. 结论

当前：

```text
include/atp_error.h 37 lines
```

内容非常简单：

```c
typedef enum {
    ATP_OK = 0,
    ATP_ERR_GENERAL = -1,
    ATP_ERR_NOMEM = -2,
    ATP_ERR_NOENT = -3,
    ATP_ERR_PERM = -4,
    ATP_ERR_TIMEOUT = -5,
    ATP_ERR_BUSY = -6,
    ATP_ERR_INVAL = -7,
    ATP_ERR_IO = -8,
    ATP_ERR_CONFIG = -9,
    ATP_ERR_EBPF = -10,
    ATP_ERR_SERVICE = -11,
    ATP_ERR_NETLINK = -12,
} atp_error_t;
```

以及：

```c
static inline const char *atp_strerror(int err);
```

模块不需要拆分。

真正要解决的是语义：

> 这套东西其实是“函数返回状态码”，不是“诊断错误事件”。

而项目里已经另有：

```text
atpd_error.h / atpd_error.c
```

负责：

```text
错误历史
message
file/line/function
timestamp
errno
diagnostics ring
```

两者应该明确分工。

推荐最终将：

```text
atp_error_t
```

重命名/重定位为：

```text
atp_result_t
```

或类似的“通用操作结果”。

---

# 2. 当前命名容易与 `atpd_error.h` 混淆

现在项目中同时有：

```text
atp_error_t
atpd_error_code_t
atp_strerror()
atpd_error_report()
```

对于维护者来说很容易产生问题：

```text
函数失败到底应该 return ATP_ERR_IO
还是调用 ATPD_ERROR(ATPD_ERROR_IO)
还是两者都做？
```

---

# 3. 正确分层

推荐：

```text
return code
    ↓
告诉 caller “操作结果是什么”

diagnostic event
    ↓
告诉 operator “发生了什么，在哪里，为什么”
```

例如：

```c
atp_result_t rc = config_load(...);

if (rc != ATP_OK) {
    atpd_error_report(...);
    return rc;
}
```

但是否 report 应由：

```text
owner / boundary
```

决定。

---

# 4. 不要每层都 report

否则：

```text
leaf
→ report
caller
→ report
top-level
→ report
```

同一个错误会进入 ring 三次。

---

# 5. 推荐规则

低层函数：

```text
return typed result
set out detail / preserve errno
```

owner/boundary：

```text
决定是否写 atpd_error diagnostics
```

例如：

```text
service owner
config transaction owner
daemon startup owner
```

---

# 6. 当前 `ATP_ERR_EBPF` 应删除

前面的架构已经确定：

```text
ATPD 不拥有 eBPF dataplane
sing-box owns ebpf-in
```

所以通用 ATPD result enum 不应继续存在：

```text
ATP_ERR_EBPF
```

---

# 7. 更大的问题：`SERVICE` / `NETLINK` 也是“模块名型错误”

当前：

```text
ATP_ERR_SERVICE
ATP_ERR_NETLINK
```

这种设计会导致 result enum随着模块增长：

```text
ATP_ERR_API
ATP_ERR_UDS
ATP_ERR_REACTOR
ATP_ERR_SESSION
ATP_ERR_LOGGER
...
```

最终变成另一个 diagnostics taxonomy。

---

# 8. 通用函数返回码不应该按模块分类

caller真正需要知道的是：

```text
invalid argument
not found
permission denied
timeout
busy
I/O
out of memory
invalid config
not supported
unavailable
canceled
internal error
```

而不是：

```text
错误发生在哪个模块
```

模块来源应由：

```text
call context / diagnostic report
```

表达。

---

# 9. 推荐通用 result taxonomy

例如：

```c
typedef enum {
    ATP_OK = 0,

    ATP_ERR_GENERAL      = -1,
    ATP_ERR_NOMEM        = -2,
    ATP_ERR_NOENT        = -3,
    ATP_ERR_PERM         = -4,
    ATP_ERR_TIMEOUT      = -5,
    ATP_ERR_BUSY         = -6,
    ATP_ERR_INVAL        = -7,
    ATP_ERR_IO           = -8,
    ATP_ERR_CONFIG       = -9,
    ATP_ERR_NOTSUP       = -10,
    ATP_ERR_UNAVAILABLE  = -11,
    ATP_ERR_CANCELED     = -12,
} atp_result_t;
```

这里只是示意。

最终以真实 callsite audit为准。

---

# 10. 不要为了“完整”预先添加很多状态

先 grep 当前 caller真正需要：

```text
timeout
permission
config
not found
busy
I/O
invalid
memory
```

再决定 enum。

---

# 11. `ATP_ERR_GENERAL` 应尽量少用

它本质上等于：

```text
unknown/internal failure
```

如果大量 caller都：

```c
return ATP_ERR_GENERAL;
```

typed result就失去价值。

---

# 12. 推荐重命名

如果真实含义是：

```text
internal/unclassified error
```

可以：

```text
ATP_ERR_INTERNAL
```

比：

```text
GENERAL
```

更明确。

---

# 13. `ATP_ERR_NOENT` 当前 strerror 是 `"File not found"`

但：

```text
NOENT
```

并不一定是文件。

可能：

```text
PID不存在
service不存在
session不存在
resource不存在
```

---

# 14. 推荐字符串

```text
"Not found"
```

不要把 generic code解释成：

```text
File not found
```

---

# 15. `ATP_ERR_PERM`

当前：

```text
"Permission denied"
```

合理。

---

# 16. `ATP_ERR_BUSY`

当前：

```text
"Resource busy"
```

合理。

---

# 17. `ATP_ERR_INVAL`

当前：

```text
"Invalid argument"
```

合理。

但 config syntax/value错误应：

```text
ATP_ERR_CONFIG
```

不要全部压成 INVAL。

---

# 18. `ATP_ERR_IO`

合理。

但需要明确：

```text
网络 socket error算 IO?
```

对于通用 result：

可以。

更具体的：

```text
ECONNREFUSED
EPIPE
```

保存在：

```text
errno / transport-specific detail
```

而不是继续扩 generic enum。

---

# 19. 不要复制整套 errno

当前 enum里已经有：

```text
NOMEM
NOENT
PERM
TIMEOUT
BUSY
INVAL
IO
```

它们和 errno高度相似。

这是可以接受的：

```text
跨模块 stable abstraction
```

但不要继续变成：

```text
ATP_ERR_AGAIN
ATP_ERR_INTR
ATP_ERR_PIPE
ATP_ERR_CONNRESET
...
```

---

# 20. 什么时候直接返回 `-errno`

这是另一种设计：

```c
return -errno;
```

Linux C里常见。

但 ATPD当前已经建立：

```text
ATP_ERR_*
```

不建议现在全仓改成 raw `-errno`。

---

# 21. 原因

raw `-errno` 会：

```text
把平台 errno ABI直接暴露给所有内部模块
```

并且：

```text
业务结果 CONFIG/CANCELED/UNAVAILABLE
```

没有自然 errno映射。

---

# 22. 推荐继续保留 stable internal result enum

系统错误额外携带：

```text
saved_errno
```

需要时用于 diagnostics。

---

# 23. 与前面的 `atpd_error` 方案联动

例如：

```c
int saved_errno = errno;

return ATP_ERR_IO;
```

上层：

```c
ATPD_ERROR_ERRNO(ATPD_ERROR_IO, saved_errno, "...");
```

这样：

```text
result = machine branch
errno = OS detail
diagnostic code = operator category
message = context
```

分层清楚。

---

# 24. 不要用 `atp_strerror()` 代替 `strerror(errno)`

它们不是一回事。

```text
atp_strerror(ATP_ERR_IO)
→ "I/O error"

strerror(EACCES)
→ "Permission denied"
```

诊断时可能同时需要。

---

# 25. 推荐 result-to-string 名称

如果类型改成：

```text
atp_result_t
```

函数更明确：

```c
const char *atp_result_string(atp_result_t result);
```

而不是：

```text
atp_strerror
```

---

# 26. `strerror` 这个名字天然让人联想到 errno

`atp_strerror()` 虽然有前缀，

仍然会让人误以为：

```text
传 errno
```

---

# 27. 当前函数签名也暴露这个问题

```c
static inline const char *atp_strerror(int err)
```

它接受：

```text
任何 int
```

而不是：

```text
atp_error_t
```

---

# 28. 应改成 typed 参数

例如：

```c
const char *atp_result_string(atp_result_t result);
```

---

# 29. `static inline` 是否合理

当前字符串 switch放在 public header里。

优点：

```text
简单
无 link dependency
```

缺点：

```text
每个 include TU都看到实现
header扩大
未来表变化导致更多 recompilation
不能方便做唯一 coverage测试
```

---

# 30. 模块只有十几个 code

两种方案都可以。

但如果前面已经在清理 umbrella/public headers：

更推荐：

```text
include/atp_result.h
    enum + declaration

src/atp_result.c
    string implementation
```

---

# 31. 是否值得为了 15行新增 `.c`

这取决于最终 header cleanup。

如果项目希望极简文件数：

保留 inline也完全可以。

这里不是 correctness问题。

---

# 32. 更重要的是名字与 taxonomy

不要为了这个点过度设计。

---

# 33. Numeric values是否稳定

当前显式：

```text
0
-1
...
-12
```

需要先 audit是否出现在：

```text
UDS protocol
API response
shell exit code
logs parsed by scripts
tests
persistent state
```

---

# 34. 如果只在进程内部使用

可以自由：

```text
删除 EBPF
调整 enum
重命名
```

---

# 35. 如果已经对外序列化

必须：

```text
保留 numeric ABI
```

例如删除：

```text
-10
```

时可以：

```text
reserved
```

而不是把 NETLINK从：

```text
-12
```

移动到：

```text
-10
```

---

# 36. 目前最推荐先做 callsite audit

搜索：

```bash
grep -R "ATP_ERR_" src include tests
grep -R "ATP_OK" src include tests
grep -R "atp_error_t" src include tests
grep -R "atp_strerror" src include tests
```

并分组：

```text
function return
comparison
serialization
log
CLI exit mapping
tests
```

---

# 37. 特别检查返回类型

项目当前大量函数很可能仍然：

```c
int foo(...);
```

但 return：

```text
ATP_OK
ATP_ERR_*
```

---

# 38. 这是可以接受的迁移阶段

最终可以逐步改为：

```c
atp_result_t foo(...);
```

让编译器/读者知道：

```text
这不是任意 int。
```

---

# 39. 但不要一次改所有 `int` 返回

只改那些：

```text
明确遵循 ATP result convention
```

的 API。

---

# 40. 有些函数返回 int 是真实值

例如：

```text
fd
count
pid
bytes
bool-ish result
```

不能机械替换。

---

# 41. 推荐 API convention

对于：

```text
只返回成功/失败类别
```

使用：

```c
atp_result_t
```

---

# 42. 对：

```text
值 + failure
```

使用：

```c
atp_result_t foo(..., value_t *out);
```

不要：

```text
负数是 error
正数是 value
```

除非领域天然如此。

---

# 43. 例如 fd API

POSIX本身：

```text
fd >= 0
-1 failure
```

可以继续 POSIX convention。

不需要强行 ATP result。

---

# 44. Result enum不应该直接成为 process exit code

当前：

```text
-1 ... -12
```

shell exit code只能：

```text
0..255
```

如果：

```c
return ATP_ERR_CONFIG;
```

从 `main()` 返回：

```text
-9
```

shell看到：

```text
247
```

完全不可读。

---

# 45. 必须有 CLI/process exit mapping

例如：

```text
internal result
→ daemon/CLI exit category
```

前面 main方案已经建议：

```text
usage
config invalid
already running
startup failure
runtime fatal
clean shutdown
```

---

# 46. 所以不要：

```c
return ATP_ERR_*;
```

直接穿透到：

```text
main()
```

除非显式转换。

---

# 47. UDS也不应直接发负数 enum

除非协议正式定义：

```text
stable wire codes
```

更推荐：

```text
protocol status enum
```

独立于内部 result。

---

# 48. 否则内部重构会破坏客户端

---

# 49. `ATP_ERR_CONFIG` 的边界

这是目前少数真正有业务语义的 code。

合理场景：

```text
parse error
validation fail
candidate invalid
```

---

# 50. 但 reload candidate失败不等于 runtime fatal

caller收到：

```text
ATP_ERR_CONFIG
```

后决定：

```text
preserve active config
```

这与前面的 transactional reload方案一致。

---

# 51. `ATP_ERR_SERVICE` 应拆回性质码

例如 service失败可能是：

```text
binary not found
→ NOENT

permission
→ PERM

child timeout
→ TIMEOUT

invalid service state
→ BUSY / INVAL

fork/pipe I/O
→ IO / GENERAL
```

所以：

```text
SERVICE
```

本身信息量反而低。

---

# 52. `ATP_ERR_NETLINK` 同理

可能是：

```text
socket create fail
bind fail
permission
unsupported
parse malformed kernel message
```

generic caller真正需要：

```text
PERM
IO
NOTSUP
INVAL
```

---

# 53. 模块名进入 diagnostic event更合适

例如：

```text
ATPD_ERROR_NETLINK
```

说明：

```text
故障来源
```

与此同时函数 return：

```text
ATP_ERR_PERM
```

说明：

```text
故障性质
```

这正好互补。

---

# 54. 所以 `atpd_error` taxonomy 与 result taxonomy不应一一对应

这是重要原则。

例如：

```text
diagnostic code:
NETLINK

result:
PERM
```

或者：

```text
diagnostic code:
SERVICE

result:
TIMEOUT
```

完全合理。

---

# 55. 不要写自动映射：

```text
ATP_ERR_NETLINK → ATPD_ERROR_NETLINK
```

因为删除模块型 result后就不需要。

owner知道自己的诊断 domain。

---

# 56. Error context不要塞进 result enum

不要添加：

```text
ATP_ERR_SERVICE_START_TIMEOUT
ATP_ERR_SERVICE_STOP_TIMEOUT
ATP_ERR_API_CONNECT_TIMEOUT
```

这种 explosion。

---

# 57. Context应该在 message / typed owner-specific result

如果某 owner真的需要区分：

```text
START_TIMEOUT
STOP_TIMEOUT
```

可以内部：

```text
service_stop_result_t
```

但 public generic result仍：

```text
TIMEOUT
```

---

# 58. 不要把所有 owner-specific result都公开

内部 static/private enum即可。

---

# 59. `NOTSUP` 很值得加入

前面多个模块已经出现：

```text
splice fallback
platform feature
API capability
```

当前 generic result里没有：

```text
not supported
```

导致 caller可能用：

```text
GENERAL / INVAL
```

不准确。

---

# 60. `UNAVAILABLE` 也可能值得加入

比如：

```text
sing-box Native API暂时不可用
control service未ready
```

它不同于：

```text
NOENT
TIMEOUT
BUSY
```

---

# 61. 但要避免重复

如果实际 callsite可以用：

```text
BUSY
```

就不一定要 UNAVAILABLE。

通过 callsite audit决定。

---

# 62. `CANCELED` 对 async lifecycle很有用

前面的：

```text
async_validate
service shutdown
reload cancellation
```

都存在：

```text
operation canceled because daemon is stopping
```

这不是：

```text
TIMEOUT / GENERAL
```

---

# 63. 所以推荐候选：

```text
NOTSUP
CANCELED
```

优先级高于一堆 module codes。

---

# 64. 是否需要 `AGAIN`

如果 reactor async API需要：

```text
would block / try later
```

更推荐使用：

```text
EAGAIN作为内部 I/O条件
```

不要立刻加入 generic result。

---

# 65. 是否需要 `EXISTS`

single-instance PID lock：

```text
already running
```

可以：

```text
BUSY
```

或者新：

```text
EXISTS
```

---

# 66. 对 ATPD 来说 `BUSY` 已足够

CLI boundary再映射为：

```text
already running
```

不必扩 enum。

---

# 67. Error string必须稳定但不必过度详细

`atp_result_string()` 是：

```text
generic fallback
```

不是最终 operator message。

---

# 68. 例如：

```text
ATP_ERR_TIMEOUT
→ "Operation timed out"
```

即可。

具体：

```text
sing-box failed to stop within 5s
```

来自 owner diagnostics。

---

# 69. 不要在 result string里包含动态数据

保持：

```text
const literal
```

无 allocation。

---

# 70. Thread safety

当前 static string switch：

```text
天然 thread-safe
```

这是优点。

保留。

---

# 71. Unknown code

当前：

```text
"Unknown error"
```

合理。

若改 result naming：

```text
"Unknown result"
```

更准确，但用户文案略怪。

可以：

```text
"Unknown error"
```

继续。

---

# 72. 0 应该是唯一 success

保持：

```text
ATP_OK = 0
```

这与 C convention很好。

---

# 73. 是否允许 positive status

不建议。

generic result：

```text
0 success
negative error
```

保持简单。

---

# 74. Warning/degraded不属于 generic function failure

不要添加：

```text
ATP_WARN_DEGRADED = 1
```

runtime health：

```text
RUNNING / DEGRADED
```

由 state/snapshot表达。

---

# 75. Partial success也应 owner-specific

例如 logger：

```text
file fail but stderr works
```

用：

```text
LOGGER_INIT_DEGRADED
```

比污染 generic ATP result更合适。

---

# 76. 与 logger方案联动

logger init可能采用：

```text
OK / DEGRADED / FAILED
```

这不是：

```text
atp_result_t
```

的最佳模型。

它可以有自己的：

```text
logger_init_result_t
```

---

# 77. 与 netlink方案联动

netlink attach也可能：

```text
required vs degraded
```

同样由 owner结果 + daemon startup policy决定。

---

# 78. Generic result不要承担 lifecycle health state

---

# 79. 与 public header cleanup联动

建议最终：

```text
atp_error.h
```

重命名为：

```text
atp_result.h
```

---

# 80. 这是一个值得做的 rename

因为它可以明确：

```text
这不是 atpd_error diagnostics。
```

---

# 81. 但 rename优先级低于语义清理

顺序：

```text
先删 module codes
确认 callers
再 rename
```

避免大 diff混在一起。

---

# 82. 如果担心一次 rename影响大量 include

可以阶段一：

```text
保留文件名 atp_error.h
typedef atp_result_t
```

阶段二再 rename。

---

# 83. 不建议长期做 compatibility typedef

例如：

```c
typedef atp_result_t atp_error_t;
```

只作为短迁移期。

最终删旧名。

---

# 84. 编译器 warning帮助迁移

如果 toolchain支持：

```text
deprecated attribute
```

不值得为内部项目加入。

直接 grep/build fix更简单。

---

# 85. 与 `atp.h` 删除联动

当前 `atp.h`：

```c
#include "atp_error.h"
```

导致 result type被所有模块隐式获得。

前一轮已经要求：

```text
direct include what you use
```

---

# 86. 最终只有真正 return ATP result的模块：

```c
#include "atp_result.h"
```

其他模块不 include。

---

# 87. 这会暴露一些函数其实从没遵循 ATP result convention

很好。

不要强行统一它们。

---

# 88. Header self-containment

`atp_result.h` 最终几乎没有 dependency。

非常容易单独 compile。

---

# 89. Tests：string coverage

遍历每个 enum：

```text
result_string != "Unknown"
```

确保增加 code时：

```text
忘记 switch
```

会被发现。

---

# 90. 如果 enum不是 contiguous

不要用简单 for loop假设。

可以显式 test table。

---

# 91. Test：unknown

```text
atp_result_string((atp_result_t)-999)
```

返回 stable fallback。

---

# 92. Test：NOENT文案

改后：

```text
"Not found"
```

而不是：

```text
"File not found"
```

---

# 93. Test：no eBPF result

```bash
grep -R "ATP_ERR_EBPF" include src tests
```

目标：

```text
0
```

---

# 94. Test：no module result codes

目标最终：

```text
ATP_ERR_SERVICE
ATP_ERR_NETLINK
```

也为 0。

---

# 95. 但删除前必须迁移 caller

例如：

```text
service returns ATP_ERR_SERVICE
```

改成：

```text
TIMEOUT / NOENT / IO / PERM / INTERNAL
```

按实际 failure path。

---

# 96. 不能机械全部改 `GENERAL`

否则只是换名字。

---

# 97. Test：no negative internal result from `main`

扫描：

```text
main return
```

必须通过：

```text
exit code mapper
```

不要让：

```text
-9 → shell 247
```

---

# 98. Test：UDS/API wire independence

检查：

```text
是否直接序列化 ATP_ERR_* numeric
```

若有：

建立 protocol mapping。

---

# 99. Test：diagnostics not duplicated

故障注入：

```text
config invalid
service timeout
netlink permission
```

检查 ring：

```text
每个 logical failure只有 owner boundary的一条主要 diagnostic
```

不要层层 report。

---

# 100. TSan

这个 header/string函数本身没有共享 mutable state。

无需专项 TSan。

---

# 101. Fuzz也没有必要

switch enum不是 parser。

---

# 102. 推荐 Commit 1

```text
result: audit ATP error-code consumers and wire exposure
```

先记录：

```text
return
comparison
serialization
exit
```

---

# 103. Commit 2

```text
result: remove obsolete eBPF error code
```

---

# 104. Commit 3

```text
result: replace module-specific errors with failure semantics
```

删除：

```text
SERVICE
NETLINK
```

按 caller真实故障迁移。

---

# 105. Commit 4

```text
result: add only required generic statuses
```

候选：

```text
NOTSUP
CANCELED
```

实际 callsite驱动。

---

# 106. Commit 5

```text
result: rename atp_error_t to atp_result_t
```

并：

```text
atp_strerror
→ atp_result_string
```

---

# 107. Commit 6

```text
headers: make result dependencies explicit
```

配合删除：

```text
atp.h umbrella
```

---

# 108. Commit 7

```text
main/control: map internal results to stable process/protocol exit codes
```

---

# 109. 不建议做复杂 error object

不要把：

```text
message
errno
file
line
timestamp
```

全部塞进：

```text
atp_result_t
```

因为这些已经属于：

```text
atpd_error diagnostics
```

---

# 110. 也不要返回 heap error

例如：

```c
atp_error_t *error
```

完全没必要。

---

# 111. C项目这里保持 value enum最稳

```text
cheap
copyable
no ownership
no allocation
```

---

# 112. 最终推荐边界

```text
atp_result_t
    ↓
control flow / caller branching

errno
    ↓
OS failure detail

owner-specific result
    ↓
仅在确实需要更多状态时

atpd_error_report
    ↓
operator diagnostics/history

process exit code
    ↓
CLI/service-manager contract

UDS/API status code
    ↓
wire contract
```

五层不要混。

---

# 113. 示例：Netlink permission failure

底层：

```c
int saved_errno = errno;
return ATP_ERR_PERM;
```

netlink owner：

```c
ATPD_ERROR_ERRNO(
    ATPD_ERROR_NETLINK,
    saved_errno,
    "failed to open netlink socket");
```

daemon startup：

```text
required netlink?
→ map to startup failure
```

CLI：

```text
exit nonzero stable code
```

---

# 114. 示例：Service stop timeout

service：

```text
owner-specific stop result = timeout
generic result = ATP_ERR_TIMEOUT
```

diagnostics：

```text
ATPD_ERROR_SERVICE
message = sing-box did not exit...
```

main：

```text
shutdown exit reason / escalation
```

---

# 115. 示例：Invalid reload candidate

config：

```text
ATP_ERR_CONFIG
```

validation report：

```text
line/key/reason
```

daemon：

```text
preserve old runtime
last_reload_failed
```

不是：

```text
runtime FAILED
```

---

# 116. 最终 Invariants

Codex最终必须保证：

```text
I1:
The generic ATP result type represents failure semantics, not module names.

I2:
ATPD-owned eBPF has no generic result code.

I3:
Return status and diagnostic error events are separate concepts.

I4:
Generic results never carry dynamic diagnostic context.

I5:
System errno detail is preserved separately when relevant.

I6:
Internal negative result codes are never leaked directly as process exit codes.

I7:
Internal result numeric values are not exposed on UDS/API unless explicitly mapped by a stable protocol.

I8:
Low-level layers do not duplicate the same diagnostic event at every call frame.

I9:
atp_result_string() accepts the typed result enum and returns stable static strings.

I10:
Only real callers justify additions to the generic result taxonomy.
```

---

# 117. 最终验收标准

## eBPF

```text
ATP_ERR_EBPF
→ removed
```

## Module errors

最终：

```text
ATP_ERR_SERVICE
ATP_ERR_NETLINK
→ removed
```

## Typed API

明确遵循 generic result convention的函数：

```text
return atp_result_t
```

## Diagnostics

```text
atpd_error
≠
atp_result
```

职责清楚。

## Process

```text
main never returns raw negative ATP result
```

## Wire

```text
UDS/API never depend accidentally on internal enum numeric layout
```

## Strings

每个公开 result：

```text
stable result string
```

---

# 118. 最终结论

`atp_error.h` 代码本身没有复杂 bug。

它的问题是 **命名与层次边界**：

```text
atp_error_t
```

其实更像：

```text
atp_result_t
```

而：

```text
ATP_ERR_SERVICE
ATP_ERR_NETLINK
ATP_ERR_EBPF
```

又把“错误发生在哪个模块”混进了“调用者应该如何理解失败”。

长期正确模型应该是：

```text
函数 return
→ TIMEOUT / PERM / IO / CONFIG / ...

diagnostics
→ SERVICE / NETLINK / API / ...

errno
→ OS具体原因
```

其中 `ATP_ERR_EBPF` 随前面的 dataplane ownership调整直接删除。

这个模块不需要做大，而是要做得更“窄”：最终它应该只是一组非常稳定、非常小的通用操作结果。
