# ATPD `atpd_error.c / atpd_error.h` 错误状态与并发安全收敛方案

## 1. 模块结论

当前：

```text
src/atpd_error.c      127 lines
include/atpd_error.h   56 lines
```

模块本身很小，不需要拆文件。

它已经有一个不错的基础：

```text
固定大小 ring buffer
pthread mutex
错误码
file/line/function
total_count
日志输出
```

真正需要修的是：

```text
并发读取安全
内部指针泄漏
错误 ownership 重复
旧 eBPF 错误码
时间语义
日志与锁边界
API 命名/快照
```

---

# 2. P0/P1：`atpd_error_get()` 在加锁前读取 count

当前：

```c
const atpd_error_entry_t* atpd_error_get(int index) {
    if (index < 0 || index >= g_error_ring.count) return NULL;

    pthread_mutex_lock(&g_error_ring.mutex);
    ...
}
```

问题：

```text
g_error_ring.count
```

是共享可变状态，却在：

```text
mutex外
```

读取。

如果另一个线程同时：

```text
push
clear
init/reset
```

这是数据竞争。

---

# 3. `atpd_error_get_last()` 同样

当前：

```c
if (g_error_ring.count == 0) return NULL;
pthread_mutex_lock(...)
```

也是：

```text
check outside lock
use inside lock
```

典型 TOCTOU。

---

# 4. 更严重：getter 返回 ring 内部指针

`atpd_error_get()`：

```c
const atpd_error_entry_t *entry = &g_error_ring.entries[idx];
pthread_mutex_unlock(...);
return entry;
```

`atpd_error_get_last()`：

```text
同样
```

调用者拿到的是：

```text
ring内部 slot pointer
```

锁已经释放。

下一次：

```text
push()
```

可能覆盖这个 slot。

因此 caller持有的：

```text
const atpd_error_entry_t *
```

虽然类型是 const，

实际内容仍会在背后变化。

---

# 5. 这是典型 borrowed-pointer lifetime bug

API暗示：

```text
caller可以安全读取 returned entry
```

实际上只能在：

```text
mutex仍持有
```

时安全。

但 mutex在函数返回前已经释放。

所以 API contract本身错误。

---

# 6. 推荐改成 copy-out API

例如：

```c
int atpd_error_get(size_t index, atpd_error_entry_t *out);
int atpd_error_get_last(atpd_error_entry_t *out);
```

实现：

```text
lock
validate
copy
unlock
return
```

caller得到：

```text
稳定 snapshot
```

---

# 7. `atpd_error_count()` 可以保留

它已经：

```text
lock
copy count
unlock
```

正确。

但如果 caller：

```text
count()
for i...
    get(i)
```

中间 ring仍可变化。

---

# 8. 所以最好提供批量 snapshot

例如：

```c
typedef struct {
    atpd_error_entry_t entries[ATPD_ERROR_MAX];
    size_t count;
    uint64_t total_count;
} atpd_error_snapshot_t;

int atpd_error_snapshot(atpd_error_snapshot_t *out);
```

status/debug一次拿完整一致视图。

---

# 9. 不要让 status自己 count+逐条 get

否则：

```text
第一条和最后一条
```

可能来自不同时间点。

snapshot一次 copy更清楚。

---

# 10. `atpd_error_print_all()` 在持锁状态下调用 logger

当前：

```text
pthread_mutex_lock(error_mutex)
↓
LOG_INFO(...)
LOG_INFO(...)
...
↓
unlock
```

`atpd_error_push()` 同样：

```text
持 error mutex
↓
LOG_ERROR
↓
unlock
```

---

# 11. 这会扩大 lock critical section

logger可能：

```text
内部 mutex
file IO
flush
rotation
callback
```

于是 error mutex被长期占用。

---

# 12. 更危险的是 lock ordering

如果未来 logger在 error path里又调用：

```text
ATPD_ERROR(...)
```

就可能出现：

```text
error_mutex → logger_mutex
logger_mutex → error_mutex
```

死锁。

即使今天没有，

也不应建立这种锁嵌套。

---

