# ATPD `cli.c / cli.h` 参数解析与命令边界加固方案

## 1. 结论

当前：

```text
src/cli.c      203 lines
include/cli.h   41 lines
```

模块规模合适，**不建议拆分**。

当前优点：

```text
getopt_long 使用清晰
command enum 简单
默认值集中初始化
help/version/usage 集中
```

主要问题不是结构，而是 CLI parser 目前偏“宽松”：

```text
路径静默截断
数字使用 atoi
互斥选项可同时留下矛盾状态
命令后的多余参数未严格拒绝
一些选项已经失去架构意义
eBPF CLI/文案已经过时
CLI options 与 logger 类型耦合
```

这一轮应把 CLI 做成：

> 严格、无歧义、无静默 coercion、只负责用户输入解析，不承担 daemon runtime policy。

---

# 2. P1：`--config` / `--pid` 路径静默截断

当前：

```c
strncpy(opts->config_file, optarg,
        sizeof(opts->config_file) - 1);
opts->config_file[sizeof(opts->config_file) - 1] = '\0';
```

`--pid` 同样。

如果用户传入：

```text
PATH_MAX 以上路径
```

parser不会报错，而是：

```text
截成另一个路径
```

这对 root daemon 尤其危险。

---

# 3. 为什么静默路径截断不能接受

例如：

```text
--config /very/long/.../production.json
```

最终可能变成：

```text
/very/long/.../product
```

随后：

```text
读取错误文件
创建错误 PID file
连接错误 runtime endpoint
```

CLI 必须 fail closed。

---

# 4. 推荐 bounded copy helper

例如：

```c
static int copy_cli_path(char *dst,
                         size_t dst_size,
                         const char *src,
                         const char *option_name)
{
    size_t len;

    if (!dst || dst_size == 0 || !src)
        return -1;

    len = strlen(src);
    if (len >= dst_size) {
        fprintf(stderr, "%s: path too long\n", option_name);
        return -1;
    }

    memcpy(dst, src, len + 1);
    return 0;
}
```

不要：

```text
截断后继续。
```

---

# 5. P1：`--ipv6` 使用 `atoi()`

当前：

```c
opts->ipv6 = atoi(optarg);
```

以及 eBPF subcommand parser：

```c
opts->ipv6 = atoi(argv[i + 1]);
```

因此：

```text
--ipv6 abc
→ 0

--ipv6 1junk
→ 1

--ipv6 -123
→ -123
```

与 usage 宣称的：

```text
1|0
```

不一致。

---

# 6. 如果选项暂时保留

必须严格 parse：

```c
if (strcmp(optarg, "0") == 0)
    opts->ipv6 = 0;
else if (strcmp(optarg, "1") == 0)
    opts->ipv6 = 1;
else
    return CLI_PARSE_USAGE_ERROR;
```

不要为 boolean 用：

```text
atoi
strtol 宽松 trailing chars
```

---

# 7. 但更推荐：随 ATPD eBPF 模块一起删除 `--ipv6`

当前 `--ipv6` 的 help 是：

```text
Enable/disable IPv6 for eBPF probe
```

而新的 ownership 已经确定：

```text
sing-box owns ebpf-in
ATPD 不做 kernel eBPF probe
```

因此：

```text
CMD_EBPF_PROBE
CMD_EBPF_STATUS
--ipv6
parse_ebpf_command()
```

都应随着 eBPF removal plan 删除。

---

# 8. CLI 不应该继续暴露 ATPD-owned BPF diagnostics

删除：

```text
atpd ebpf probe
atpd ebpf status
```

如果未来需要 datapath 诊断：

应该是：

```text
atpd status
```

中的：

```text
sing-box Native API datapath state
```

而不是 ATPD 自己调用 BPF probe。

---

# 9. 所有 “Pure eBPF” 文案一起删除

当前：

```text
Command line interface implementation - Pure eBPF Edition
"Pure eBPF Edition"
"Start daemon (Pure eBPF mode)"
"atpd %s (Pure eBPF)"
```

都已经不准确。

当前实际架构：

```text
ATPD = root control plane
sing-box = worker + ebpf-in dataplane
```

---

# 10. 建议简单文案

例如：

```text
ATPD <version>
Advanced Transparent Proxy Daemon
```

