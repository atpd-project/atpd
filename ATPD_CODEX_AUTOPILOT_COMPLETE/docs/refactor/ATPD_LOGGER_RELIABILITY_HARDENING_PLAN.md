# ATPD `logger.c / logger.h` 日志可靠性与安全边界加固方案

## 1. 结论

当前：

```text
src/logger.c      ~304 lines
include/logger.h  ~129 lines
```

整体评价：

```text
结构清晰
文件打开安全意识较好
mutex保护主要 mutable logger state
已有 rotation
Android log兼容
```

不建议拆文件，也不建议引入复杂异步 logging framework。

本轮目标是：

> 保留当前简单同步 logger，修掉边界错误、假配置、初始化错误吞掉、锁外 data race 和 shutdown/rotation 语义问题。

---

# 2. 当前做得好的部分

文件打开使用：

```c
O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC
```

并在 `open()` 后：

```c
fstat(fd, &st)
```

验证：

```text
regular file
st_nlink == 1
```

这比普通：

```text
fopen(path, "a")
```

安全得多。

这些保护应保留。

---

# 3. P0/P1：log level 可以导致数组越界

当前：

```c
static const char *level_strings[] = {
    [DEBUG] = ...,
    [INFO] = ...,
    [WARN] = ...,
    [ERROR] = ...,
    [FATAL] = ...
};
```

只有：

```text
0..4
```

但 enum 还有：

```c
LOG_LEVEL_NONE = 5
```

而 public：

```c
log_write(level,...)
log_write_v(level,...)
log_set_level(level)
```

都没有验证 level。

---

# 4. 危险路径

例如：

```c
log_set_level(LOG_LEVEL_DEBUG);
log_write(LOG_LEVEL_NONE, ...);
```

第一层判断：

```c
if (level < g_log_config.min_level) return;
```

不会挡住：

```text
5 < 0 = false
```

然后：

```c
level_strings[level]
```

访问：

```text
level_strings[5]
```

越界。

---

# 5. 任意非法 enum更危险

例如：

```text
level = 100
level = -1
```

都可能：

```text
OOB read
```

public API必须验证。

---

# 6. 推荐统一 validator

```c
static bool log_level_is_emittable(log_level_t level)
{
    return level >= LOG_LEVEL_DEBUG &&
           level <= LOG_LEVEL_FATAL;
}
```

`LOG_LEVEL_NONE`：

```text
只能用于 min_level
不能用于 emit
```

---

# 7. `log_set_level()` 也要校验

合法 threshold：

```text
DEBUG
INFO
WARN
ERROR
FATAL
NONE
```

非法值：

```text
ignore/error
```

不要写入 global config。

---

# 8. `log_get_level()` 当前锁外读取

当前：

```c
log_level_t log_get_level(void) {
    return g_log_config.min_level;
}
```

而 setter：

```text
mutex内写
```

如果支持多线程：

这就是 data race。

---

# 9. `log_write()` / `log_write_v()` 的 early level check同样锁外

当前两个函数都有：

```c
if (level < g_log_config.min_level) return;
```

与此同时：

```text
log_set_level()
```

可能修改 min_level。

所以：

```text
read outside mutex
write inside mutex
```

在 C memory model下并不安全。

---

# 10. 不要因为字段很小就假设原子

enum/int普通读写：

```text
硬件层面可能是原子的
```

但 C 层仍然可能构成：

```text
data race / undefined behavior
```

---

# 11. 两种修法

### 方案 A：全部通过 mutex

最简单：

```text
进入 log_write
lock
读 min_level
...
unlock
```

但 formatting和 Android log placement要设计好。

### 方案 B：min_level 用 C11 atomic

例如：

```c
_Atomic int min_level;
```

如果 log level runtime经常读、很少写：

这是更合理的。

---

# 12. 推荐方案 B

只把：

```text
min_level
```

作为 atomic threshold。

其他 config：

```text
targets
file
rotation
color
```

继续 mutex保护。

这样 hot path可以：