# 13. 推荐先更新 ring，再 unlock，再 log

例如：

```text
lock
copy/store entry
unlock
↓
LOG_ERROR
```

日志需要的数据先放 local copy。

---

# 14. `print_all()` 也应该先 snapshot

```text
atpd_error_snapshot(&snap)
↓
unlock already done
↓
LOG_INFO snap
```

这样：

```text
logger IO
```

不占 error lock。

---

# 15. `atpd_error_clear()` 没重置 total_count

当前：

```text
entries/count/head/tail reset
total_count 保留
```

这可能是故意：

```text
total_count = process lifetime total
```

如果是，应明确写进 API contract。

---

# 16. 当前 `clear` 与 `init` 语义不同

```text
init
→ total_count = 0

clear
→ total_count unchanged
```

名字不够明确。

推荐：

```text
atpd_error_clear_history()
```

语义：

```text
清当前 ring
保留 lifetime total
```

如果需要全重置：

```text
atpd_error_reset()
```

仅测试/startup使用。

---

# 17. 更推荐 startup不需要显式 `init()`

当前 ring：

```c
static ... PTHREAD_MUTEX_INITIALIZER
```

已经静态初始化。

如果 daemon lifecycle只启动一次：

```text
atpd_error_init()
```

主要只是清零。

---

# 18. 与 context one-shot init类似

不要让 runtime中途调用：

```text
atpd_error_init()
```

把历史错误无声清掉。

最好：

```text
startup once
```

或者直接依赖 static initialization。

---

# 19. Context里的 error state应删除

前面 `atpd_context` 已确认有：

```text
last_error
error_count
stats.errors_total
```

而 `atpd_error.c` 已经拥有：

```text
ring
last entry
count
total_count
```

这是多份 source of truth。

---

# 20. 推荐唯一 owner

```text
atpd_error.c
```

作为错误历史权威来源。

context中删除：

```text
last_error
error_count
stats.errors_total
```

status从：

```text
atpd_error_snapshot
```

读取。

---

# 21. 不要再同步两份 counter

不要：

```text
ATPD_ERROR(...)
同时
g_atpd_ctx.error_count++
```

否则迟早漂移。

---

# 22. `atpd_error_code_t` 中有 obsolete eBPF code

当前：

```text
ATPD_ERR_EBPF_INIT
ATPD_ERR_EBPF_RELOAD
```

根据当前架构：

```text
sing-box owns ebpf-in
ATPD删除自身 eBPF module
```

这两个错误码也应删除。

---

# 23. `APP_FILTER / GEOIP_UPDATE` 也要 callsite audit

当前 enum还有：

```text
ATPD_ERR_APP_FILTER
ATPD_ERR_GEOIP_UPDATE
```

但当前 branch的实际 architecture已经变化很多。

Codex必须全仓搜索：

```text
ATPD_ERR_
```

列出：

```text
defined
used
dead
replacement
```

不要长期保留历史 shell-era错误码。

---

# 24. 错误码应该围绕当前 subsystem

更合理的长期分类：

```text
CONFIG
SERVICE
REACTOR
NETLINK
API
IPC
SESSION
VALIDATION
IO
MEMORY
PERMISSION
TIMEOUT
INTERNAL
```

不要过度细分到：

```text
每个函数一个 code
```

---

# 25. Error code与 errno要分开

当前 entry只有：

```text
atpd_error_code_t code
char msg[]
```

很多系统错误真正关键的是：

```text
errno
```

例如：

```text
EPERM
ENOENT
EMFILE
ENOMEM
EADDRINUSE
```

目前只能塞进 message。

---

# 26. 推荐 entry增加 `sys_errno`

例如：

```c
typedef struct {
    atpd_error_code_t code;
    int sys_errno;
    ...
} atpd_error_entry_t;
```

没有 errno：

```text
0
```

---

# 27. 为什么不要只保存 strerror文本

因为 status/tests需要：

```text
typed machine-readable errno
```

同时 strerror会受 locale影响。

---

# 28. Macro可以分两种

例如：

```c
#define ATPD_ERROR(code, msg) ...
#define ATPD_ERROR_ERRNO(code, err, msg) ...
```