不需要把实现细节写进产品 edition 名称。

---

# 11. P1：命令后的额外位置参数未严格拒绝

当前 parser 找到：

```c
const char *cmd = argv[optind];
```

设置 command 后就结束。

并没有为普通命令检查：

```text
optind + 1 == argc
```

因此：

```text
atpd start garbage
atpd stop foo bar
atpd status typo
```

都可能被接受。

---

# 12. 这会隐藏脚本错误

例如：

```text
atpd reload production
```

用户以为：

```text
reload production profile
```

实际 ATPD 只是：

```text
reload
```

并静默忽略 production。

CLI 应对未知输入严格失败。

---

# 13. 推荐 command arity table

例如：

```text
start      0 positional args
stop       0
restart    0
status     0
reload     0
check      0
version    0
help       0
```

任何多余 positional：

```text
usage error
```

---

# 14. 如果未来某命令有 subcommand

由该 command 的 parser 明确消费。

不要让 generic parser：

```text
静默忽略剩余 argv
```

---

# 15. `-V` 和 `-q` 可以留下矛盾状态

当前：

```c
case 'V':
    opts->verbose = 1;
    opts->log_level = DEBUG;

case 'q':
    opts->quiet = 1;
    opts->log_level = ERROR;
```

例如：

```text
atpd -V -q start
```

最终：

```text
verbose = 1
quiet = 1
log_level = ERROR
```

反过来：

```text
-q -V
```

得到：

```text
verbose = 1
quiet = 1
log_level = DEBUG
```

---

# 16. `verbose/quiet` 两个 bool 是重复状态

真正需要的其实只有：

```text
effective verbosity / log threshold
```

建议删除：

```text
opts->verbose
opts->quiet
```

只保留：

```text
opts->log_level
```

或者更好：

```text
CLI verbosity enum
```

---

# 17. CLI 不应依赖 logger 的内部 enum

当前 `cli.h`：

```c
#include "logger.h"
...
log_level_t log_level;
```

这让：

```text
CLI public header
→ logger public header
```

产生不必要耦合。

---

# 18. 更推荐 CLI 自己表达意图

例如：

```c
typedef enum {
    CLI_VERBOSITY_DEFAULT = 0,
    CLI_VERBOSITY_VERBOSE,
    CLI_VERBOSITY_QUIET
} cli_verbosity_t;
```

startup command 再映射：

```text
VERBOSE → logger DEBUG
DEFAULT → logger INFO
QUIET   → logger ERROR
```

---

# 19. 如果不想新增 enum

也可以只保留：

```text
int verbosity_delta
```

但 enum 更清晰。

---

# 20. `-f` 和 `-d` 同样是 last-option-wins

当前：

```text
-f:
    foreground=1
    daemon=0

-d:
    daemon=1
    foreground=0
```

所以：

```text
-f -d
```

和：

```text
-d -f
```

结果不同。

---

# 21. 这里 last-option-wins 可以接受吗？

技术上很多 CLI 允许：

```text
最后一个覆盖前一个
```

但如果保留，应：

```text
只存一个 mode
```

而不是两个互斥 bool。

---

# 22. 推荐：

```c
typedef enum {
    CLI_RUN_MODE_DEFAULT = 0,
    CLI_RUN_MODE_FOREGROUND,
    CLI_RUN_MODE_DAEMON
} cli_run_mode_t;
```

这样不可能出现：

```text
foreground=1 && daemon=1
```

---

# 23. 更重要：默认 daemon 是否仍应该由 CLI 决定

当前：

```c
opts->daemon = 1;
```

help 也说：

```text
Run as daemon (default for start)
```

但前面 `main.c` review 已经指出：

```text
Android/Magisk/KernelSU/APatch service manager
```

往往更适合让 ATPD 前台运行，由外部 supervisor 管理。

---

# 24. 这是产品部署决策，不应现在擅自改默认

Codex 第一阶段：

```text
保持当前默认行为
```

先修 daemon startup handshake。

之后再根据实际 service scripts 决定：

```text
default foreground
or daemon
```

---

# 25. CLI 应只表达 mode，不实现 daemonize

`cli.c`：

```text
parse --foreground / --daemon
```

即可。

真正：

```text
double fork
startup handshake
```

属于 daemon lifecycle。