```text
atomic_load(min_level)
→ early return
```

避免每条 filtered log都拿 mutex。

---

# 13. 但不要把整个 config 全部 atomic化

字符串/path/FILE*：

```text
仍然必须由 mutex统一保护
```

---

# 14. `enable_timestamp` 是假配置

`log_config_t` 包含：

```c
int enable_timestamp;
```

默认：

```text
1
```

但实际 `log_write_v()`：

```text
始终 get_timestamp()
始终输出 timestamp
```

没有任何地方读取：

```text
enable_timestamp
```

---

# 15. 二选一

如果产品总是需要 timestamp：

```text
删除 enable_timestamp
```

更推荐。

如果确实需要 toggle：

```text
真正实现
```

不要保留无效配置字段。

---

# 16. `LOG_TARGET_SYSLOG` 同样是假 target

header定义：

```c
LOG_TARGET_SYSLOG = 1 << 2
```

但实现只处理：

```text
FILE
STDERR
```

没有：

```text
syslog
```

---

# 17. 假 target比缺功能更危险

caller可能：

```c
log_set_targets(LOG_TARGET_SYSLOG);
```

以为日志会进入 syslog，

实际：

```text
Linux上什么都不输出
```

Android路径又可能单独输出到 logcat。

---

# 18. 推荐删除未实现 `LOG_TARGET_SYSLOG`

除非项目确实马上实现：

```text
syslog()
```

当前 Android/Linux daemon没有必要为了 API表面完整保留它。

---

# 19. Android logging target语义不清

当前 Android：

```text
只要编译在 Android
且 liblog可加载
→ 每条 log都调用 __android_log_print
```

它不受：

```text
g_log_config.targets
```

控制。

所以实际 target是：

```text
implicit logcat
+
configured stderr/file
```

---

# 20. 这会产生意外重复日志

例如 caller设置：

```text
FILE only
```

仍然：

```text
logcat + file
```

如果设计就是如此：

必须文档明确。

更推荐：

```text
LOG_TARGET_ANDROID
```

显式 target。

---

# 21. Android target最好不要复用 SYSLOG

定义：

```c
LOG_TARGET_ANDROID = 1 << 2
```

Android build启用。

Linux则忽略/拒绝。

比“syslog但其实logcat”更清楚。

---

# 22. `atpd_log_init()` 不是线程安全的

当前：

```c
if (atpd_log_initialized) return;
atpd_log_initialized = 1;
dlopen...
dlsym...
```

多个线程第一次同时 log：

可能 data race。

---

# 23. 推荐 `pthread_once`

```c
static pthread_once_t g_android_log_once = PTHREAD_ONCE_INIT;
```

然后：

```text
pthread_once(...)
```

---

# 24. `dlopen` handle没有保存/关闭

当前：

```text
void *handle = dlopen(...)
dlsym(...)
```

handle没有：

```text
dlclose
```

---

# 25. 这不是重大泄漏

因为：

```text
process lifetime只初始化一次
```

保留 loaded liblog也是合理的。

但应该：

```text
保存 handle并注明 process-lifetime ownership
```

或者：

```text
不打算 dlclose
```

明确注释。

---

# 26. `log_init()` 吞掉所有初始化失败

当前：

```text
atp_timezone_init()
get_app_dir(...)
mkdir_recursive(...)
open log file...
```

所有 return value基本都忽略。

函数本身：

```c
void log_init(void)
```

所以 caller无法知道：

```text
日志文件不可写
app dir失败
run dir失败
path截断
```

---

# 27. 这与 init transaction方案冲突

我们前面要求 logger phase必须明确：

```text
file log失败
→ stderr fallback?
还是 startup fatal?
```

当前完全无法判断。

---

# 28. 推荐 `logger_init()` 返回结果

例如：

```c
typedef enum {
    LOGGER_INIT_OK = 0,
    LOGGER_INIT_DEGRADED,
    LOGGER_INIT_FAILED
} logger_init_result_t;
```

或者简单：