不要在 macro内部无条件读：

```text
errno
```

因为 caller可能已经执行了别的函数导致 errno变化。

---

# 29. 最好 caller显式 capture errno

例如：

```c
int saved_errno = errno;
ATPD_ERROR_ERRNO(ATPD_ERR_IO, saved_errno, "pipe2 failed");
```

最稳。

---

# 30. `file` / `func` 参数没有 NULL保护

当前：

```c
strncpy(entry->file, file, ...)
strncpy(entry->func, func, ...)
```

如果 public API caller传：

```text
NULL
```

会 crash。

宏正常不会。

但 public function contract应 defensive。

---

# 31. 推荐：

```text
file ? file : ""
func ? func : ""
msg必须 nonnull
```

---

# 32. `__FUNCTION__` 建议改 `__func__`

标准 C99/C11：

```c
__func__
```

比 compiler extension：

```c
__FUNCTION__
```

更标准。

项目是 C11，直接用：

```text
__func__
```

---

# 33. `timestamp` 用 `time(NULL)` 是 wall-clock

对于日志展示：

```text
wall time
```

合理。

但如果要判断：

```text
error age
time since last error
```

wall clock会被系统时间调整影响。

---

# 34. 推荐同时保存 monotonic timestamp

例如：

```c
time_t wall_time;
uint64_t monotonic_ms;
```

status：

```text
last error age
```

用 monotonic。

renderer：

```text
human timestamp
```

用 wall time。

---

# 35. 如果要保持 struct小

128 entries × ~400 bytes已经约几十KB。

需要注意 ATPD低内存目标。

不一定值得每条都双 timestamp。

---

# 36. 更节省的方案

每 entry保存：

```text
uint64_t monotonic_ms
```

如果 human wall timestamp不必要，

可以不保存 wall。

但当前日志本身已经有 logger timestamp。

所以 ring主要用于 runtime diagnostics：

```text
monotonic更有价值
```

---

# 37. Error ring内存预算

当前每 entry大约：

```text
code 4
msg 256
file 64
line 4
func 64
timestamp 8
≈ 400 bytes
```

128 entries：

```text
约 50 KB
```

对目标 RSS ~2–3 MB来说不算巨大，

但不是完全免费。

---

# 38. File路径64字节可能被截断

`__FILE__` 很可能：

```text
src/foo.c
```

短。

问题不大。

func 64也基本够。

---

# 39. 如果只需要最近错误

128条是否需要要看 status需求。

可以保留。

不建议为了省几十KB贸然缩到8条，

除非资源测试证明有必要。

---

# 40. Error ring不应该当“日志系统”

logger负责：

```text
完整日志
```

error ring只应该用于：

```text
最近重要错误 snapshot
health/status/debug
```

所以不要把每个普通 EAGAIN之类都 push进去。

---

# 41. 必须定义哪些错误进入 ring

建议：

```text
需要 operator/actionable 的错误
状态降级
启动/重载失败
subsystem fatal
unexpected invariant
```

不要：

```text
普通 peer disconnect
expected timeout retry
normal cancellation
```

全部写 ring。

---

# 42. 否则 ring会被噪声覆盖

比如：

```text
UDS peer disconnect
session EPIPE
```

高频情况下128条很快全是同类噪声。

真正：

```text
service restart failure
```

反而被挤掉。

---

# 43. 可以加 severity 吗

如果 logger已经有 severity，

error ring可以只存：

```text
ERROR级别
```

暂时不需要再造：

```text
error severity enum
```

除非 status明确需要：

```text
WARN vs ERROR
```

---

# 44. 更重要的是 dedup/rate-limit

高频同类错误：

```text
same code + same subsystem
```

可能连续刷。

长期可以支持：

```text
repeat_count
```

但不是第一阶段必须。

---

# 45. 第一阶段不要过度设计 error registry

先保证：

```text
correctness
single owner
snapshot
errno
```

即可。

---

# 46. Error ring的 `head/tail/count` 用 int

最大128，

没问题。

但 API index建议：

```text
size_t
```

更自然。

