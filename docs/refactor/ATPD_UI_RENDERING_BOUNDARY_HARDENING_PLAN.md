# ATPD `ui.c / ui.h` 渲染边界与终端安全收敛方案

## 1. 结论

当前：

```text
src/ui.c      450 lines
include/ui.h  157 lines
```

这个模块不是高风险 runtime core，但已经有明显的职责和状态泄漏：

```text
全局输出 FILE*
全局终端宽度
全局 no-color flag
直接读取 g_config
ANSI color policy
emoji policy
status table formatting
```

它不需要机械拆成很多文件。

但随着 `status.c` 计划改成：

```text
runtime producers
→ immutable status_snapshot
→ text / JSON renderer
```

`ui.c` 应同步收敛成：

> 一个无 daemon-global 依赖、无隐藏终端状态、可以对指定输出流稳定渲染纯文本的 presentation helper。

本轮优先级最高的确定 bug 是：

```text
ui_set_no_color() 当前完全无效。
```

---

# 2. P1：`ui_set_no_color()` 是死配置

当前：

```c
static int g_force_no_color = 0;

void ui_set_no_color(int enable) {
    g_force_no_color = enable;
}
```

但是后续所有输出都直接写：

```c
COLOR_CYAN
COLOR_GREEN
COLOR_RED
COLOR_RESET
```

整个 `ui.c` 没有任何地方读取：

```text
g_force_no_color
```

所以：

```text
--no-color
```

即使正确传入 UI，也无法关闭颜色。

---

# 3. 这是用户可见 correctness bug

例如：

```text
atpd --no-color status
```

预期：

```text
纯文本
```

当前 UI renderer仍会输出：

```text
ESC [ ... m
```

ANSI escape。

---

# 4. 不要只在每个函数里加 if

例如不建议机械改成：

```c
if (!g_force_no_color)
    printf(COLOR_GREEN);
```

然后复制几十次。

应该统一：

```text
render context
```

决定：

```text
color enabled
emoji enabled
width
output stream
```

---

# 5. 当前最大的架构问题：`g_ui_out`

当前：

```c
static FILE *g_ui_out = NULL;

void ui_set_output_file(FILE *fp) {
    g_ui_out = fp;
}
```

然后用宏：

```c
#define OUT_FP (g_ui_out ? g_ui_out : stdout)
#define printf(...) fprintf(OUT_FP, __VA_ARGS__)
#define vprintf(...) vfprintf(OUT_FP, __VA_ARGS__)
```

这是一个 process-global mutable output sink。

---

# 6. UDS 当前依赖这个全局切换

`uds.c`：

```c
FILE *mem = open_memstream(&buf, &size);

ui_set_output_file(mem);
status_show(...);
ui_set_output_file(NULL);

fclose(mem);
```

也就是：

```text
整个 process UI output
→ 临时改成某个 UDS request 的 memstream
```

---

# 7. 这是 borrowed FILE lifetime 问题

`g_ui_out` 不拥有：

```text
FILE *
```

但 UI module长期保存这个 pointer。

如果 caller：

```text
忘记 reset
提前 fclose
未来异常 return
```

UI里就留下：

```text
dangling FILE *
```

---

# 8. C 当前没有 exception，但 contract仍然脆弱

今天 `handle_status()` 路径比较简单。

但未来如果：

```text
status renderer提前返回
增加错误分支
嵌套 render
多个线程调用
```

就容易：

```text
输出跑错 sink
use-after-close
内容串流
```

---

# 9. 最好的修法不是“记得 reset”

应该取消：

```text
ui_set_output_file()
```

这种 process-global setter。

---

# 10. 推荐显式 render context

例如：

```c
typedef struct {
    FILE *out;
    int width;
    bool color;
    bool emoji;
} ui_render_ctx_t;
```

所有 renderer：

```c
ui_table_row(ctx, ...);
ui_title(ctx, ...);
```

---

# 11. 如果嫌每个函数都传 ctx 太啰嗦