```c
int logger_init(...)
```

并通过 snapshot/status记录：

```text
file target unavailable
stderr still active
```

---

# 29. 对 ATPD推荐策略

日志文件打开失败：

```text
不必让 daemon完全启动失败
```

只要：

```text
stderr/logcat可用
```

可以：

```text
DEGRADED + stderr fallback
```

但必须 truthful。

---

# 30. 如果所有输出 target都不可用

例如 daemonized：

```text
stderr不可用
file打开失败
Android log不可用
```

那才更接近：

```text
required startup failure
```

实际可根据部署方式决定。

---

# 31. `log_open_file_unlocked()` 返回 void导致错误消失

当前：

```text
open fail → return
fstat fail → return
fdopen fail → return
```

没有：

```text
errno
reason
```

---

# 32. 推荐返回 typed/bool result

例如：

```c
static int log_open_file_unlocked(int *saved_errno);
```

注意：

> logger自身失败不能用普通 `ATPD_ERROR` 报告。

否则可能：

```text
atpd_error_report
→ LOG_ERROR
→ logger failure
→ atpd_error_report
```

递归。

---

# 33. Logger自身错误只走 fallback

建议：

```text
write direct stderr
or Android log
or internal logger status
```

不能进入常规 error-report→logger循环。

---

# 34. 与 `atpd_error` 的锁顺序要求

我们刚刚要求：

```text
error ring mutex外再 LOG_ERROR
```

logger也不要反过来：

```text
持 logger mutex
→ atpd_error_report
```

最终应保证：

```text
error subsystem
logger subsystem
```

不存在双向锁调用。

---

# 35. `get_app_dir()` 返回值被忽略

当前：

```c
char app_dir[PATH_MAX];
get_app_dir(app_dir, sizeof(app_dir));
snprintf(...)
```

如果 `get_app_dir()` 失败且没有可靠初始化 buffer：

可能使用错误内容。

目前 utils实现还会 fallback `"."`，

但我们已经计划删除这个危险 fallback。

---

# 36. 所以 logger必须处理 app_dir失败

例如：

```text
explicit log path from config
```

优先。

否则：

```text
file target unavailable
stderr fallback
```

不要默默写当前目录。

---

# 37. `log_path` 使用 PATH_MAX+32，但 config只保存256字节

当前：

```text
char log_path[PATH_MAX + 32]
```

然后：

```c
strncpy(g_log_config.log_file,
        log_path,
        sizeof(g_log_config.log_file)-1);
```

而：

```text
log_file[256]
```

---

# 38. 这会静默截断长路径

结果可能：

```text
原路径 /very/long/.../atpd.log
↓
截断成另一个路径
```

然后 open：

```text
错误位置
或失败
```

---

# 39. 推荐 `log_file` 至少 PATH_MAX

或者更好：

```text
logger_init传入已验证路径
```

内部：

```text
PATH_MAX
```

并明确拒绝超长。

---

# 40. 所有 `snprintf(path)` 都要检查 truncation

包括：

```text
log_path
run_dir
rotation .N paths
```

当前都没有检查。

---

# 41. Rotation path截断尤其危险

如果：

```text
path接近 PATH_MAX
```

拼：

```text
.1
.2
```

可能被截断，

导致 rename目标错误。

必须：

```text
snprintf result >= sizeof
→ abort rotation safely
```

---

# 42. Rotation错误全部被忽略

当前：

```c
rename(...)
```

返回值不检查。

所以：

```text
permission
filesystem readonly
target conflict
I/O error
```

都静默。

然后重新 open primary log。

---

# 43. Rotation失败不应导致 logging停止

正确策略：

```text
尝试rotate
失败
→ 保持/重新打开原log
→ mark logger degraded
→ fallback report
```

不要：

```text
recursive LOG_ERROR
```

---

# 44. `access()` + `rename()` 是 TOCTOU

当前：

```c
if (access(old_path, F_OK) == 0)
    rename(old_path,new_path);
```

完全不需要先：