---

# 47. `total_count` overflow理论上可以忽略

uint64_t足够。

不需要 saturating counter。

---

# 48. `error_code_string()` 是 static

status如果需要 string：

现在只能：

```text
重新switch
```

或者拿不到。

---

# 49. 推荐导出纯函数

```c
const char *atpd_error_code_string(atpd_error_code_t code);
```

这样：

```text
status/json/log
```

共用一个映射。

---

# 50. 但不要返回可变 storage

固定 string literal安全。

---

# 51. `atpd_error_print_all()` 是否还需要

如果未来 CLI/status renderer已经能：

```text
snapshot → render
```

这个函数会重复 presentation逻辑。

建议 callsite audit。

---

# 52. 如果只有 debug用途

可以删除：

```text
atpd_error_print_all()
```

让：

```text
status/ui
```

负责 render。

---

# 53. Error subsystem最好只管理数据

理想：

```text
push
snapshot
count
total
code string
```

不负责：

```text
print all
UI formatting
```

这更符合分层。

---

# 54. `LOG_ERROR()` 放在 push里是否应该保留

这取决于 API contract。

当前：

```text
ATPD_ERROR()
```

意味着：

```text
record + log
```

这很方便。

但可能导致 caller已经：

```text
LOG_ERROR
```

后又：

```text
ATPD_ERROR
```

重复日志。

---

# 55. 必须全仓检查 usage pattern

搜索：

```text
LOG_ERROR
ATPD_ERROR
```

看是否经常：

```text
LOG_ERROR(...)
ATPD_ERROR(...)
```

如果是：

会双报。

---

# 56. 两种模型选一个

### 模型 A

```text
ATPD_ERROR = record + log
```

caller不再额外 LOG_ERROR。

### 模型 B

```text
atpd_error_record = state only
```

caller自己 log。

---

# 57. 推荐模型 A

对于关键错误：

```text
一个调用
→ ring + log
```

减少漏报。

但函数名更准确：

```text
atpd_error_report
```

---

# 58. 如果叫 `push`

看起来只是：

```text
放入 ring
```

却带有：

```text
LOG_ERROR副作用
```

名字不够清楚。

推荐：

```c
void atpd_error_report(...);
```

---

# 59. 或保留内部 `push_locked`

```text
report()
├─ ring insert
└─ log outside lock
```

---

# 60. Error report不能递归

如果 logger本身失败：

```text
logger → atpd_error_report
```

而 report又：

```text
logger
```

就递归。

所以要明确：

> logger subsystem内部不能通过普通 `ATPD_ERROR` 报告 logger自身写日志失败。

---

# 61. Logger自身错误应 fallback stderr/internal counter

不要进同一 logging cycle。

这需要文档说明。

---

# 62. 与 logger方案联动

以后 review logger时重点确认：

```text
logger是否调用 atpd_error
```

如果有：

需要打破递归。

---

# 63. `atpd_error_init()` 使用 mutex后 memset entries

没问题。

但如果另一个线程还持有 getter返回的内部指针：

即使锁保护，

pointer仍会失效。

copy-out改完后这个问题消失。

---

# 64. `clear` 同样

copy-out以后：

```text
历史 snapshot
```

caller自己的 copy不受 clear影响。

语义清晰。

---

# 65. Snapshot需要按时间顺序

当前 ring indexing：

```text
head oldest
tail next write
```

snapshot应输出：

```text
oldest → newest
```

或者：

```text
newest → oldest
```

必须固定。

推荐：

```text
newest-first
```

更符合 status最近错误。

---

# 66. 但 API index如果保留

当前：

```text
index 0 = oldest
```

最好保持兼容，

或者明确变更。

---

# 67. Status更可能只要 last N

可以：

```c
int atpd_error_snapshot_recent(out, max_entries);
```

避免复制128条。

但当前50KB copy也不大。

不必过度优化。

---

# 68. 更适合的是专门 last snapshot

```c
bool atpd_error_get_last(atpd_error_entry_t *out);
```

status summary只用这一条。

详细 debug才全 snapshot。

---

# 69. JSON status需要 machine-readable字段