可以建立：

```c
ui_writer_t
```

然后 status renderer持一个 pointer。

这是 presentation 层，

显式 ctx 的代价很低。

---

# 12. 不建议 TLS/global stack hack

不要做：

```text
thread-local current UI
push_output/pop_output
global render stack
```

会把简单问题复杂化。

---

# 13. 当前 terminal width 取错对象

`get_terminal_width()`：

```c
ioctl(STDOUT_FILENO, TIOCGWINSZ, &w)
```

无论：

```text
g_ui_out
```

指向什么，

都固定看：

```text
STDOUT_FILENO
```

---

# 14. UDS status 因此很奇怪

daemon收到：

```text
status request
```

后，把内容 render到：

```text
open_memstream
```

但 UI width来自：

```text
daemon自己的 stdout
```

不是：

```text
客户端 terminal
```

---

# 15. 如果 daemon已经后台化

STDOUT通常：

```text
/dev/null
```

`ioctl`失败，

于是 status固定按：

```text
80 columns
```

render。

客户端即使：

```text
手机窄终端
宽桌面终端
```

都不知道。

---

# 16. UDS protocol不应该由 server决定终端 presentation

正确边界更推荐：

```text
daemon
→ structured/plain status data

CLI client
→ 根据自己的 TTY/width/color偏好 render
```

---

# 17. 这与 status snapshot方案完全一致

最终：

```text
daemon status snapshot
      │
      ├─ UDS structured response / JSON
      │
      └─ local renderer

atpd status client
      ↓
知道自己的 terminal width
知道 --no-color
知道 emoji preference
      ↓
ui renderer
```

---

# 18. 如果第一阶段还不改 UDS protocol

至少 server-rendered UDS status应：

```text
固定 plain text
color=false
width=80
```

不要输出 daemon终端相关 escape。

这是兼容期最安全行为。

---

# 19. 当前 UDS status 很可能包含 ANSI escape

因为：

```text
status_show()
→ ui_table_*_color()
→ 固定 COLOR_xxx
```

而 `ui_set_no_color()` 又无效。

所以 memstream会收集：

```text
ANSI color codes
```

然后直接通过 socket发送。

---

# 20. 这让 protocol payload 与 terminal presentation耦合

例如未来：

```text
GUI
script
JSON adapter
test parser
```

消费 UDS text时，

必须先去 ANSI。

不合理。

---

# 21. 推荐 protocol invariant

```text
UDS machine/control response contains no ANSI escape sequences.
```

颜色只在：

```text
interactive CLI renderer
```

生成。

---

# 22. Emoji policy也不该由 daemon global config直接控制

当前每一个：

```c
ui_emoji_ok()
ui_emoji_service()
ui_emoji_vpn()
...
```

都读取：

```c
g_config.core.ui_emoji_enabled
```

所以 `ui.c`：

```text
#include "atpd_global.h"
```

---

# 23. 这是 atpd_global elimination 的直接阻塞点

UI只是 presentation helper，

不应该知道：

```text
daemon config root
```

---

# 24. 推荐：

```text
emoji bool
```

进入：

```text
ui_render_ctx_t
```

render时决定。

这样：

```text
ui.c
```

可以完全删除：

```text
atpd_global.h
g_config
```

---

# 25. Emoji preference属于谁

如果是 daemon config：

```text
config snapshot
→ CLI/UI prefs
```

如果未来变成纯 client preference：

更合理可以：

```text
CLI --emoji / --no-emoji
terminal capability
```

但第一阶段不需要改产品语义。

只需要：

```text
不要由 UI主动查 global config。
```

---

# 26. Color definitions和 logger重复

`ui.h`自己定义：

```text
COLOR_RESET
COLOR_RED
COLOR_GREEN
...
```

并用：

```c
#ifndef COLOR_RESET
```

声称：

```text
compatible with logger.h
```

---

# 27. 这说明两个 presentation subsystem共享了一套隐式 macro ABI

并不理想。