```text
access
```

直接：

```text
rename
```

处理：

```text
ENOENT
```

即可。

---

# 45. 删除 access可减少 race

同时代码更简单：

```text
rename old→new
if ENOENT ignore
else error
```

---

# 46. Rotation的 symlink安全边界

主文件 open：

```text
O_NOFOLLOW
```

很好。

但：

```text
rename(path, path.1)
```

是 pathname操作。

安全性主要依赖：

```text
log directory ownership/permissions
```

---

# 47. 必须保证 log dir不是攻击者可写

如果 log目录：

```text
root-owned
not world-writable
```

当前设计合理。

如果可能在共享目录：

rotation需要更强：

```text
dirfd + renameat/fstatat
```

ATPD部署下建议优先保证目录权限。

---

# 48. 当前 `run_dir` mkdir 与 log file目录并不一定一致

`log_path`：

```text
app_dir / ATP_LOG_FILE
```

但创建的是：

```text
app_dir/run
```

必须检查：

```text
ATP_LOG_FILE
```

到底位于：

```text
run/?
logs/?
root?
```

如果不是 run目录：

这个 mkdir可能与日志无关。

---

# 49. Codex应确认实际 macro

搜索：

```text
ATP_LOG_FILE
ATP_RUN_DIR
```

不要假设。

如果 log目录需要创建：

logger应创建：

```text
dirname(log_path)
```

或 startup platform owner提前创建。

---

# 50. `g_current_log_size` 只按自己写入字节更新

如果其他进程：

```text
truncate
append
rotate
```

logger的：

```text
current size
```

会漂移。

ATPD通常单 writer，

可以接受。

---

# 51. 但外部 logrotate可能造成问题

如果：

```text
path被rename + new file created
```

ATPD仍然写：

```text
旧 open fd
```

直到自己的 rotation/reopen。

如果产品支持外部 logrotate：

需要：

```text
SIGHUP reopen
inode check
```

---

# 52. 当前不建议同时支持两套 rotation

最简单：

```text
ATPD owns its own rotation
```

文档明确：

```text
不要外部 rotate
```

或者：

```text
disable internal rotation
```

交给系统。

不要半支持。

---

# 53. Rotation发生在 `log_write()` mutex内

当：

```text
file size达到 max
```

普通 log call会现场：

```text
fflush
fclose
多个 rename
open
fdopen
setvbuf
```

---

# 54. 如果调用者是 reactor thread

这会直接增加 event-loop latency。

这是同步 logger不可避免的一部分。

---

# 55. 需不需要做 async logger？

当前不推荐。

理由：

```text
代码复杂度
额外线程
queue ownership
shutdown flush
log loss/backpressure
```

对于 ATPD这种小 daemon，

先保持同步更稳。

---

# 56. 但 rotation应避免在高频 hot path做太多工作

可以：

```text
写入时只 mark rotation_needed
```

然后：

```text
下一次 maintenance/idle
```

做 rotate。

但这要求额外 orchestration。

---

# 57. 第一阶段可保持同步 rotation

前提是：

```text
rotate_count很小
文件系统正常
日志不高频
```

并做 latency benchmark。

---

# 58. 推荐测试 reactor latency

制造：

```text
高频日志
每100KB rotation
rotate_count=5
```

同时测：

```text
reactor timer p99
UDS ping p99
```

如果明显抖动：

再考虑 deferred rotation。

---

# 59. 不要默认每条 log `fflush`

当前设置：

```c
setvbuf(..., _IOLBF, 0)
```

每行通常 flush。

这有助于 crash前日志可见，

但增加 IO syscall。

---

# 60. 对 daemon这是合理权衡

特别是：

```text
错误诊断
root service
```

不建议为了 benchmark盲目改成全缓冲。

---

# 61. 但 shutdown必须明确 flush成功/失败语义

当前：

```text
fflush
fclose
```

返回值都忽略。

正常 shutdown：

至少应该：

```text
best-effort flush
```

如果失败：

不能再用 logger报自己。