建议：

```text
code
code_name
message
errno
age_ms
```

file/function/line可以：

```text
debug模式
```

不一定默认输出。

---

# 70. 不要暴露源码路径给普通用户状态

如果 binary build里：

```text
__FILE__绝对路径
```

可能泄露 build path。

当前大概率相对路径，

但 renderer仍应把：

```text
file/line/func
```

视为 debug字段。

---

# 71. 可以在 compile时确保 basename

例如 macro：

```text
__FILE_NAME__
```

不是标准。

不需要为了这个做复杂处理。

---

# 72. Error message可能包含 secret

例如 API/config errors。

error ring会长期保留 message。

所以 caller必须确保：

```text
secret/token/password
```

不进入 msg。

---

# 73. 尤其 Native API Bearer secret

绝不能：

```text
ATPD_ERROR(... formatted config line ...)
```

直接保存 secret。

---

# 74. 错误系统本身可以不做通用 redaction

因为无法理解所有 domain。

由 owner module在构造 message时保证。

但测试应覆盖关键 secret。

---

# 75. Context last error migration

Codex必须搜索：

```text
last_error
error_count
errors_total
atpd_error_
ATPD_ERROR
```

列出所有 duplicate update。

---

# 76. 迁移原则

如果模块已经：

```text
ATPD_ERROR(...)
```

就删除：

```text
context error counter
```

更新。

如果模块只：

```text
context last_error = ...
```

改为：

```text
ATPD_ERROR
```

---

# 77. 不要每次 logger ERROR都自动进入 ring

logger只是输出级别。

error ring要有明确语义。

例如：

```text
debug tool输出失败
```

可能 LOG_ERROR但不值得影响 daemon health。

---

# 78. Error count是否用于 health要谨慎

```text
total_count > 0
```

不能说明 daemon degraded。

因为历史上任何一次 recoverable错误都会永久让 total>0。

---

# 79. Health应该依赖当前 subsystem state

例如：

```text
service FAILED
netlink DEGRADED
API UNAVAILABLE
```

而不是：

```text
曾经出现过错误
```

error ring只是诊断。

---

# 80. `last_error` 也不能作为唯一 health truth

例如：

```text
API曾失败
后来恢复
```

last error仍存在。

status应该显示：

```text
API healthy
last error 3 min ago
```

而不是：

```text
FAILED
```

---

# 81. 因此 error subsystem与 state subsystem必须分开

```text
state = current truth
error ring = historical diagnostics
```

非常重要。

---

# 82. 与 service方案联动

service应保留：

```text
current state
last failure reason
```

error ring可以记录重大 transition失败。

不要把 service runtime state完全塞进 error ring。

---

# 83. 与 config reload联动

reload失败：

```text
ATPD_ERR_CONFIG_RELOAD
```

记录历史。

但因为 old config继续运行：

```text
daemon runtime保持 RUNNING
```

这是前面已确定的语义。

---

# 84. 与 async_validate联动

validation失败需要区分：

```text
user config invalid
timeout
exec failure
internal IO
```

不是全部：

```text
ATPD_ERR_CONFIG_LOAD
```

可以加入：

```text
ATPD_ERR_VALIDATION
```

或复用 TIMEOUT/IO。

---

# 85. 与 reactor联动

真正 fatal：

```text
epoll failure
registration invariant
```

建议有：

```text
ATPD_ERR_REACTOR
```

当前 enum没有。

---

# 86. 与 session联动

普通：

```text
peer EPIPE
VPN teardown
```

不应刷 error ring。

真正：

```text
reactor modify failure
GC invariant
refcount violation
```

可以：

```text
ATPD_ERR_SESSION / INTERNAL
```

---

# 87. 与 UDS联动

普通 client错误：

```text
bad command
unauthorized peer
disconnect
```

不一定进入 ring。

listener fatal：

```text
bind/register failure
```

才应该。

---

# 88. 与 netlink联动

```text
ENOBUFS / overrun
```

如果自动 resync成功：

可能 WARN+metric。

持续 degraded：

```text
ATPD_ERROR
```