logger和UI虽然都用ANSI，

但职责不同。

---

# 28. 推荐 UI 内部私有 color sequence

例如：

```c
static const char ANSI_RED[] = "...";
```

或 private header：

```text
terminal_ansi.h
```

只有真的有第三个消费者时才抽。

---

# 29. 不建议建立大型 color subsystem

当前只需消除：

```text
public global COLOR_* macro collision
```

即可。

---

# 30. Status.c 当前直接引用 `COLOR_*`

例如：

```c
ui_table_row_color(..., COLOR_GREEN);
```

这进一步把 renderer style泄漏到 status collector。

---

# 31. 长期更推荐 semantic style

例如：

```c
typedef enum {
    UI_STYLE_DEFAULT,
    UI_STYLE_OK,
    UI_STYLE_WARN,
    UI_STYLE_ERROR,
    UI_STYLE_INFO
} ui_style_t;
```

然后：

```c
ui_table_row(ctx, label, value, UI_STYLE_OK);
```

---

# 32. 为什么 semantic style更好

status只知道：

```text
这个值是 OK/WARN/ERROR
```

不需要知道：

```text
ANSI green = \033[...
```

JSON renderer则完全忽略 style。

---

# 33. 第一阶段可以继续颜色参数吗

可以。

如果要最小改动：

```text
先修 no-color + global ctx
```

第二阶段随 status renderer再改 semantic style。

不需要一次重写所有 callsite。

---

# 34. `truncate_string()` 用字节长度处理 UTF-8

当前：

```c
int len = strlen(src);
```

然后：

```c
strncpy(dst, src, max_len - 3);
```

`strlen` 是：

```text
bytes
```

不是 terminal columns。

---

# 35. 对 emoji/CJK会失真

例如：

```text
✓
🚀
中文
```

UTF-8单字符可能：

```text
2–4 bytes
```

terminal显示宽度可能：

```text
1或2 columns
```

当前计算完全不同。

---

# 36. 更严重：截断可能切断 UTF-8 sequence

如果：

```text
max_len
```

正好落在某个 multibyte character中间，

输出会成为：

```text
invalid UTF-8
```

---

# 37. 当前很多 value是 ASCII

所以这个 bug不一定高频。

但 UI本身明确支持：

```text
emoji
手机终端
```

因此不能宣称“adaptive width”却使用纯 byte truncation。

---

# 38. 修法有两档

### 简单版

不要主动 byte truncate UTF-8 input：

```text
让 terminal自然 wrap
```

只对已知 ASCII internal values truncate。

### 完整版

实现：

```text
UTF-8 decode + wcwidth/display-width
```

---

# 39. ATPD更推荐先简单

这是 daemon CLI，

不需要做完整 terminal layout engine。

建议：

```text
避免切断 UTF-8
```

优先。

真正 display-width完美对齐不是 release blocker。

---

# 40. 至少做 UTF-8 boundary-safe truncation

如果必须截断：

```text
回退到合法 codepoint boundary
```

再加：

```text
...
```

---

# 41. 但 `...` 仍按3列

ASCII没问题。

---

# 42. `truncate_string()` 没有 NULL defensive check

当前：

```text
strlen(src)
```

若 caller传：

```text
NULL
```

直接 crash。

---

# 43. 多个 public UI API同样假设 non-NULL

例如：

```text
title
label
value
color
emoji
```

内部调用通常正确。

但 presentation helper做轻量防御很便宜。

---

# 44. 推荐统一：

```text
NULL string → ""
```

color NULL：

```text
default
```

不要 crash status renderer。

---

# 45. `ui_table_row_color()` 的格式和普通 row不同

普通：

```c
printf("  %-*s  %s\n", label_width, label, value);
```

color：

```c
printf("  %s%s" COLOR_RESET "  %s\n", color, label, value);
```

它不再使用：

```text
label_width
```

对齐。

---

# 46. 所以带颜色 row会打破表格列宽

这是 visual correctness bug。