可以：

```text
direct stderr
```

---

# 62. `logger_close()` 应幂等

当前：

```text
g_log_fp == NULL
```

时安全。

这一点不错。

---

# 63. `log_init()` 重复调用会 close/reopen

当前允许：

```text
log_init
log_init
```

不会直接 crash，

但会：

```text
重复 timezone init
重算 app dir
关闭/重开日志
```

---

# 64. 与 init one-shot方案一致

logger init应该：

```text
startup exactly once
```

reload只调用：

```text
logger_apply_config
```

不能重新 init整个 logger。

---

# 65. `logger_init` 与 `log_init` 完全重复

当前：

```c
void logger_init(void) {
    log_init();
}
```

对外同时暴露：

```text
log_init
logger_init
```

没有必要。

---

# 66. 推荐只保留一个 public init

例如：

```c
int logger_init(const logger_config_t *cfg);
```

内部 helper可以：

```text
logger_open...
```

删除 `log_init()` public alias。

---

# 67. `log_set_target` 与 `log_set_targets` 也是重复 API

当前：

```text
log_set_targets(uint32_t)
→ log_set_target(int)
```

保留一个：

```text
log_set_targets(uint32_t)
```

即可。

---

# 68. Header正在积累兼容别名

类似：

```text
logger_init/log_init
log_set_target/log_set_targets
```

应该趁项目尚未稳定 public ABI时清理。

---

# 69. `LOG_DEBUG_LAZY` 等并不 lazy

当前：

```c
#define LOG_DEBUG_LAZY(...) LOG_DEBUG(...)
```

没有：

```text
callback
deferred evaluation
```

所以名字错误。

---

# 70. 例如：

```c
LOG_DEBUG_LAZY("x=%s", expensive_function());
```

仍然会：

```text
先执行 expensive_function()
```

再进入 log_write。

---

# 71. 推荐删除这些 `_LAZY` alias

如果未来真需要 lazy：

可提供：

```text
if (log_is_enabled(DEBUG))
```

让 caller自己避免昂贵计算。

---

# 72. 推荐 `log_is_enabled()`

```c
bool log_is_enabled(log_level_t level);
```

读取 atomic threshold。

这样：

```c
if (log_is_enabled(LOG_LEVEL_DEBUG)) {
    ...
}
```

---

# 73. `LOG_EXEC(cmd)` 有 secret leakage风险

它会：

```text
完整输出 command string
```

如果 command包含：

```text
API secret
token
password
config secret
```

DEBUG log会泄露。

---

# 74. 结合 utils shell runner清理

我们计划减少：

```text
shell command strings
```

因此 `LOG_EXEC` 很可能可以删除。

如果保留：

必须 caller保证已 redacted。

---

# 75. `LOG_EBPF` 应随 ATPD eBPF删除

当前 header还定义：

```c
LOG_EBPF(...)
```

根据架构：

```text
ATPD不再拥有 eBPF subsystem
```

删除。

sing-box ebpf-in错误：

```text
SERVICE/API
```

归类。

---

# 76. `LOG_ROUTE` 要确认实际 owner

如果 route subsystem已经不存在或并入 netlink：

审计 caller。

无 caller就删。

---

# 77. Category macro不要无限增长

当前：

```text
LOG_SERVICE
LOG_API
LOG_ROUTE
LOG_NETLINK
LOG_REACTOR
LOG_EBPF
```

本质只是：

```text
message prefix
```

可以保留少量常用的。

无需为每个新 module加 macro。

---

# 78. 更长期可以传 component字段

如果以后做 structured JSON logs：

```text
level
component
message
```

比拼字符串：

```text
"[SERVICE]"
```

更好。

---

# 79. 当前项目不需要现在做 structured logger重写

status已经有 JSON需求，

logging未必。

先保持文本 log。

---

# 80. Timestamp与 timezone的职责

当前 `logger_init()` 主动：

```text
atp_timezone_init()
```

这让 logger拥有 timezone startup。

