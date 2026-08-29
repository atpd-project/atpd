# ATPD `utils.c / utils.h` 拆分与安全加固方案

## 1. 结论

当前：

```text
src/utils.c      ~1232 lines
include/utils.h  46 lines
```

这个模块已经不是“小工具集合”，而是同时承担：

```text
字符串
文件/目录
命令执行
进程发现/kill/wait
/proc metrics
binary version
安装路径
Android system property
Android tzdata parser
timezone initialization
```

因此这次与前面 reactor/UDS 不同：

> `utils.c` 建议拆分。

并且在拆分之前，有几个必须优先修复的 correctness / safety bug。

---

# 2. P0/P1：`exec_cmd_argv()` 的 timeout 实际可能完全失效

当前流程：

```text
pipe
fork
child dup stdout/stderr → pipe
exec

parent:
    blocking read(pipe)
    一直读到 EOF 或 output buffer满
    ↓
close pipe
    ↓
才开始 clock_gettime()
    ↓
waitpid(WNOHANG) timeout loop
```

问题：

```text
timeout计时是在 blocking read 之后才开始。
```

如果 child：

```text
不退出
stdout/stderr保持打开
没有更多输出
```

parent 会卡在：

```c
read(pipefd[0], ...)
```

无限等待。

所以：

```text
timeout_sec = 3
```

并不意味着函数最多3秒返回。

---

# 3. 这会阻塞 reactor/main thread

任何调用：

```text
get_binary_version()
→ exec_cmd_argv()
```

如果落在 runtime hot path，

都可能无限 block。

这和我们前面：

```text
Native API sync block
```

属于同类 daemon 风险。

---

# 4. 正确 command runner模型

应从：

```text
blocking read
then timeout wait
```

改为：

```text
pipe2(O_NONBLOCK | O_CLOEXEC)
fork
↓
poll/ppoll
    child output readable
    timeout deadline
    child exit
↓
drain pipe
↓
wait/reap
```

必须用：

```text
CLOCK_MONOTONIC absolute deadline
```

---

# 5. 更推荐复用统一 child-process primitive

目前 ATPD 已经有：

```text
service child
async_validate child
utils exec child
```

不要继续各自实现第三、第四套：

```text
fork/pipe/wait/timeout/kill
```

短期可以先修 `exec_cmd_argv()`。

中期建议抽取窄的内部 primitive：

```text
process_run_capture()
```

但不要做万能 shell framework。

---

# 6. `exec_cmd_argv()` 的 pipe 应使用 CLOEXEC

当前：

```c
pipe(pipefd)
```

没有：

```text
O_CLOEXEC
```

child 在 `execvp()` 后：

```text
stdout/stderr需要保留的 duplicated fd
```

没问题，

但原始 descriptor及其他继承行为最好严格控制。

推荐：

```c
pipe2(pipefd, O_CLOEXEC | O_NONBLOCK)
```

child `dup2` 后关闭原 fd。

---

# 7. Child `dup2()` 返回值当前没检查

当前：

```c
dup2(pipefd[1], STDOUT_FILENO);
dup2(pipefd[1], STDERR_FILENO);
execvp(...);
```

如果 dup2失败仍继续 exec。

推荐：

```text
exec-error pipe
```

报告：

```text
dup2 errno
exec errno
```

而不是全部 `_exit(127)`。

---

# 8. Exit 127语义混淆

当前：

```text
execvp failed
→ _exit(127)
```

但真实程序也可能：

```text
正常 exit(127)
```

caller无法区分。

与 async_validate/service方案一致：

> 用 O_CLOEXEC exec-error pipe。

---

# 9. `output == NULL` 时当前行为也不合理

即使 caller不需要 output，

child stdout/stderr仍被 redirect到 pipe。

parent随后不读取，直接 close read end。

如果 child有输出：

```text
write pipe
→ EPIPE/SIGPIPE
```

child可能因为“caller不想捕获输出”而异常退出。

---

# 10. 正确语义

如果：

```text
output == NULL
```

应该明确选择：

```text
inherit stdout/stderr
or
redirect /dev/null
```

推荐 command helper提供：

```text
capture_output=true/false
```

如果 false：

```text
/dev/null
```

更适合 daemon helper。

---

# 11. Output buffer满以后仍应 drain child output

当前：

```text
while total < output_size - 1
    read...
```

buffer一满就停止读取并关闭 pipe。

child如果继续输出：

```text
EPIPE/SIGPIPE
```