应保持：

```text
颜色码不影响 visible width
```

同时用相同 label padding。

---

# 47. `ui_table_subrow_color()` 同样

当前：

```text
不使用 label_width 做 padding
```

与非color版本表现不一致。

---

# 48. 这个问题与 ANSI sequence本身无关

可以：

```text
先输出 color
再 %-*s label
再 reset
```

例如：

```c
fprintf(out, "    %s%-*s%s  %s\n",
        color, label_width, label, reset, value);
```

---

# 49. Emoji combined row也可能 width不准

```c
char combined[128];
snprintf(combined, "%s %s", emoji, label);

printf("%-*s", label_width + 2, combined);
```

`printf` field width仍然按：

```text
bytes
```

而不是 terminal cell width。

---

# 50. 不要继续声称“精确 adaptive width”

如果不实现 wcwidth，

UI注释应更诚实：

```text
best-effort terminal formatting
```

---

# 51. `ui_table_subrow()` 完全忽略 prefix

header说：

```text
Draw sub-row with prefix (├─, └─, etc.)
```

实现：

```c
(void)prefix;
printf("    ...");
```

---

# 52. 这是 API contract不一致

caller传：

```text
├─
└─
```

以为会显示树结构，

实际被丢弃。

---

# 53. 二选一

### A

真正 render prefix。

### B

删除 prefix 参数。

---

# 54. 推荐 B

因为注释已经说：

```text
plain text只用 spaces
```

那么 API就不该继续要求 caller构造：

```text
"├─"
"└─"
```

这只是噪声。

---

# 55. Status.c因此有大量无意义字面量

例如：

```text
"├─"
"└─"
```

都被 UI丢掉。

删除 prefix 参数可以显著简化调用。

---

# 56. `ui_table_begin()` / `ui_table_sep()` 是 no-op

当前：

```c
void ui_table_begin(void) {
    ensure_init();
}

void ui_table_sep(void) {
    ensure_init();
}
```

没有其他行为。

---

# 57. 如果没有 future state machine需要

这些 API应删除。

否则 caller以为：

```text
begin/end
```

有成对 contract，

但其实：

```text
begin no-op
end只打印 newline
```

---

# 58. 推荐 status renderer直接：

```text
header
rows
blank
```

更直观。

---

# 59. `ui_banner()` 直接硬编码 ANSI

即使 no-color未来修好其他函数，

这里：

```c
printf("\033[1;36m"...)
```

仍会输出颜色。

---

# 60. `ui_banner_with_version()` 同样

所以 color policy必须覆盖：

```text
所有输出函数
```

不能漏 banner。

---

# 61. Banner是否还需要

如果只在 interactive CLI显示：

可以保留。

UDS/status JSON绝不能包含。

---

# 62. UI output error目前完全不检查

几乎所有：

```text
fprintf
vfprintf
```

return被忽略。

例如：

```text
EPIPE
disk/full memstream unlikely
closed stdout
```

presentation失败不应该 crash daemon。

---

# 63. 需要不需要每行返回 int

不建议把所有函数都复杂化。

对 CLI presentation：

```text
best effort
```

合理。

---

# 64. 但 status UDS renderer不应依赖 FILE global

如果最终：

```text
status_render_text(snapshot, buffer/builder)
```

可以直接得到：

```text
成功/截断
```

更清楚。

---

# 65. `FILE *` 还是可以保留

`open_memstream` 很方便。

核心不是 FILE不好，

而是：

```text
不要存在 process-global current FILE*。
```

---

# 66. 更小改法

如果 status重构还没开始，

可先把：

```c
void status_show_to(FILE *out, ..., const ui_options_t *opts);
```

内部构造 local UI ctx。

这样 UDS不再：

```text
ui_set_output_file(mem)
```

---

# 67. Thread safety

当前 global mutable：

```text
g_ui_out
g_force_no_color
g_term_width
g_initialized
```

全部没有 mutex/atomic。

---