我们刚在 utils方案中建议：

```text
timezone subsystem明确 startup init
```

---

# 81. logger不应负责 timezone detection

正确顺序：

```text
platform/timezone init
↓
logger init
```

或者：

```text
logger使用UTC
```

但 logger内部不该隐式：

```text
getprop
tzdata parse
setenv
```

---

# 82. 所以删除：

```c
atp_timezone_init();
```

从 logger里。

由 init transaction明确调用。

---

# 83. `get_timestamp()` 失败显示全0

当前：

```text
0000-00-00 00:00:00
```

可接受，但 debug价值一般。

如果 localtime失败：

更合理可以 fallback：

```text
UTC gmtime
```

---

# 84. Timestamp是否应该带毫秒

当前秒级。

对于：

```text
reactor/session/service race
```

毫秒会更有诊断价值。

---

# 85. 推荐：

```text
YYYY-MM-DD HH:MM:SS.mmm
```

使用：

```text
clock_gettime(CLOCK_REALTIME)
```

然后 localtime转换 seconds。

---

# 86. 是否需要 monotonic字段

普通文本日志：

```text
wall clock
```

足够。

若要诊断时序：

可以同时加：

```text
+12345.678s
```

但会增加噪声。

当前不必。

---

# 87. `enable_color` 默认=1，即使 stderr不是 TTY

daemon输出重定向文件时：

会产生 ANSI escape。

---

# 88. 推荐默认自动判断

```text
isatty(STDERR_FILENO)
```

才启用 color。

显式 CLI：

```text
--color / --no-color
```

可覆盖。

---

# 89. 这对 daemon日志很实际

system service捕获 stderr时：

不应该看到：

```text
\033[31m
```

---

# 90. `log_set_color()`仍可保留

但默认 init：

```text
enable_color = isatty(stderr)
```

更合理。

---

# 91. `targets` runtime更新与 Android implicit log问题一起修

所有 sink selection：

```text
FILE
STDERR
ANDROID
```

都在同一 config语义里。

---

# 92. `log_set_file(NULL)` 当前什么都不做

如果 caller想：

```text
disable/clear file path
```

无法做到。

最好明确：

```text
NULL invalid
"" clears
```

或专门：

```text
logger_disable_file
```

---

# 93. Config reload应怎么处理 logger

建议 reloadable：

```text
min level
targets
color
max size
rotate count
file path（可选）
```

apply必须：

```text
under logger mutex
```

并保持旧 logger可用直到新 file成功打开。

---

# 94. File path reload需要 transactional semantics

不要：

```text
close old
set new
open new失败
→ no file logger
```

当前 `log_set_file()` 正是这样。

---

# 95. 正确做法

```text
open/validate new path first
↓
成功
↓
lock
swap FILE*
close old
commit path
```

如果新路径失败：

```text
旧 file sink继续工作
```

---

# 96. 这和 config transaction完全一致

logger也需要：

```text
prepare
commit
```

但不用造大型 transaction framework。

一个：

```c
logger_set_file_transactional(path)
```

就够。

---

# 97. Rotation count边界

当前：

```text
rotate_count
```

没有校验。

如果：

```text
0
negative
huge
```

for loop行为异常或耗时很大。

---

# 98. Max file size也需要边界

例如：

```text
0
```

当前每次 write后：

```text
size >= 0
→ rotate
```

每条日志都 rotation。

---

# 99. 推荐 config validator

```text
max_file_size >= sensible minimum
rotate_count 0..N
```

例如：

```text
rotate_count=0
→ no rotation
```

要定义语义。

---

# 100. 推荐 `rotate_count=0`

表示：

```text
rotation disabled
```

而不是异常。

如果 size cap仍配置：

需要明确。

更简单：

```text
rotate_count=0
→ file can grow
```

---

# 101. `level_strings[level]` 需要 static assertion吗

可以：

```text
validator
```

就够。

无需复杂。

---

# 102. `vsnprintf` truncation当前静默

MAX_LOG_MSG：

```text
1024
```