---

# 26. `--test` 与 `check` command 重复

当前：

```text
-t / --test
```

设置：

```text
opts->test_config = 1
```

同时又有：

```text
check
```

command。

需要做 callsite audit：

```text
test_config
```

到底是否仍有独立语义。

---

# 27. 如果 `--test` 只是“validate config and exit”

推荐删除：

```text
-t / --test
```

统一：

```text
atpd check [-c FILE]
```

CLI 更清楚。

---

# 28. 不要保留两个相同功能入口

尤其是：

```text
atpd --test
atpd check
```

后续容易：

```text
一个更新了，一个忘了
```

---

# 29. `--force` 也需要 callsite audit

当前 CLI 保存：

```text
opts->force
```

usage：

```text
Skip confirmation for dangerous operations
```

但必须确认当前 branch 是否真的有：

```text
confirmation prompt
dangerous command
```

使用它。

---

# 30. 如果没有 caller

删除：

```text
-F / --force
force field
```

不要保留“未来可能用”的 CLI ABI。

---

# 31. `--no-color` 与 logger/UI ownership

当前：

```text
opts->no_color
```

是合理 CLI intent。

但应用应该：

```text
main/start command
→ logger/ui config
```

不要让 CLI 自己调用 logger。

---

# 32. `cli.c` 当前 include `logger.h`

主要是为了：

```text
LOG_LEVEL_*
```

一旦 verbosity 与 logger enum 解耦：

可以删除 logger include。

---

# 33. `cli.c` include `atp.h` 也要确认必要性

如果只是：

```text
ATP_NAME / ATP_VERSION_STRING
```

版本信息应该来自：

```text
version.h
```

减少 include。

---

# 34. `print_version()` 与 `version.c` ownership

当前 `cli.c` 自己格式：

```text
atpd <version> (Pure eBPF)
```

而项目已有：

```text
version.c
version.h
```

建议：

```text
version module提供 canonical version string
CLI只打印
```

避免 version source 再分裂。

---

# 35. 与 repo/version plan 联动

我们已经发现：

```text
versions.env = 1.0.0
Makefile = 2.0.0
README v2.0 architecture
```

最终：

```text
root VERSION
```

单一来源。

`cli.c` 不拼另外的 edition/version语义。

---

# 36. `print_help()` 只是 `print_usage()` alias

当前：

```c
void print_help(...) {
    print_usage(...);
}
```

两个 public API 没必要。

保留一个：

```text
print_usage
```

或：

```text
cli_print_help
```

---

# 37. `print_version()` 同样可由 version owner承担

最终 `cli.h` 可以更小：

```text
parse
print help
command string
```

---

# 38. `command_to_string()` 应返回稳定 literal

当前做到了。

可以继续保留。

---

# 39. 但 eBPF command cases应删除

```text
CMD_EBPF_PROBE
CMD_EBPF_STATUS
```

一起从 enum 和 mapper移除。

---

# 40. `CMD_NONE → help` 的行为合理

当前没有 command：

```text
opts->command = CMD_HELP
```

这是友好的。

但 exit code要由 main决定：

```text
no args → help + 0
```

可以接受。

---

# 41. Unknown command应该是 usage exit code

当前 parser：

```text
return -1
```

main大概率统一：

```text
exit 1
```

长期建议区分：

```text
usage error
runtime error
config invalid
daemon unavailable
```

---

# 42. CLI parse结果推荐 typed

例如：

```c
typedef enum {
    CLI_PARSE_OK = 0,
    CLI_PARSE_USAGE_ERROR
} cli_parse_result_t;
```

不需要复杂。

---

# 43. main 映射：

```text
CLI_PARSE_USAGE_ERROR
→ EX_USAGE-like code
```

是否采用 POSIX sysexits 数值不是必须。

关键是：

```text
不要与 daemon runtime fail混成一个语义
```

---

# 44. `opterr = 1` 会让 getopt 自己打印错误

同时 parser 又会：

```text
可能由 main 打 usage
```

这容易形成：

```text
双重/不一致错误输出
```

---

# 45. 推荐 `opterr = 0`

由 CLI 自己统一输出：

```text
unknown option
missing argument
usage hint
```

这样测试稳定。

---

# 46. 对 `?` 与 `:` 分别处理