于是一个“只是输出太多”的命令可能被 ATPD主动杀坏。

这和 async_validate的 >4KB问题同类。

---

# 12. 正确方式

```text
capture buffer有上限
但 pipe消费必须继续
```

即：

```text
buffer未满 → copy
buffer满 → discard
一直 drain到 EOF/exit/deadline
```

并记录：

```text
output_truncated=true
```

---

# 13. P0/P1：`str_replace()` 的 size_t 下溢

当前长度：

```c
size_t buf_len =
    str_len + count * (new_len - old_len) + 1;
```

如果：

```text
new_len < old_len
```

因为：

```text
size_t 无符号
```

`new_len - old_len` 会变成巨大正数。

结果：

```text
错误巨型 malloc
ENOMEM
或后续 overflow
```

---

# 14. 正确长度计算

必须分支：

```c
if (new_len >= old_len) {
    delta = new_len - old_len;
    check multiplication/add overflow;
    size = str_len + count * delta + 1;
} else {
    delta = old_len - new_len;
    size = str_len - count * delta + 1;
}
```

---

# 15. 同时必须检查 size_t overflow

对于增长替换：

```text
count * delta
str_len + growth + 1
```

都应：

```text
SIZE_MAX guard
```

---

# 16. Test：更短替换

例如：

```text
"aaaa"
replace "aa" → "b"
```

不能：

```text
申请超大内存
```

输出应正确。

---

# 17. Test：更长替换 + overflow injection

构造大 length/count边界。

预期：

```text
return NULL / error
```

不 wrap。

---

# 18. `exec_cmd()` 使用 shell 拼接，有 command injection surface

当前：

```c
snprintf(timeout_cmd, ..., "timeout %d %s", timeout_sec, cmd);
popen(timeout_cmd, "r");
```

这会经过：

```text
/bin/sh
```

如果 `cmd` 含：

```text
;
&
$()
backticks
redirection
```

就会执行 shell语义。

---

# 19. 即使当前 caller都是内部可信字符串，也不应作为通用 helper存在

名字：

```text
exec_cmd
```

太容易未来接入：

```text
config path
interface name
user input
```

而产生 injection。

推荐：

> production code优先全部迁移到 argv runner。

---

# 20. `timeout` 外部命令依赖也不稳定

`exec_cmd()` 假设系统存在：

```text
timeout
```

Android环境并不应该把核心 correctness建立在：

```text
coreutils/toybox timeout
```

是否存在上。

timeout应由 ATPD自身实现。

---

# 21. 推荐最终删除 shell-based `exec_cmd()`

全仓审计：

```text
exec_cmd(
exec_cmd_simple(
```

如果都能改 argv：

删除。

如果确有 shell脚本需求：

重命名：

```text
exec_shell_command_unsafe()
```

并限制 caller。

但最好不要保留 public generic API。

---

# 22. `get_pid_by_name()` 第三层匹配过宽

它依次：

```text
/proc/PID/comm exact
/proc/PID/exe basename exact
/proc/PID/cmdline strstr(name)
```

第三层：

```c
strstr(line, name)
```

可能匹配：

```text
参数里出现 sing-box
wrapper script path
配置路径
其他程序命令行
```

---

# 23. 这与 service PID问题直接相关

我们已经确定：

```text
service child必须按 owned PID + starttime/generation
```

而不是：

```text
get_pid_by_name("sing-box")
```

所以 service、reload、kill逻辑应停止依赖这个 generic helper。

---

# 24. `get_pid_by_name()` 如果保留

仅用于：

```text
diagnostics
best-effort status
```

且：

```text
comm/exe exact match
```

就足够。

建议删除：

```text
cmdline substring fallback
```

或者只匹配：

```text
argv[0] basename exact
```

---

# 25. `kill_all_by_name()` 风险更高

它扫描整个 `/proc`：

```text
comm exact
→ kill
```

对于 root daemon，

可能杀掉：

```text
不是 ATPD启动的同名进程
```

---

# 26. ATPD service lifecycle不应该使用 `kill_all_by_name`

最终：

```text
只 kill owned child identity
```

如果该 helper只用于 emergency CLI：

也必须非常谨慎。

推荐全仓 callsite audit后删除。

---

# 27. `kill_process()` 只是 `kill()` 薄包装

```c
return kill(pid, signal);
```

没有增加语义。

如果无 mock/test abstraction价值：

删除，直接用 owner module中的明确 kill逻辑。

---

# 28. `process_exists()` 错把 EPERM 当“不存在”

当前：