超长 message被截断。

通常合理。

但最好：

```text
明确以 "…" / [truncated]
```

标记。

---

# 103. UTF-8截断可能切断 multibyte

对日志不是安全问题。

不必为了漂亮增加复杂度。

---

# 104. `fmt == NULL` 需要 defensive check

public API如果：

```text
fmt=NULL
```

`vsnprintf` undefined/crash。

macro正常不会。

可以简单：

```text
if (!fmt) return;
```

---

# 105. `file/func` 在 location disabled时其实无需安全

当前 compile-time：

```text
LOG_LOCATION_ENABLED=0
```

不会使用。

如果 enable：

`get_file_basename(NULL)`安全，

func传给 `%s`：

```text
NULL行为implementation-specific
```

建议：

```text
func ? func : "unknown"
```

---

# 106. `__FUNCTION__` 改 `__func__`

与 error模块一致：

```text
C11 standard
```

不要用 compiler extension。

---

# 107. Test：invalid log level

依次：

```text
-1
LOG_LEVEL_NONE as emitted level
100
```

不能：

```text
OOB
crash
```

---

# 108. Test：concurrent level change + logging

多线程：

```text
log_set_level
log_write
log_get_level
```

TSan：

```text
0 race
```

---

# 109. Test：file symlink

已有相关测试可能覆盖。

继续确保：

```text
log path is symlink
→ open rejected
```

---

# 110. Test：hard link

`st_nlink != 1`：

```text
reject
```

保留。

---

# 111. Test：non-regular file

FIFO/device/socket：

```text
reject
```

---

# 112. Test：path > internal capacity

必须：

```text
explicit fail
```

不能 silent truncation。

---

# 113. Test：rotation path near PATH_MAX

`.1/.2` 拼接超限：

```text
rotation fails safely
original logging continues
```

---

# 114. Test：rename failure

mock/read-only fs：

```text
rotation failure
```

不能：

```text
logger永久关闭
```

---

# 115. Test：new log file reload failure

当前旧文件：

```text
A
```

reload到不可写：

```text
B
```

预期：

```text
A继续可用
config apply fails/degraded
```

---

# 116. Test：shutdown flush

写最后一条：

```text
shutdown marker
```

调用：

```text
logger_close
```

文件中必须出现。

---

# 117. Test：double close

```text
logger_close
logger_close
```

安全。

---

# 118. Test：double init

长期目标：

```text
第二次 init返回 already initialized / no-op
```

不能随意 reset active logger。

---

# 119. Test：stderr non-TTY

默认：

```text
无 ANSI color
```

---

# 120. Test：Android target

配置：

```text
FILE only
```

若采用 explicit target：

```text
不写 logcat
```

配置：

```text
ANDROID only
```

只写 logcat。

---

# 121. Test：logger internal failure不会进入 error recursion

模拟：

```text
open file EACCES
```

确保：

```text
无递归
无 deadlock
```

fallback直接 stderr/logcat。

---

# 122. Test：rotation latency

压力：

```text
小 max size
频繁 rotate
```

测：

```text
reactor p99
```

如果满足预算：

保持同步。

---

# 123. 推荐 Commit 1

```text
logger: validate levels and remove lock-free config races
```

优先：

- emit level range
- threshold validation
- atomic min_level或锁保护
- get_level线程安全

---

# 124. Commit 2

```text
logger: make file initialization and path failures explicit
```

- logger_init返回结果
- get_app_dir/mkdir/path truncation
- file open errno
- degraded stderr fallback

---

# 125. Commit 3

```text
logger: harden rotation and preserve logging on rotate failure
```

- no access TOCTOU
- snprintf checks
- rename return checks
- keep sink usable

---

# 126. Commit 4

```text
logger: remove fake and duplicate configuration APIs
```

删除/收敛：

```text
enable_timestamp（若永远开启）
LOG_TARGET_SYSLOG（若未实现）
log_init alias
log_set_target alias
LOG_*_LAZY aliases
LOG_EBPF
```