short option string 可以前置：

```text
:
```

让 missing argument得到：

```text
':'
```

而不是和 unknown option都混成 `?`。

例如：

```c
":c:p:fdqFtn6:hv"
```

但 eBPF/legacy options删完以后要重新生成。

---

# 47. Long option unknown也统一走错误 helper

例如：

```text
atpd --does-not-exist start
```

输出：

```text
Unknown option: --does-not-exist
Try 'atpd help'
```

不要依赖 libc 文案。

---

# 48. GNU getopt 参数 permutation 要明确

当前使用 GNU `getopt_long`。

默认可能允许：

```text
atpd start --foreground
```

即使 option在 command后。

---

# 49. 需要决定 CLI grammar

两种都可以：

### A

```text
atpd [global options] COMMAND
```

命令后不再允许 global option。

### B

```text
atpd COMMAND [options]
```

现代 CLI 更常见。

---

# 50. 当前 help写的是：

```text
Usage: atpd [options] command
```

所以更严格的语义应是：

```text
global options必须在 command前
```

---

# 51. 如果选择这个 grammar

short options string可以加：

```text
+
```

让 getopt 遇到第一个 non-option 后停止：

```c
"+c:p:..."
```

这样：

```text
atpd start --foreground
```

会把 `--foreground` 当 command trailing arg：

```text
明确报错
```

---

# 52. 或者重新设计成 command-first parser

如果未来想：

```text
atpd start --foreground
atpd status --json
```

那应该：

```text
先 parse command
再 parse command-specific options
```

这对未来扩展更好。

---

# 53. 本项目更推荐 command-first 长期设计

因为后面很可能出现：

```text
status --json
status --verbose
check --strict
```

这些不是 global daemon start options。

---

# 54. 但当前不用一次大改

第一阶段可以：

```text
保持 [options] command
严格 trailing argv
```

以后 status JSON 落地时再切 command-specific parser。

---

# 55. Help/version early return会忽略后续 argv

例如：

```text
atpd --help garbage
```

当前遇到 `--help`：

```text
立即 return 0
```

所以 garbage不检查。

---

# 56. 这通常是可接受的 CLI习惯

`--help` / `--version` 可作为：

```text
terminal options
```

不用为了严格性强制报错。

只需要文档/测试固定即可。

---

# 57. `opts == NULL` 没防御

当前第一句：

```c
memset(opts, 0, sizeof(atp_options_t));
```

public API caller传 NULL会 crash。

---

# 58. 推荐：

```c
if (!opts || argc < 0 || !argv)
    return CLI_PARSE_USAGE_ERROR;
```

内部调用通常不会错，但 cost极低。

---

# 59. `progname == NULL` 也可防御

`print_usage`：

```c
strrchr(progname, '/')
```

若 NULL会 crash。

用：

```text
"atpd"
```

fallback。

---

# 60. `PATH_MAX` 放进 public options struct合理吗

当前：

```text
config_file[PATH_MAX]
pid_file[PATH_MAX]
```

简单且无 heap ownership，

适合小 daemon。

可以保留。

---

# 61. 但 UDS path不是 PATH_MAX

后面 main/UDS方案已经确认：

```text
sockaddr_un.sun_path ~108 bytes
```

如果未来 CLI支持 socket path：

必须单独验证 protocol limit。

不要因为 options buffer大就认为路径都合法。

---

# 62. Command-specific option validation

当前所有 option：

```text
-c
-p
-f
-d
-V
-q
-F
-t
-n
```

都可与任何 command一起使用。

因此：

```text
atpd --foreground status
atpd --daemon stop
atpd --pid X version
```

parser都可能接受。

---

# 63. 这会产生无意义参数

CLI应区分：

```text
accepted
ignored
invalid
```

最差的是：

```text
accepted but silently ignored
```

---

# 64. 推荐 options applicability matrix

例如：

```text
                 start  stop restart status reload check version help
config             Y     ?      Y      ?      ?     Y      N      N
pid                Y     Y      Y      Y      Y     N      N      N
foreground         Y     N      Y      N      N     N      N      N
daemon             Y     N      Y      N      N     N      N      N
verbosity          Y     maybe  Y      Y      Y     Y      N      N
no-color           Y     Y      Y      Y      Y     Y      N      N
```