```c
return kill(pid, 0) == 0;
```

POSIX/Linux：

```text
kill(pid,0) == -1, errno=EPERM
```

表示：

```text
process存在，但无权限发送 signal
```

正确：

```text
0 → exists
EPERM → exists
ESRCH → not exists
other → error/unknown
```

虽然 ATPD通常 root，

API本身仍应正确。

---

# 29. 更大的问题：`kill(pid,0)` 不能验证 identity

PID可能：

```text
退出
→ kernel reuse PID
```

所以它不能用于：

```text
child ownership verification
```

service仍必须：

```text
PID + /proc starttime
```

---

# 30. `wait_for_pid_exit()` 也只看 PID存在

当前：

```text
while kill(pid,0)==0
sleep 200ms
```

如果原进程退出、PID很快被复用：

```text
可能一直认为“原进程还活着”
```

而且它并不：

```text
waitpid/reap child
```

所以不能用于 owned child lifecycle。

---

# 31. 建议删除 generic `wait_for_pid_exit`

service需要：

```text
waitpid owned child
```

外部非child diagnostics需要：

```text
PID identity-aware observer
```

两者不是一个 helper。

---

# 32. `/proc/PID/stat` parsing存在经典 comm-with-space问题

`get_process_cpu_percent()`：

```c
sscanf(line, "%*d %*s %*c ...")
```

`/proc/<pid>/stat` 的 field 2：

```text
(comm)
```

可以包含：

```text
空格
括号
```

`%s` 并不能可靠跳过。

---

# 33. `get_process_uptime_sec()` 更明显

它用：

```text
strtok_r(line, " ")
```

数第22个 field。

如果 process comm包含空格：

```text
field numbering全部错位
```

---

# 34. 正确 proc stat parser

必须：

```text
先找到最后一个 `)` 对应 comm结束
```

然后从：

```text
state field
```

继续 parse。

建议做一个内部 helper：

```c
proc_stat_read(pid, &parsed)
```

统一：

```text
utime
stime
starttime
```

---

# 35. 不要继续在多个 helper各 parse一次 `/proc/stat`

可以建立：

```c
typedef struct {
    uint64_t utime_ticks;
    uint64_t stime_ticks;
    uint64_t starttime_ticks;
} proc_stat_info_t;
```

internal only。

---

# 36. CPU cache有 PID reuse问题

当前 cache key：

```text
PID only
```

如果：

```text
PID 123 old process退出
PID 123 new process出现
```

cache会把：

```text
旧 last_total
```

应用给新进程。

结果 CPU sample错误。

---

# 37. Cache key必须包含 process starttime

例如：

```text
pid + starttime_ticks
```

发现 generation改变：

```text
reset sample
```

---

# 38. CPU measurement使用 `time(NULL)`

这会受：

```text
wall-clock adjustment
```

影响。

应该：

```text
CLOCK_MONOTONIC
```

---

# 39. CPU被硬 cap到100%

当前：

```text
if cpu_percent > 100
    cpu_percent = 100
```

多线程 process在多核系统上：

```text
CPU可能 >100%
```

例如 sing-box。

如果定义是：

```text
one-core normalized percentage
```

不应该 cap。

如果定义是：

```text
whole-machine percentage
```

则需要除 CPU count。

必须明确指标语义。

---

# 40. 推荐 status资源指标

为了前面的 resource observability：

可以定义：

```text
cpu_core_percent
```

允许：

```text
0..N*100
```

最直观。

---

# 41. CPU cache只有64 entries

当 status/diagnostic观察超过64个 PID后：

```text
后续新 PID永远不缓存
```

CPU永远可能返回0初始值。

而 stale dead PID entry也不会清理。

---

# 42. 对 ATPD其实不需要 generic 64-PID cache

通常真正关心：

```text
ATPD
sing-box
```

最多几个固定进程。

更好的方法：

```text
caller持 sample state
```

而不是 utils global cache。

---

# 43. 推荐把 CPU sampler放到 status/resource owner

例如：

```c
process_cpu_sample_t
```

每个 observed process一份。

这样没有：

```text
global mutex
64-entry cache
PID cleanup
```

问题。

---

# 44. `get_process_memory_kb()` 等失败返回0语义不够清楚

当前：

```text
open /proc fail
→ 0
```

但：

```text
0 KB
```

与：

```text
unavailable
```

不同。

推荐：

```c
int get_process_memory_kb(pid_t pid, long *out);
```

返回：

```text
0 success
-1 unavailable
```

---