不要每个 packet parser问题都刷。

---

# 89. 推荐 enum清理方式

第一阶段不要一下重编号。

因为可能：

```text
tests/status JSON
```

依赖数值。

Codex先搜所有序列化/比较。

---

# 90. 如果 code只是内部

可以自由整理 enum。

如果对外JSON已暴露 numeric code：

尽量：

```text
显式赋值
```

保持兼容。

---

# 91. 推荐新的 enum草案

例如：

```c
typedef enum {
    ATPD_ERR_NONE = 0,
    ATPD_ERR_CONFIG,
    ATPD_ERR_VALIDATION,
    ATPD_ERR_SERVICE,
    ATPD_ERR_REACTOR,
    ATPD_ERR_NETLINK,
    ATPD_ERR_API,
    ATPD_ERR_IPC,
    ATPD_ERR_SESSION,
    ATPD_ERR_TIMEOUT,
    ATPD_ERR_MEMORY,
    ATPD_ERR_IO,
    ATPD_ERR_PERMISSION,
    ATPD_ERR_INTERNAL
} atpd_error_code_t;
```

---

# 92. 不一定要一次合并 CONFIG_LOAD/RELOAD

如果 status/operator区分有价值：

可以保留。

核心是：

```text
去 stale code
补 current subsystem
```

---

# 93. Test：concurrent push/get

至少：

```text
4 writer threads
4 reader threads
100k operations
```

TSan：

```text
0 race
```

---

# 94. Test：returned snapshot稳定

```text
get_last(copy A)
↓
push 200 new errors
↓
copy A内容不变
```

这是旧 pointer API的直接 regression test。

---

# 95. Test：clear race

writers/readers同时：

```text
clear
```

不 crash/data race。

但 production是否允许 runtime clear要明确。

---

# 96. Test：ring wrap

写：

```text
ATPD_ERROR_MAX + 100
```

验证：

```text
count == ATPD_ERROR_MAX
total_count == ATPD_ERROR_MAX + 100
oldest被覆盖
顺序正确
```

---

# 97. Test：clear保留 total

如果保留当前语义：

```text
push 10
clear
count=0
total=10
```

需要显式 test。

---

# 98. Test：init/reset total

startup reset：

```text
total=0
```

---

# 99. Test：NULL metadata

直接调用：

```text
file=NULL
func=NULL
```

不能 crash。

---

# 100. Test：long message/path/function

确保：

```text
NUL termination
no OOB
```

---

# 101. Test：errno

```text
sys_errno=ENOENT
```

snapshot中仍保留：

```text
ENOENT numeric
```

message可另外 render strerror。

---

# 102. Test：lock ordering / recursive logger

最好做一个 logger mock：

```text
LOG callback attempts error snapshot
```

确认：

```text
no deadlock
```

最简单方式就是：

```text
任何 logger call都在 error mutex外
```

---

# 103. Test：snapshot while wrap

持续 writer覆盖 ring，

reader snapshot：

```text
内部顺序一致
每 entry完整
```

不能出现：

```text
code来自新 entry
message来自旧 entry
```

copy under one lock即可。

---

# 104. Test：secret redaction

针对 API/config重要路径：

```text
error snapshot
```

不含：

```text
Bearer secret
API secret
```

---

# 105. Test：context duplication清零

最终 grep：

```text
g_atpd_ctx.*error
last_error
errors_total
error_count
```

确认没有第二份 authoritative error state。

---

# 106. 推荐 Commit 1

```text
error: replace borrowed ring pointers with copy-out snapshots
```

内容：

- get/get_last锁内校验
- copy-out
- full snapshot
- concurrency tests

---

# 107. Commit 2

```text
error: move logging outside ring mutex
```

消除：

```text
error mutex → logger
```

锁嵌套。

---

# 108. Commit 3

```text
error: add typed errno and standard source metadata
```

- sys_errno
- `__func__`
- NULL-safe metadata

---

# 109. Commit 4

```text
error: remove obsolete codes and align with current subsystems
```

配合：

```text
eBPF module removal
```

---

# 110. Commit 5