具体 `?` 取决于 control path设计。

---

# 65. 与 main command prerequisite方案同步决定

例如长期：

```text
stop/reload/status
```

通过固定 UDS endpoint，

可能根本不需要：

```text
--config
```

而 `--pid` 只作为 fallback。

---

# 66. Parser应拒绝“不适用于此命令”的 option

例如：

```text
atpd --foreground status
```

返回：

```text
--foreground is only valid with start/restart
```

比静默忽略更好。

---

# 67. 实现方式不需要复杂 framework

parse时先记录：

```text
seen_flags
```

command确定后：

```text
validate_cli_options(command, opts, seen)
```

即可。

---

# 68. 这还能检测重复/冲突

例如：

```text
-f + -d
-V + -q
```

二选一：

```text
last-wins
```

或：

```text
conflict error
```

---

# 69. 本项目推荐冲突 error

因为 daemon管理工具通常用于脚本：

```text
明确失败
```

比“顺序决定语义”安全。

---

# 70. 推荐：

```text
--foreground + --daemon
→ usage error

--verbose + --quiet
→ usage error
```

---

# 71. 重复相同选项

例如：

```text
-v? / --config a --config b
```

需要定义。

---

# 72. 路径类推荐重复报错

```text
--config a --config b
```

通常说明脚本拼接错误。

可以：

```text
duplicate option
```

而不是 last-wins。

---

# 73. boolean相同重复可以接受

例如：

```text
--no-color --no-color
```

无害。

不必过度严格。

---

# 74. `-V` 与 `-v` 容易混淆但目前合理

```text
-V verbose
-v version
```

这是常见 Unix习惯之一。

help必须保持清楚。

---

# 75. `-6` 作为需要参数的 IPv6 option不直观

如果未来仍有 IPv6 general setting：

更合理：

```text
--ipv6
--no-ipv6
```

而不是：

```text
--ipv6 1|0
```

但当前它随 eBPF probe删除即可。

---

# 76. CLI parser不要修改 runtime/config对象

当前做得还不错：

```text
只填 opts
```

应继续保持。

不要以后加入：

```text
parse option
→ log_set_level()
→ config mutation
```

---

# 77. 应用 CLI override由 start/config owner处理

例如：

```text
candidate config
+ CLI overrides
→ validated effective startup config
```

而不是 parser自己写 global。

---

# 78. `--pid` 是否应该属于 config override

PID path本质：

```text
runtime control path
```

如果 CLI传入：

```text
--pid
```

需要明确它是否：

```text
只影响 current command
or
覆盖 daemon runtime config
```

---

# 79. 对 `start`

它可以是：

```text
startup override
```

对 `stop/status/reload`：

则是：

```text
control discovery override
```

虽然同一 flag，语义不同。

可以接受，但要文档清楚。

---

# 80. `restart` 的 options传递也要明确

当前 likely：

```text
same opts
→ stop
→ start
```

所以 `--pid` 对两阶段都生效。

这通常合理。

---

# 81. `restart --foreground`

意味着：

```text
stop旧 daemon
start新 foreground
```

合理。

---

# 82. `restart --config new.json`

意味着：

```text
stop current
start with new config
```

也合理。

---

# 83. `check --pid` 应拒绝

因为没有意义。

---

# 84. `version --config` 应拒绝还是忽略？

如果 command-style：

```text
atpd --config X version
```

更推荐：

```text
reject irrelevant option
```

但传统 Unix global option有时会忽略。

ATPD作为管理 daemon，严格更好。

---

# 85. Test：long config path

```text
len == PATH_MAX
```

应：

```text
parse error
```

不能 truncate。

---

# 86. Test：long pid path

同样。

---

# 87. Test：invalid IPv6（若删除前）

```text
abc
1foo
-1
2
```

全部 error。

---

# 88. Test：trailing positional

```text
atpd start foo
atpd stop foo
atpd status foo
```

全部 usage error。

---

# 89. Test：verbosity conflict

```text
-V -q
-q -V
```

如果采用推荐策略：

```text
both usage error
```

不能顺序依赖。

---

# 90. Test：run-mode conflict

```text
-f -d
-d -f
```

同样 error。

---

# 91. Test：irrelevant option

例如：