---

# 127. Commit 5

```text
logger: make Android logcat an explicit sink
```

仅 Android。

使用：

```text
pthread_once
```

初始化 liblog。

---

# 128. Commit 6

```text
logger: make runtime file changes transactional
```

如果 config支持 logfile reload。

---

# 129. Commit 7

```text
logger: decouple timezone initialization
```

logger只消费已经初始化好的 process timezone。

---

# 130. 不建议做的事

不要引入：

```text
logging worker thread
lock-free queue
shared-memory ring
JSON logger
remote logger
```

除非性能测试证明同步 logger真是瓶颈。

---

# 131. Logger的定位应该继续简单

```text
bounded formatting
few local sinks
safe file ownership
small synchronized state
explicit degradation
```

这对 ATPD最适合。

---

# 132. 与 init方案联动

startup：

```text
timezone/platform init
logger init
```

logger init结果：

```text
OK / DEGRADED / FAILED
```

不能再无条件 phase success。

---

# 133. 与 error方案联动

硬 invariant：

```text
logger never calls atpd_error while holding logger mutex
```

最好：

```text
logger内部完全不调用 atpd_error
```

避免递归。

---

# 134. 与 utils方案联动

logger不再依赖：

```text
get_app_dir fallback "."
隐式 timezone init
不安全 mkdir语义
```

路径由 startup/config明确提供会更干净。

---

# 135. 与 global删除方案联动

`logger.c` 当前第一行：

```c
#include "atpd_global.h"
```

但当前源码中并未实际使用：

```text
g_atpd / g_config / g_svc / ...
```

这是不必要 include。

应直接删除。

---

# 136. 这是一个很好的 dependency cleanup

最终 logger只依赖：

```text
logger/platform path/time support
```

不依赖整个 daemon global root。

---

# 137. 最终 Invariants

Codex最终必须保证：

```text
I1:
Only DEBUG..FATAL are valid emitted levels; NONE is threshold-only.

I2:
Logger configuration reads/writes contain no C data races.

I3:
Logger file paths are never silently truncated.

I4:
Logger file open/rotation failures are observable and do not recursively enter atpd_error.

I5:
A failed rotation does not silently disable all logging.

I6:
All configured targets are actually implemented; no fake SYSLOG target remains.

I7:
Android logcat is explicit rather than an unconditional hidden sink.

I8:
Logger initialization is one-shot; reload applies deltas instead of re-init.

I9:
Runtime logfile changes preserve the old sink if the new sink cannot be prepared.

I10:
Logger shutdown is idempotent and flushes best-effort.

I11:
Logger does not own timezone detection.

I12:
Logger has no dependency on atpd_global.
```

---

# 138. 最终验收标准

## Safety

```text
invalid level
→ no OOB
```

## TSan

```text
concurrent write/set-level/get-level
→ 0 race
```

## File security

```text
symlink/hardlink/non-regular
→ rejected
```

## Paths

```text
long path
→ explicit error
no truncation
```

## Rotation

```text
rename failure
→ original/fallback logging remains available
```

## Init

```text
file sink failure
→ truthful DEGRADED
```

## Shutdown

```text
last log line flushed
double close safe
```

## Dependencies

```text
logger.c no atpd_global.h
logger.c no hidden timezone initialization
```

---

# 139. 最终结论

`logger.c` 不需要大改。

它当前最值得保留的是：

```text
O_NOFOLLOW
O_CLOEXEC
regular-file validation
hard-link rejection
mutex-protected file lifecycle
```

真正需要修的是边界：

```text
invalid level OOB
min_level lock-free data race
fake SYSLOG/enable_timestamp config
hidden Android sink
silent path truncation
silent init/rotation failures
duplicate APIs
logger→timezone/global dependency
```

所以本轮建议仍然是：

> 加固，不拆分，不引入异步 logger。

把 logger 做成一个小型、同步、可预测、失败可观测，而且绝不与 `atpd_error` 形成递归/锁环的基础设施模块。