# 45. Threads / FD / socket count同理

现在失败：

```text
0
```

status可能显示：

```text
FDs: 0
```

误导。

应该能表达：

```text
unknown
```

---

# 46. Resource metrics建议迁到独立模块

由于 status/resource testing已经是明确需求，

建议拆：

```text
procfs.c / procfs.h
```

负责：

```text
PID identity
VmRSS
VmHWM
VmSize
Threads
FD count
socket count
CPU stat
starttime
```

---

# 47. `utils.c` 不应该继续承担 procfs observability

这是一个独立领域，

而且需要：

```text
严格 parser
unknown/error semantics
unit tests
fixture tests
```

非常适合单独模块。

---

# 48. `get_process_user_group()` 只是输出数字字符串

函数名听起来：

```text
user/group names
```

实际输出：

```text
uid/gid numeric strings
```

命名误导。

如果保留：

```text
get_process_uid_gid
```

返回 numeric types更合理：

```c
uid_t
gid_t
```

renderer自己转字符串。

---

# 49. UID/GID parse应检查 `sscanf` 返回

当前：

```text
默认 uid=0/gid=0
parse失败
→ 仍返回 root
```

这是危险的假成功。

必须：

```text
seen_uid
seen_gid
```

否则 return failure。

---

# 50. `get_binary_version()` 不适合放 utils

它知道：

```text
binary version command格式
"version "
```

这是 service/sing-box/domain-specific behavior。

如果仅用于 sing-box：

迁到：

```text
service_process
or singbox API/version owner
```

---

# 51. 更重要：我们已经有 Native API GetVersion

前面 `singbox_api` 方案决定：

```text
version按 service generation从 Native API缓存
```

因此外部执行：

```text
sing-box version
```

可能可以彻底删除。

---

# 52. `get_binary_version` static cache也没有 mutex

如果多线程调用：

```text
s_cached_path
s_cached_ver
```

存在 data race。

如果迁移/删除：

无需修。

---

# 53. Cache只按 path，不按 binary identity

同一路径 binary升级：

```text
旧版本 cache永久保留
```

也不正确。

再次支持删除。

---

# 54. `mkdir_recursive()` 不检查 snprintf truncation

当前：

```c
snprintf(tmp, PATH_MAX, "%s", path);
```

如果 path过长：

```text
被截断
```

然后函数可能：

```text
创建截断后的另一个目录
```

这是文件系统 correctness问题。

必须：

```text
ret < 0 || ret >= sizeof(tmp)
→ ENAMETOOLONG
```

---

# 55. 遇到 EEXIST还必须确认是 directory

当前：

```text
mkdir() == -1 && errno == EEXIST
→ 当成功
```

但 path component可能是：

```text
regular file
symlink
```

后续会失败或出现安全歧义。

建议：

```text
fstatat/lstat
确认目录
```

---

# 56. Root daemon路径操作需要更严格

对于 runtime directories：

```text
run_dir
log_dir
config dirs
```

尤其不要盲目穿过攻击者可控制 symlink。

具体需要结合目录权限模型。

第一阶段至少：

```text
truncation check
EEXIST-is-directory check
```

---

# 57. `file_exists()` 使用 `access(F_OK)`

名字只表示存在，

可接受。

但：

```text
permission/TOCTOU
```

不能用于后续安全决策。

如果 caller随后 open：

直接：

```text
open
```

并处理 errno。

---

# 58. `read_file()` / `write_file()` 是过于宽泛的 API

`write_file()`：

```text
fopen("w")
fprintf
fclose
```

没有：

```text
mode control
O_CLOEXEC
fsync
atomic rename
symlink policy
```

所以不能用于：

```text
PID/config/security-sensitive runtime files
```

---

# 59. 建议根据用途迁回 owner

例如：

```text
config_save_runtime
→ config owns atomic file

pidfile
→ daemon/service owns

generic small proc/sys read
→ platform helper
```

如果剩余 caller很少：

可以保留 internal `read_text_file`。

---

# 60. `write_file()` 应避免成为安全关键路径

全仓审计后，

如果仅测试/非critical：

保留也行。

否则使用：

```text
open flags + explicit mode + fsync/rename
```

的 owner-specific实现。

---

# 61. `get_app_dir()` 失败时返回 `"."` 但仍返回 success

当前最终 fallback：

```c
snprintf(buf, size, ".");
return 0;
```

这意味着：

```text
无法解析安装目录
```

会静默变成：

```text
当前工作目录
```

---