```text
atpd --foreground status
atpd --force version
```

明确 error。

---

# 92. Test：unknown option

```text
--foobar
-z
```

返回稳定 usage error。

---

# 93. Test：missing option arg

```text
--config
--pid
```

稳定错误信息。

---

# 94. Test：unknown command

```text
atpd starts
```

不能 fuzzy match。

当前 exact strcmp是好的，保留。

---

# 95. Test：no command

```text
atpd
```

输出 help，成功或 usage code需固定。

当前推荐：

```text
help + 0
```

---

# 96. Test：help/version无 daemon side effects

配合 main方案：

```text
parse
→ CMD_HELP / CMD_VERSION
```

不能：

```text
config load
timezone
context
logger file
```

这主要由 main验证。

---

# 97. Test：eBPF CLI消失

完成 removal后：

```text
atpd ebpf probe
```

应：

```text
unknown command
```

或给迁移提示一个 release周期：

```text
eBPF diagnostics moved to `atpd status`
```

---

# 98. 如果兼容期需要提示

可以临时：

```text
CMD_DEPRECATED_EBPF
```

但不建议保留 actual probe。

---

# 99. Test：version single source

```text
atpd --version
atpd version
```

输出完全一致。

版本来自：

```text
version owner
```

---

# 100. `print_usage` 需要与实际 parser自动同步吗

项目很小，

不需要 command registry framework。

但测试应检查重要 option在 help存在。

---

# 101. 可做 golden help test

把：

```text
atpd --help
```

输出存 fixture，

CLI改动时审查 diff。

---

# 102. 不要做动态表驱动过度设计

当前 8个 command，

simple switch/strcmp最易维护。

---

# 103. 推荐最终 `atp_options_t`

大致：

```c
typedef enum {
    CLI_RUN_MODE_DEFAULT = 0,
    CLI_RUN_MODE_FOREGROUND,
    CLI_RUN_MODE_DAEMON
} cli_run_mode_t;

typedef enum {
    CLI_VERBOSITY_DEFAULT = 0,
    CLI_VERBOSITY_VERBOSE,
    CLI_VERBOSITY_QUIET
} cli_verbosity_t;

typedef struct {
    atp_command_t command;

    char config_file[PATH_MAX];
    char pid_file[PATH_MAX];

    cli_run_mode_t run_mode;
    cli_verbosity_t verbosity;

    bool no_color;

    bool config_file_set;
    bool pid_file_set;
} atp_options_t;
```

---

# 104. 为什么需要 `*_set`

因为：

```text
空字符串
默认值
用户显式 override
```

不能只靠：

```text
buffer[0] != '\0'
```

推断所有语义。

---

# 105. `force/test/ipv6` 如果 callsite audit后无必要

直接不进入最终 struct。

---

# 106. Bool统一使用 `bool`

当前 header已经：

```c
#include <stdbool.h>
```

但字段仍然是：

```text
int
```

可以改成：

```text
bool
```

---

# 107. `command_to_string` 返回类型

当前：

```c
const char*
```

风格建议：

```c
const char *command_to_string(...)
```

只是格式，不重要。

---

# 108. `parse_ebpf_command()` 当前手写 option loop也不严格

它：

```text
只识别 --ipv6
其他参数全部忽略
```

例如：

```text
atpd ebpf probe --garbage foo
```

不会主动拒绝所有未知项。

---

# 109. 但无需修这段

因为推荐：

```text
整个删除
```

不要在 obsolete parser上投入时间。

---

# 110. `parse_ebpf_command()` 的 argv重映射也增加理解成本

当前调用：

```c
parse_ebpf_command(argc - optind + 1,
                   &argv[optind - 1],
                   opts);
```

属于为了让内部继续假设：

```text
argv[2] = subcommand
```

而做的 pointer arithmetic。

---

# 111. 如果未来有 subcommand

不要复用这种方式。

应该：

```c
parse_x_command(argc - command_index,
                &argv[command_index],
                opts);
```

内部约定：

```text
argv[0] = command
argv[1] = subcommand
```

清楚很多。

---

# 112. 推荐 Commit 1

```text
cli: reject truncated paths and invalid arguments
```

包括：

```text
config/pid path length
trailing positional args
opts NULL
stable parse errors
```

---

# 113. Commit 2