# 68. 如果 UI只在 reactor/main单线程调用

今天大概率不会出 race。

但 public module没有写明：

```text
single-thread only
```

---

# 69. 最好的修法不是加 mutex

因为这些 global根本可以不存在。

local render ctx天然：

```text
thread-safe by isolation
```

无需锁。

---

# 70. `ui_init()` lazy global init也可以消失

ctx创建时：

```text
detect terminal
clamp width
```

即可。

---

# 71. Terminal width需要根据实际 output FD

如果：

```text
ctx->out == stdout
```

可以：

```text
fileno(out)
ioctl TIOCGWINSZ
```

如果：

```text
memstream
pipe
file
```

则：

```text
固定默认宽度
```

---

# 72. 更严格：

```text
isatty(fileno(out))
```

后才：

```text
ioctl
color
```

---

# 73. Color默认也应该看真正输出 sink

类似 logger review：

```text
interactive TTY
→ color on

pipe/file/memstream
→ color off
```

除非 user显式 force。

---

# 74. 这会自然修复：

```text
atpd status | grep ...
```

当前如果默认有颜色，

脚本处理会麻烦。

---

# 75. 推荐 color mode不是 bool

可以：

```c
typedef enum {
    UI_COLOR_AUTO,
    UI_COLOR_ALWAYS,
    UI_COLOR_NEVER
} ui_color_mode_t;
```

---

# 76. 当前 CLI只有 `--no-color`

因此第一阶段：

```text
AUTO / NEVER
```

足够。

不必提供 `--color=always`。

---

# 77. Emoji也最好在非TTY时关闭吗

不一定。

UTF-8 pipe仍然完全可以接受 emoji。

这是产品选择。

先保持现有 config语义即可。

---

# 78. `ui_status_async("ASYNC")`

结合 API review：

```text
假 async API要删除
```

这个 UI helper可能也变成 dead code。

全仓 audit后删除未使用 status helpers。

---

# 79. Emoji helper也有大量 legacy domain

当前：

```text
app_filter
mac_filter
geo_bypass
temperature
speed...
```

需要 callsite audit。

---

# 80. 不要保留无 caller emoji API

UI header 157行，

相当一部分只是：

```text
返回某个 emoji literal
```

如果 status重构后不使用：

直接删。

---

# 81. 例如：

```text
ui_emoji_app_filter
ui_emoji_mac_filter
ui_emoji_geo_bypass
```

可能来自旧 architecture。

先 grep后决定。

---

# 82. 这不是性能问题

而是：

```text
public UI API表面面积过大
```

让后续代码误以为这些 feature仍属于当前 product。

---

# 83. 与 eBPF removal联动

UI/status里仍有大量：

```text
Pure eBPF
Zero iptables
Direct eBPF
cgroup socket interception
```

其中大部分在 `status.c`，

不是 `ui.c`。

但 UI重构测试应确保：

```text
presentation层不再固化旧 architecture wording。
```

---

# 84. `ui.c` 自身 banner不含 eBPF

这很好。

---

# 85. 与 `atpd_global` 方案联动

这是一个非常明确的迁移目标：

```text
ui.c
```

最终：

```text
不 include atpd_global.h
不读 g_config
```

---

# 86. 与 config方案联动

如果：

```text
ui_emoji_enabled
```

仍是 config field，

status/CLI在构造 render options时 copy：

```text
emoji_enabled
```

即可。

---

# 87. 与 status方案联动

理想：

```c
status_snapshot_t snap;
status_collect(&snap);

ui_render_status(out, &snap, &render_opts);
```

UI只看：

```text
snapshot values
render options
```

---

# 88. UI绝不能重新查询 runtime

当前 `ui.c` 本身还没有查 service/netlink，

这是好事。

继续保持。

---

# 89. 与 UDS方案联动

最终 UDS更推荐：

```text
machine-readable snapshot
```

而不是：

```text
daemon terminal text
```

---

# 90. 如果短期保持 text UDS

必须：