# 62. 对 root daemon这是危险 fallback

如果 cwd变化或启动位置不同：

ATPD可能：

```text
在错误位置找 binary/config
创建 run/localtime
```

推荐：

```text
没有可靠 app dir
→ return failure
```

除非 caller明确允许 cwd fallback。

---

# 63. 这与 deterministic deployment相冲突

ATPD模块路径应该来自：

```text
/proc/self/exe
configured prefix
explicit config
```

而不是：

```text
whatever cwd happens to be
```

---

# 64. `find_command_path()` 会信任环境 PATH

对于 root daemon：

```text
PATH
```

如果环境受外部影响，

可能找到：

```text
非预期同名 executable
```

---

# 65. sing-box binary应优先使用配置中的绝对路径

service ownership方案应该已经做到：

```text
explicit executable identity
```

不应该靠 generic PATH discovery作为长期主路径。

---

# 66. 如果保留 PATH search

仅用于：

```text
CLI/dev diagnostics
```

并且：

```text
not privileged service spawn
```

---

# 67. `utils.c` 最后约450行其实是 timezone subsystem

从：

```text
TZ structs
Android tzdata paths
POSIX fallback table
Android property
tzdata parser
localtime extraction
TZ env setup
offset calculation
```

这已经完全不是 generic utils。

---

# 68. Timezone应该拆成独立模块

推荐：

```text
timezone.c
timezone.h
```

迁移：

```text
atp_timezone_init
atp_timezone_get_name
atp_timezone_get_offset_sec
Android tzdata parsing
```

---

# 69. 为什么这次确实值得拆

timezone code本身：

```text
~450 lines
```

有独立：

```text
state
mutex
parser
filesystem behavior
Android-specific fallback
tests
```

已经是完整 subsystem。

---

# 70. 当前 timezone fallback有产品语义风险

如果检测完全失败：

```c
detected_tz = "Asia/Shanghai";
```

最后 ultimate fallback：

```text
CST-8
```

这意味着任何非中国设备只要检测失败：

```text
日志时间可能静默变成 UTC+8
```

---

# 71. 这不应该是 generic daemon默认

更安全 fallback：

```text
UTC
```

因为：

```text
UTC不会错误宣称用户所在地
```

如果产品明确只面向中国设备另说。

但 ATPD目标包括：

```text
Android/Linux通用环境
```

应默认 UTC。

---

# 72. Locale不能可靠推断 timezone

当前：

```text
zh → Asia/Shanghai
ja → Asia/Tokyo
ko → Asia/Seoul
```

语言 ≠ 时区。

例如：

```text
zh locale用户可能在美国、新加坡、台湾等
```

所以：

```text
ro.product.locale
```

不应作为 authoritative timezone fallback。

---

# 73. 推荐 timezone detection priority

```text
1 explicit TZ
2 /etc/localtime
3 persist.sys.timezone
4 Android authoritative timezone source if available
5 system offset fallback
6 UTC
```

不要：

```text
locale → timezone
```

---

# 74. Offset-only fallback会丢 DST规则

`date +%z` 只能得到：

```text
当前 offset
```

转换成：

```text
UTC±N
```

不会包含：

```text
future DST transitions
```

所以它只能作为：

```text
last-resort display fallback
```

不能假装完整 timezone。

---

# 75. Timezone init会修改整个 process环境

它调用：

```c
setenv("TZ", ..., 1);
tzset();
```

这是 process-global side effect。

必须：

```text
仅 startup执行一次
```

而不是 getter lazy init时随时触发。

---

# 76. Getter不应隐式修改全局时区

当前：

```text
atp_timezone_get_name()
如果未init
→ 调 atp_timezone_init()
```

这意味着一个看似：

```text
read getter
```

可能：

```text
创建目录
读取 Android tzdata
写 localtime temp file
setenv
tzset
popen getprop/date
```

隐藏副作用非常大。

---

# 77. 推荐明确 startup init

```text
daemon startup
→ timezone_init once
```

getter：

```text
只读 snapshot
```

如果没init：

```text
return UTC/uninitialized
```

不要 lazy heavyweight init。

---

# 78. `atp_timezone_get_offset_sec()` 有并发读取问题

它：

```c
if (!g_tz_initialized)
    atp_timezone_init();
```

没有先持：

```text
g_tz_mutex
```

如果确实多线程：

这是 unsynchronized access。

拆分后一起修。

---

# 79. Timezone extraction temp file权限

当前：

```c
fopen(tmp_file, "wb")
```