```text
cli: replace conflicting boolean options with typed modes
```

：

```text
run mode
verbosity
bool fields
```

---

# 114. Commit 3

```text
cli: validate option applicability per command
```

拒绝：

```text
foreground status
pid check
etc.
```

---

# 115. Commit 4

```text
cli: remove obsolete eBPF diagnostics
```

删除：

```text
CMD_EBPF_*
--ipv6
parse_ebpf_command
Pure eBPF wording
LOG/usage examples
```

---

# 116. Commit 5

```text
cli: remove duplicate and unused legacy options
```

callsite确认后删除：

```text
--test
--force
```

若确实无必要。

---

# 117. Commit 6

```text
cli: decouple command parsing from logger internals
```

删除：

```text
#include "logger.h"
log_level_t
```

使用 CLI verbosity。

---

# 118. Commit 7

```text
cli: use canonical version output
```

与 VERSION single source联动。

---

# 119. 不建议拆分

最终即使增加 validation：

```text
cli.c ~200–300 LOC
```

仍然很合理。

不要新建：

```text
cli_parser.c
cli_commands.c
cli_options.c
```

---

# 120. 最终职责边界

`cli.c`：

```text
argv
↓
validated typed options
```

到这里结束。

它不负责：

```text
load config
daemonize
open PID
change logger
connect UDS
change global state
```

---

# 121. 与 `main.c` 方案联动

最终：

```text
parse_arguments
↓
switch command
↓
command-specific facade
```

main不需要重新验证：

```text
CLI conflicts/trailing args
```

parser已经保证 options合法。

---

# 122. 与 `logger.c` 联动

CLI只给：

```text
verbosity intent
no_color intent
```

logger owner映射并应用。

---

# 123. 与 `eBPF removal` 联动

这是删掉用户可见 legacy architecture 的最后几个入口之一。

完成后：

```text
CLI不再声称 ATPD 自己管理/探测 eBPF。
```

---

# 124. 与 `version.c` 联动

CLI不再拼：

```text
Pure eBPF
v2 architecture
```

只展示 canonical product version。

---

# 125. 与 `config` 联动

`-c FILE` 只是：

```text
selected config path
```

CLI不读/validate。

`check/start` owner再决定用途。

---

# 126. 与 `global` 删除联动

CLI当前没有直接 global state，这是好的。

继续保持：

```text
pure parser
```

不要以后加：

```text
g_config override
g_running
logger global writes
```

---

# 127. 最终 Invariants

Codex最终必须保证：

```text
I1:
No CLI path is silently truncated.

I2:
No numeric/boolean option accepts partial or garbage strings.

I3:
Unknown/trailing positional arguments are never silently ignored.

I4:
Mutually exclusive options cannot leave contradictory state.

I5:
Options invalid for a command are rejected rather than ignored.

I6:
CLI parsing has no runtime side effects.

I7:
CLI public types do not depend on logger internals.

I8:
Obsolete ATPD-owned eBPF commands/options are removed.

I9:
Help/version wording reflects the current architecture.

I10:
Version output comes from one canonical version source.
```

---

# 128. 最终验收

## Paths

```text
>= buffer capacity
→ usage error
```

## Strict argv

```text
atpd start foo
→ error
```

## Conflicts

```text
-f -d
-V -q
→ error
```

## Command applicability

```text
--foreground status
→ error
```

## Legacy

```text
ebpf probe/status
--ipv6
Pure eBPF Edition
→ removed
```

## Architecture

```text
cli.h no logger.h
cli.c no runtime/global mutations
```

## Help/version

```text
consistent with parser
canonical VERSION only
```

---

# 129. 最终结论

`cli.c` 当前不是危险模块，但需要从“宽松 parser”收紧成“严格 control-daemon CLI”。

最值得优先修的是：

```text
1. config/pid 路径不能静默截断
2. trailing argv 不能静默忽略
3. mutually-exclusive flags 不应产生矛盾 options state
4. 移除 eBPF probe/status/--ipv6 和 Pure eBPF legacy文案
```

然后逐步删除：

```text
CLI → logger enum
CLI → obsolete test/force options
```

最终它仍然保持一个小模块：

```text
argv
→ typed + validated command/options
```

不需要拆文件，也不要让它承担 daemon policy。