```text
plain
no ANSI
fixed deterministic width
```

这样测试和脚本稳定。

---

# 91. 与 CLI方案联动

`--no-color`：

```text
CLI intent
→ local render opts
```

不是：

```text
修改全局 g_force_no_color
```

---

# 92. 与 logger方案联动

不要共用：

```text
COLOR_* macros
global color switch
```

两个模块各自消费同一个：

```text
CLI no-color intent
```

即可。

---

# 93. Unit test：no-color

调用：

```text
render_opts.color = NEVER
```

所有输出中搜索：

```text
"\033["
```

必须：

```text
0 occurrence
```

包括：

```text
title
separator
table color rows
info/success/warn/error
banner
```

---

# 94. Regression test：当前 bug

专门验证：

```text
ui_set_no_color / replacement API
```

确实影响输出。

---

# 95. Test：UDS text has no ANSI

发：

```text
status
```

response：

```text
grep ESC
→ none
```

---

# 96. Test：custom output isolation

两个 render ctx：

```text
A → stream A
B → stream B
```

交替调用。

内容不能串。

---

# 97. 如果支持 threads

并发 A/B：

```text
TSan 0 race
stream内容独立
```

local ctx自然通过。

---

# 98. Test：FILE lifetime

render结束后：

```text
UI不保存 FILE pointer
```

可关闭 stream，

后续另一 render不访问旧 stream。

---

# 99. Test：terminal width

mock/fixed：

```text
20 → clamp 30
80 → 80
300 → clamp 200
```

如果仍保留 clamp。

---

# 100. Test：non-TTY width

pipe/memstream：

```text
固定 deterministic width
```

例如 80。

不能取 daemon stdout。

---

# 101. Test：colored/noncolored alignment

相同 label/value：

```text
visible columns
```

color on/off应保持同一基本布局。

至少 ASCII case完全一致。

---

# 102. Test：long UTF-8 value

输入：

```text
中文 / emoji / mixed
```

截断后必须：

```text
valid UTF-8
```

不能切半字符。

---

# 103. 不要求第一阶段精确 wcwidth

只要求：

```text
valid UTF-8
no OOB
deterministic
```

---

# 104. Test：NULL strings

如果决定 defensive支持：

```text
NULL title/value/label
```

不 crash。

---

# 105. Test：long title

title > width：

当前：

```text
padding变负
```

for loop自然不跑，

但整行超过 width。

应定义：

```text
truncate title
or allow overflow
```

推荐安全 truncate。

---

# 106. `print_section_header()` 也应使用 safe truncation

目前 title不受 width限制。

可以保证：

```text
title visible width <= width - 4
```

---

# 107. Test：no-op APIs清理

如果删除：

```text
ui_table_begin
ui_table_sep
prefix
```

callsite编译测试自然确保迁移完整。

---

# 108. 推荐 Commit 1

```text
ui: make no-color effective for every output path
```

这是明确 bugfix。

如果 status refactor尚未进行，

可先最小修。

---

# 109. Commit 2

```text
ui: replace global output and render flags with explicit context
```

删除：

```text
g_ui_out
g_force_no_color
g_initialized
g_term_width
ui_set_output_file
ui_set_no_color
```

或把后两个改成 context setter。

---

# 110. Commit 3

```text
ui: remove global config dependency
```

emoji preference显式传入。

删除：

```text
#include "atpd_global.h"
```

---

# 111. Commit 4

```text
ui: make non-terminal output deterministic and ANSI-free
```

特别是 UDS/memstream。

---

# 112. Commit 5

```text
ui: normalize colored and plain table alignment
```

修：

```text
ui_table_row_color
ui_table_subrow_color
```

---

# 113. Commit 6

```text
ui: make truncation UTF-8 boundary safe
```

不要过度做 full terminal layout engine。

---

# 114. Commit 7

```text
ui: remove no-op and stale presentation APIs
```

审计：

```text
table_begin
table_sep
prefix
unused emoji helpers
status_async
```