```text
context: remove duplicate error state
```

让：

```text
atpd_error
```

成为唯一历史错误 owner。

---

# 111. Commit 6

```text
status: consume immutable error snapshots
```

status：

```text
last error
recent errors
age
```

只读。

---

# 112. Commit 7 可选

```text
error: remove presentation helpers
```

如果：

```text
atpd_error_print_all
```

没有必要，

让 renderer层负责。

---

# 113. 不建议拆文件

127行代码非常小。

重构后也大概率：

```text
150–220 LOC
```

继续：

```text
atpd_error.c/h
```

即可。

---

# 114. 推荐最终 API

例如：

```c
void atpd_error_report(
    atpd_error_code_t code,
    int sys_errno,
    const char *msg,
    const char *file,
    int line,
    const char *func);

bool atpd_error_get_last(atpd_error_entry_t *out);
size_t atpd_error_count(void);
uint64_t atpd_error_total(void);
int atpd_error_snapshot(atpd_error_snapshot_t *out);

const char *atpd_error_code_string(atpd_error_code_t code);
```

---

# 115. Macro

```c
#define ATPD_ERROR(code, msg) \
    atpd_error_report((code), 0, (msg), __FILE__, __LINE__, __func__)

#define ATPD_ERROR_ERRNO(code, err, msg) \
    atpd_error_report((code), (err), (msg), __FILE__, __LINE__, __func__)
```

---

# 116. 不建议 varargs error macro第一阶段就加

例如：

```text
ATPD_ERRORF
```

虽然方便，

但容易：

```text
stack formatting
secret leakage
复杂宏
```

当前 char buffer/message足够。

---

# 117. 如果需要 formatted report

可以提供普通函数：

```c
atpd_error_reportf(...)
```

内部：

```text
vsnprintf bounded
```

但不是必要。

---

# 118. Error snapshot不是 configuration/state snapshot替代品

最终 architecture：

```text
service snapshot
netlink snapshot
API snapshot
config snapshot
runtime snapshot
     ↓
current state

error snapshot
     ↓
historical diagnostics
```

二者不能混。

---

# 119. 最终 Invariants

Codex最终必须保证：

```text
I1:
No error getter reads mutable ring state outside the ring mutex.

I2:
No public API returns a pointer into mutable ring storage.

I3:
Logging never runs while holding the error ring mutex.

I4:
The error subsystem is the only authoritative owner of historical error count/history.

I5:
Current daemon health is not inferred solely from historical error presence.

I6:
Obsolete ATPD-owned eBPF error codes are removed.

I7:
System errno is preserved as typed data where relevant.

I8:
Error messages never contain secrets.

I9:
Ring wrap/clear/snapshot are race-free.

I10:
atpd_error remains a small diagnostics subsystem, not another global state database.
```

---

# 120. 最终验收标准

## TSan

```text
concurrent push/get/snapshot/clear
→ 0 races
```

## Ring

```text
wrap正确
order正确
total正确
```

## Snapshot

```text
returned data immutable after unlock
```

## Locking

```text
no logger calls under ring mutex
```

## Ownership

```text
context no duplicate last_error/error counters
```

## Error taxonomy

```text
no ATPD-owned eBPF error codes
current subsystem codes complete enough
```

## Security

```text
no Native API/config secrets in error ring
```

---

# 121. 最终结论

`atpd_error.c` 当前最大的风险不是错误码设计，而是两个 API correctness问题：

```text
1. get/get_last 在加锁前读取 count
2. getter解锁后返回 ring内部指针
```

第二个尤其重要，因为它让 caller拿到一个：

```text
看起来 const
实际上随下一次 ring覆盖而变化
```

的对象。

所以优先级应是：

```text
copy-out snapshot
↓
logger移出锁
↓
errno typed
↓
删除 context重复错误状态
↓
清理 obsolete eBPF/error codes
```

这个模块不需要复杂化。

最终只需要成为：

> 一个小型、线程安全、不可借出内部存储、只负责“历史诊断”的 error ring。

当前 health/state truth仍应留在各 subsystem snapshot，而不是由“最近是否报过错”决定。