mode受：

```text
umask
```

控制。

localtime内容不是秘密，

但 root daemon最好显式：

```text
0644 or 0600
```

并使用：

```text
O_CLOEXEC
```

---

# 80. Temp name只有 PID

```text
out_file.tmp.PID
```

如果同进程重复 init或旧残留：

可能冲突。

startup once可降低风险。

可以：

```text
mkstemp
```

或安全 `open(O_CREAT|O_EXCL)`。

---

# 81. Rename前没有 fsync

timezone cache不是关键持久数据，

所以可以不强制 fsync。

但要明确：

```text
best-effort cache
```

不是 durable config。

---

# 82. tzdata parser边界检查还可以加强

当前已经检查：

```text
magic
offset basic relation
entry count <=4096
length <=1 MiB
TZif magic
```

这是好的。

还应确认：

```text
data_offset + start_offset
+ length
```

没有：

```text
integer overflow
超出文件大小
```

---

# 83. 应先 `fstat()` tzdata文件大小

验证：

```text
index_offset <= file_size
data_offset <= file_size
data_offset + start_offset <= file_size
length <= remaining
```

不要只依赖：

```text
fseek/fread失败
```

---

# 84. `uint32_t` 加法转换到 long也要检查

尤其：

```text
data_offset + start_offset
```

先在 uint32_t发生 wrap风险。

应该提升：

```text
uint64_t
```

做 validated offset。

---

# 85. `derive_posix_tz_from_date()` 没检查数字字符

它只验证：

```text
length=5
第一个 +/- 
```

然后：

```c
(out[1]-'0')...
```

如果输出异常字符：

得到垃圾数字。

要：

```text
isdigit(out[1..4])
hours <= 23
mins <= 59
```

实际 timezone offset范围还可更严格。

---

# 86. Header stale API：`check_ip6tables_available()`

当前 `utils.h` 声明：

```c
int check_ip6tables_available(void);
```

但当前 `utils.c` 中没有对应定义。

必须全仓审计：

```text
是否在其他源文件定义
是否完全未使用
```

如果无 definition/caller：

删除 stale declaration。

---

# 87. Header/implementation另一个不一致

`utils.c` 定义：

```c
int wait_for_pid_exit(pid_t pid, int timeout_sec)
```

但当前 `utils.h` 没有声明。

如果它需要跨文件使用：

C11下应该正式声明。

如果无 caller：

删除 dead function。

---

# 88. 这说明 utils API已经发生腐化

表现为：

```text
header声明但实现消失
实现存在但header不暴露
```

正是“catch-all utils”长期增长的典型结果。

---

# 89. 拆分建议

推荐：

```text
utils.c / utils.h
```

最终只保留真正 generic 小函数，例如：

```text
trim
starts_with
ends_with
safe string helpers
maybe path helpers
```

控制在：

```text
100–250 LOC
```

---

# 90. 新建 `procfs.c / procfs.h`

放：

```text
process identity
starttime
RSS/VmHWM/VmSize
Threads
FD count
socket count
CPU sampling primitives
uptime
uid/gid
```

---

# 91. 新建 `timezone.c / timezone.h`

放：

```text
Android property timezone
tzdata parsing
TZ setup
timezone snapshot
offset
```

---

# 92. Process execution不要叫 utils

二选一：

### 如果只剩少数 caller

把执行逻辑迁到真正 owner：

```text
service_process
async_validate
```

### 如果确实有多个 caller

新建：

```text
process_exec.c / process_exec.h
```

只负责：

```text
argv exec
deadline
capture
exec error
reap
```

---

# 93. File helpers可以暂留 utils

例如：

```text
trim
starts_with
ends_with
```

以及很少量：

```text
read small text file
safe mkdir
```

但安全关键操作回 owner module。

---

# 94. `get_app_dir` 可以迁到 platform/path模块

如果只有少数 caller：

可保留在 utils，

但必须删除：

```text
"." silent success fallback
```

---

# 95. `find_command_path`

如果 service不再使用 PATH search：

可能只剩测试/CLI用途。

做 callsite审计后决定：

```text
保留
缩窄
删除
```

---

# 96. 推荐 Commit 1

```text
utils: fix command timeout and output draining
```

内容：

- nonblocking/CLOEXEC pipe
- monotonic deadline覆盖 read+wait全部阶段
- EINTR
- drain after capture truncation
- exec-error pipe
- deterministic cleanup/reap

---

# 97. Commit 2

```text
utils: fix string replacement size arithmetic
```