---

# 115. Commit 8

随 status refactor：

```text
status: render snapshots through local UI context
```

UDS不再改 UI process-global state。

---

# 116. 不建议拆成很多文件

当前 450行主要是重复 presentation helper。

收敛 API后可能自然变成：

```text
250–350 LOC
```

保持：

```text
ui.c / ui.h
```

即可。

---

# 117. 如果未来 JSON renderer增加

JSON不应该放：

```text
ui.c
```

而应：

```text
status_json.c
```

或 status renderer owner。

UI只负责 interactive text。

---

# 118. 最终推荐职责

```text
ui.c
```

只负责：

```text
terminal/plain-text formatting
ANSI styling
width handling
emoji display choice
```

它不负责：

```text
collect status
read config globals
query service
query API
own output stream globally
decide UDS protocol
```

---

# 119. 推荐最终 context

大致：

```c
typedef struct {
    FILE *out;
    int width;
    bool color_enabled;
    bool emoji_enabled;
} ui_render_ctx_t;
```

create/init：

```c
void ui_render_ctx_init(
    ui_render_ctx_t *ctx,
    FILE *out,
    int width,
    bool color_enabled,
    bool emoji_enabled);
```

---

# 120. 如果 width = 0

可以定义：

```text
auto detect if tty
otherwise 80
```

这样测试也能强制指定 width。

---

# 121. 更进一步可把 emoji helper接受 ctx

例如：

```c
const char *ui_emoji_ok(const ui_render_ctx_t *ctx);
```

或者 renderer直接：

```text
ctx->emoji_enabled ? "✓" : "[OK]"
```

不需要几十个 global helper。

---

# 122. 不要引入 heap allocation

ctx完全可以：

```text
stack-owned
```

render期间有效。

没有 lifecycle复杂度。

---

# 123. 最终 Invariants

Codex最终必须保证：

```text
I1:
No-color mode produces zero ANSI escape sequences on every UI output path.

I2:
UI never stores a borrowed FILE* in process-global mutable state.

I3:
UI terminal width is derived from the actual output context, not always STDOUT_FILENO.

I4:
Machine/UDS responses are ANSI-free.

I5:
ui.c does not include atpd_global.h or read g_config.

I6:
Emoji/color/width are explicit render preferences.

I7:
Colored and plain rows have equivalent layout semantics.

I8:
String truncation never emits invalid UTF-8.

I9:
UI has no runtime/service/API/config-query side effects.

I10:
Status JSON or machine formatting is not implemented inside ui.c.
```

---

# 124. 最终验收标准

## Color

```text
--no-color
→ no ESC sequences
```

## UDS

```text
status response
→ deterministic plain/machine output
→ no ANSI
```

## Isolation

```text
two render outputs
→ no cross-stream leakage
```

## Global dependencies

```text
grep atpd_global src/ui.c
→ 0

grep g_config src/ui.c
→ 0
```

## Layout

```text
color on/off ASCII rows align identically
```

## UTF-8

```text
long emoji/CJK input
→ valid output
```

## API cleanup

```text
no meaningless table begin/sep/prefix APIs if unneeded
```

---

# 125. 最终结论

`ui.c` 不是一个需要重写的模块。

但它目前确实有一个直接用户可见 bug：

```text
ui_set_no_color() 根本不起作用。
```

更重要的是，它通过：

```text
g_ui_out
g_term_width
g_config
```

把 presentation 做成了 process-global state。

这在 UDS status 场景尤其不合理：

```text
daemon端决定 client 的颜色、宽度和 emoji presentation
```

正确方向是：

```text
status snapshot
      ↓
local render context
      ↓
FILE / terminal
```

而不是：

```text
修改 process-global UI sink
→ 调 status
→ 再恢复 global sink
```

因此这轮推荐：

> 加固 + 去全局化，不拆文件。

等这一步和 status snapshot方案结合后，`ui.c` 会真正成为一个很干净的 presentation-only 模块。