内容：

- shorter replacement
- size overflow guards
- tests/fuzz

这是小而独立的安全 commit。

---

# 98. Commit 3

```text
procfs: replace broad process-name helpers with identity-safe inspection
```

内容：

- strict `/proc/stat` parser
- PID + starttime
- metrics error semantics
- remove broad cmdline substring
- remove kill/wait helpers from service paths

---

# 99. Commit 4

```text
procfs: move process metrics out of utils
```

新：

```text
procfs.c/h
```

---

# 100. Commit 5

```text
timezone: move Android timezone subsystem out of utils
```

机械迁移优先：

```text
behavior unchanged
tests pass
```

再做语义修复。

---

# 101. Commit 6

```text
timezone: make initialization explicit and default safely
```

内容：

- startup-only init
- getters read-only
- remove locale→timezone guess
- ultimate UTC fallback
- validated date offset
- tzdata bounds

---

# 102. Commit 7

```text
utils: harden path and file helpers
```

- path truncation
- EEXIST directory check
- no cwd silent fallback
- audit write_file

---

# 103. Commit 8

```text
utils: remove stale and dead APIs
```

审计并删除：

```text
check_ip6tables_available declaration
wait_for_pid_exit if unused
kill_process
kill_all_by_name
exec_shell helpers
get_binary_version
```

仅删除确认无必要的。

---

# 104. Command runner测试：hard hang

child：

```text
sleep forever
stdout保持打开
```

调用：

```text
timeout=1s
```

必须：

```text
~1s内 kill/reap并返回
```

这是当前 bug的直接 regression test。

---

# 105. Command runner测试：silent hang

child完全不输出：

```text
sleep 30
```

timeout=1。

必须仍然1s退出。

---

# 106. Command runner测试：continuous output

child持续写：

```text
>1MB
```

capture buffer：

```text
4KB
```

必须：

```text
truncate capture
继续drain
不死锁
正常wait/timeout
```

---

# 107. Command runner测试：output NULL

child大量 stdout/stderr。

不能因为 parent不 capture：

```text
SIGPIPE异常退出
```

---

# 108. Command runner测试：exec fail

不存在 binary。

应返回：

```text
EXEC_FAILED + errno ENOENT
```

而不是只能看到：

```text
127
```

---

# 109. Command runner测试：timeout kill/reap

每次 timeout后：

```text
waitpid确认 child无 zombie
```

跑：

```text
10k cycles
```

---

# 110. Proc parser测试必须使用带空格 comm

构造 fixture：

```text
123 (my worker name) S ...
```

确保：

```text
utime
stime
starttime
```

解析正确。

---

# 111. PID reuse测试

模拟：

```text
same pid
different starttime
```

CPU sampler：

```text
reset baseline
```

不能沿用旧 total。

---

# 112. `process_exists` EPERM测试

模拟：

```text
kill(pid,0) → EPERM
```

应返回：

```text
exists/unknown-permission
```

不是 false。

---

# 113. Metrics unavailable测试

不存在 PID：

```text
memory/thread/fd
```

必须返回：

```text
error/unknown
```

而不是：

```text
0
```

---

# 114. `mkdir_recursive` 长路径测试

>PATH_MAX：

```text
ENAMETOOLONG
```

不能创建截断路径。

---

# 115. `mkdir_recursive` file-in-path测试

例如：

```text
/a/b
```

其中 `/a` 是 regular file。

必须 fail。

---

# 116. `get_app_dir` failure测试

如果：

```text
/proc/self/exe不可解析
ATP_DEFAULT_DIR不存在
```

应：

```text
return error
```

不隐式 `"."`。

---

# 117. Timezone test：unknown device

无：

```text
TZ
/etc/localtime
persist.sys.timezone
```

最终：

```text
UTC
```

不应：

```text
Asia/Shanghai
```

---

# 118. Timezone test：locale不决定时区

例如：

```text
locale=zh-US
timezone property missing
```

不能直接：

```text
Asia/Shanghai
```

---

# 119. Timezone test：DST

对：

```text
America/Los_Angeles
Europe/London
```

分别测试冬/夏时间 offset。

确保使用真实 tzdata时：

```text
DST正确
```

---

# 120. Timezone test：malformed tzdata

fuzz：

```text
header offsets
entry length
start offset
truncated file
invalid TZif
```

不能：

```text
OOB
overflow
huge allocation
```

---

# 121. Timezone test：getter无副作用

初始化完成后：

```text
get_name/get_offset
```

不：

```text
fork/popen
mkdir
write file
setenv
```

---

# 122. Fuzz targets

非常适合：

```text
str_replace arithmetic
proc stat parser
tzdata parser
TZif footer parser
date offset parser
```

---

# 123. Sanitizers

至少：

```text
ASan
UBSan
```

对 utils拆分后的 parser测试。

如果 CPU/timezone仍支持多线程：

```text
TSan
```

检查 cache/init state。

---

# 124. 最终推荐文件结构

```text
src/
├─ utils.c
├─ procfs.c
├─ timezone.c
└─ process_exec.c   # 仅确有多个consumer才建

include/
├─ utils.h
├─ procfs.h
├─ timezone.h
└─ process_exec.h
```

---

# 125. 如果 process_exec只有 `get_binary_version` 一个 caller

则不要新建：

```text
process_exec.c
```

直接删除 generic runner或迁到 owner。

原则仍然是：

> 不为了拆文件而造新抽象。

---

# 126. 拆分目标

最终 `utils.c` 应只剩：

```text
真正无领域归属
纯函数
低副作用
小而稳定
```

而不是：

```text
任何不知道放哪里的东西
```

---

# 127. 与 service方案联动

删除：

```text
get_pid_by_name
kill_all_by_name
wait_for_pid_exit
```

在 service lifecycle中的使用。

service只处理：

```text
owned child PID + generation/starttime
```

---

# 128. 与 status方案联动

resource collector改用：

```text
procfs snapshot
```

可以增加：

```text
VmRSS
VmHWM
VmSize
FD
Threads
CPU
starttime
```

并明确 unknown。

---

# 129. 与 eBPF删除方案联动

之前：

```text
ebpf telemetry
→ get_process_fd_count(sing-box)
→ 假 active_conns
```

删除 eBPF fake telemetry以后，

这个误用也自然消失。

---

# 130. 与 singbox_api方案联动

`get_binary_version()` 应优先被：

```text
Native API version snapshot
```

替代。

避免 runtime fork子进程获取版本。

---

# 131. 与 init方案联动

timezone：

```text
startup明确 init一次
```

command runner：

```text
不能在 reactor-critical startup路径无限阻塞
```

---

# 132. 最终 Invariants

Codex最终必须保证：

```text
I1:
Every command timeout covers the entire child lifecycle, including output reads.

I2:
Captured output may truncate, but child pipe is still drained.

I3:
Every spawned utility child is reaped exactly once.

I4:
No shell command execution is used for untrusted/dynamic arguments.

I5:
str_replace never performs unsigned length underflow/overflow.

I6:
Process identity is never inferred from broad cmdline substring for lifecycle ownership.

I7:
PID-only existence checks are not used as child identity checks.

I8:
`/proc/<pid>/stat` parsing handles spaces/parentheses in comm correctly.

I9:
Process metrics distinguish unavailable from a legitimate zero.

I10:
Timezone getters are read-only after explicit initialization.

I11:
Unknown timezone falls back safely to UTC, not an arbitrary geography.

I12:
utils.c contains only small generic helpers after refactor.
```

---

# 133. 最终验收标准

## Command execution

```text
silent hung child + timeout=1
→ bounded return
→ no zombie
```

## Strings

```text
shorter/longer replacement
→ correct buffer arithmetic
```

## Process ownership

```text
service no longer uses name-based kill/discovery
```

## Procfs

```text
comm with spaces parses correctly
PID reuse detected by starttime
```

## Metrics

```text
unknown != zero
```

## Timezone

```text
explicit startup init
UTC safe fallback
DST tests pass
malformed tzdata safe
```

## Source organization

```text
utils.c ~100–250 lines target
timezone/procfs have their own focused tests
```

---

# 134. 最终结论

`utils.c` 是目前已经审过的模块里，**少数明确值得拆文件的模块**。

不是因为1232行本身，而是因为它已经混合了至少四个完全不同的生命周期/安全域：

```text
generic utilities
process execution
procfs/process inspection
Android timezone subsystem
```

本轮最优先修复的两个实际缺陷是：

```text
1. exec_cmd_argv timeout不覆盖 blocking read，可能无限挂起
2. str_replace shorter replacement触发 size_t长度下溢
```

然后按领域收敛：

```text
procfs → procfs.c
timezone → timezone.c
process execution → owner or process_exec.c
generic pure helpers → utils.c
```

最终目标不是制造更多 helper，而是让 `utils` 不再成为 ATPD 的“无归属代码收容所”。
