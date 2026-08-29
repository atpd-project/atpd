# ATPD `config_validator.c / config_validator.h` 严格校验与纯函数化方案

## 1. 结论

当前：

```text
src/config_validator.c      144 lines
include/config_validator.h   18 lines
```

模块本身很小，不需要拆文件。

但当前 validator 的核心问题不是“规则不够多”，而是：

```text
strict mode 实际没有接入 config loader
unknown key 仍然静默忽略
validator 会直接 LOG_ERROR，有副作用
strict mode 是 process-global mutable state
value validation 覆盖面很窄
key schema 与 parser 是两份手写列表
eBPF/legacy key 已经过时
```

最重要的目标：

> 把 validator 变成真正的“纯、严格、可组合的 candidate validation”，让 transactional reload 可以依赖它，而不是把错误留到 runtime apply 阶段才爆出来。

---

# 2. P0/P1：`config_validate_key()` 当前实际上没有参与 config load

当前 `config_load_file()`：

```c
while (fgets(...)) {
    ...
    char *k = line;
    char *v = eq + 1;
    ...
    parse_key_value(k, v, cfg);
}
```

中间没有：

```text
config_validate_key()
```

所以：

```text
VALID_CONFIG_KEYS[]
g_strict_mode
config_set_strict_mode()
```

都没有真正形成 parser gate。

---

# 3. 结果：unknown key 会被静默忽略

例如：

```text
API_P0RT=9080
SERVICE_START_TIMOUT=30
VPN_TAREGT_MODE=Rule
```

拼写错了。

`parse_key_value()` 找不到 matching branch：

```text
什么都不做
```

最终 config load仍然：

```text
ATP_OK
```

---

# 4. 这是 production config 的高风险行为

用户会认为：

```text
配置已经生效
```

实际运行的是：

```text
default / old behavior
```

这种错误比显式启动失败更难排查。

---

# 5. 推荐默认严格模式

ATPD 是 root daemon，不是容错型 desktop app。

建议：

```text
unknown key = validation error
```

成为默认行为。

不再保留：

```text
silent ignore
```

作为生产默认。

---

# 6. 如果确实需要兼容旧 key

不要用：

```text
strict=false
```

把所有 unknown 都放行。

应该明确：

```text
deprecated alias table
```

例如：

```text
CLASH_SECRET → API_SECRET
VPN_CLASH_MODE → VPN_TARGET_MODE
```

并：

```text
WARN deprecated
still parse
```

---

# 7. `g_strict_mode` 不应是 process-global mutable state

当前：

```c
static int g_strict_mode = 0;
```

setter/getter无 mutex/atomic。

多线程下：

```text
data race
```

更重要的是它从 architecture 上也是错误的。

---

# 8. Strictness 应是 validation policy，而不是全局开关

例如：

```c
typedef struct {
    bool reject_unknown_keys;
    bool reject_deprecated_keys;
} config_validation_policy_t;
```

caller明确传入。

---

# 9. 更简单的 ATPD 做法

如果 production 永远 strict：

可以连 policy都不要。

直接：

```text
unknown key = error
```

测试工具如需宽松模式：

另提供 test-only path。

这比维护 global strict mode更简单。

---

# 10. `config_validate_key(const char *key, atp_config_t *cfg)` 参数设计不合理

当前：

```text
cfg 完全没用
```

实现：

```c
(void)cfg;
```

说明 API contract已经漂移。

应改成：

```c
config_key_result_t config_validate_key(const char *key);
```

或者直接由 schema lookup返回 metadata。

---

# 11. 当前 key schema 与 parser 是两份独立 truth

`config_validator.c`：

```text
VALID_CONFIG_KEYS[]
```

`config.c`：

```text
if strcmp(k, ...)
else if strcmp(k, ...)
```

两份都要手工同步。

---

# 12. 这很容易产生 drift

例如：

```text
validator认为 key合法
parser实际上没处理
```

或者：

```text
parser支持了新 key
validator忘了加
```

---

# 13. 推荐 single key schema

例如：

```c
typedef enum {
    CFG_VALUE_BOOL,
    CFG_VALUE_INT,
    CFG_VALUE_STRING
} config_value_type_t;

typedef struct {
    const char *name;
    config_value_type_t type;
    bool deprecated;
    const char *canonical_name;
} config_key_spec_t;
```

---

# 14. 不一定要一步做到通用 schema parser

ATPD 当前 key数量不多。

第一阶段可以仅建立：

```text
canonical key registry
```

由：

```text
validator
parser
help/tests
```

共用。

不要再两份字符串表。

---

# 15. `VALID_CONFIG_KEYS[]` 里已经有明显 legacy

当前包含：

```text
ENABLE_EBPF
CLASH_SECRET
VPN_AUTO_CLASH_MODE
VPN_CLASH_MODE
VPN_DEFAULT_MODE
WORK_DIR
```

其中至少：

```text
ENABLE_EBPF
```

会随 ATPD eBPF module removal 删除。

---

# 16. Alias key 要分类

例如：

```text
API_SECRET / CLASH_SECRET
VPN_AUTO_MODE / VPN_AUTO_CLASH_MODE
VPN_TARGET_MODE / VPN_CLASH_MODE
VPN_FALLBACK_MODE / VPN_DEFAULT_MODE
DATA_DIR / WORK_DIR
```

不能全部当成平级 canonical key。

---

# 17. 推荐 canonical 化

例如：

```text
API_SECRET
VPN_AUTO_MODE
VPN_TARGET_MODE
VPN_FALLBACK_MODE
DATA_DIR
```

老名字：

```text
deprecated alias
```

---

# 18. 兼容策略建议

Beta/RC 阶段：

```text
deprecated alias accepted + WARN
```

v1.0 前可决定：

```text
保留一周期
or
删除
```

但状态必须明确。

---

# 19. `config_validate_values()` 当前只检查少数 int

目前只检查：

```text
API_PORT
service timeouts/failures/circuit values
RESTART_DELAY
```

这远远不等于 config candidate 已经安全。

---

# 20. 需要校验 bool 语义

当前 parser：

```c
int_val != 0
```

意味着：

```text
UI_EMOJI_ENABLED=999
VPN_AUTO_MODE=-3
ENABLE_EBPF=42
```

都可能被当成 true。

---

# 21. Boolean 应严格只接受

```text
0
1
```

或者明确支持：

```text
true/false
yes/no
```

但不能：

```text
任意非0 = true
```

---

# 22. 最推荐保持配置格式简单

当前是 KEY=VALUE。

建议 bool 只允许：

```text
0
1
```

错误则：

```text
line-level parse error
```

---

# 23. Validator 目前不知道原始值

因为 parser先：

```text
把 value 写进 struct
```

之后才：

```text
config_validate_values(cfg)
```

因此：

```text
UI_EMOJI_ENABLED=2
```

进入 bool后只剩：

```text
true
```

已经丢失原始错误信息。

---

# 24. 这说明需要两阶段校验

推荐：

```text
parse token
↓
key/type/value-level validation
↓
write candidate struct
↓
cross-field validation
```

---

# 25. 不要先 coercion 再 validation

否则很多非法输入会被：

```text
规范化/吞掉
```

---

# 26. `config_parse_int()` 已经比 atoi 严格

这是好的一点：

```text
strtol
ERANGE
full consumption
INT range
```

应该保留/复用。

---

# 27. 但 `parse_key_value()` 的控制流会制造“数字解析失败后当字符串解析”的奇怪语义

当前：

```c
if (config_parse_int(v, &int_val) == 0) {
    handle integer keys
} else {
    handle string keys
}
```

这意味着：

```text
key 的 expected type
```

不是先决定。

而是：

```text
value 能不能 parse int
```

先决定分支。

---

# 28. 示例

如果：

```text
API_PORT=abc
```

int parse失败。

然后进入 string branch。

string branch没有 API_PORT：

```text
最终什么都不做
```

于是：

```text
API_PORT仍然 default
config load成功
```

---

# 29. 这是一个严重 parser correctness bug

配置：

```text
API_PORT=abc
```

应该明确：

```text
invalid integer
```

而不是：

```text
悄悄回落到 9080
```

---

# 30. 同理

```text
SERVICE_START_TIMEOUT=abc
RESTART_DELAY=abc
UI_EMOJI_ENABLED=abc
VPN_AUTO_MODE=abc
```

都可能被 silent ignore。

---

# 31. 正确 parser必须 key-first

即：

```text
lookup key spec
↓
知道 expected type
↓
按 type parse
↓
失败 = error
```

而不是：

```text
先猜 value类型
↓
再猜 key
```

---

# 32. 这是本轮最重要的结构性修复之一

推荐：

```c
const config_key_spec_t *spec = config_find_key(k);

if (!spec)
    return UNKNOWN_KEY;

switch (spec->type) {
case CFG_VALUE_BOOL:
    parse_bool_strict(v,...)
case CFG_VALUE_INT:
    parse_int_strict(v,...)
case CFG_VALUE_STRING:
    validate/copy_string(...)
}
```

---

# 33. String copy 当前也会静默截断

大量：

```c
snprintf(field, sizeof(field), "%s", v);
```

如果 value太长：

```text
被截断
```

但 parser仍然成功。

---

# 34. 这和 CLI path问题一样

配置路径、secret、host、service args：

```text
不能静默变成另一个值。
```

---

# 35. 所有 fixed-buffer string 应先检查长度

例如：

```text
strlen(v) < sizeof(field)
```

否则：

```text
validation error
```

---

# 36. `CORE_USER_GROUP` 更明显

当前：

```text
char val[256];
snprintf(val, sizeof(val), "%s", v);
```

如果输入 >255：

先截断。

之后再：

```text
strchr(':')
```

可能产生完全不同的 user/group。

---

# 37. 应先验证完整原始字符串

例如：

```text
exactly one separator
user non-empty
group non-empty
each < field size
```

---

# 38. user/group 是否存在应该在哪里检查

当前 validator include 了：

```text
pwd.h
grp.h
```

但实际上没用。

这说明可能原计划做 existence check，但没有实现。

---

# 39. 这里要区分 syntax validation 与 environment validation

建议：

### Candidate syntax validation

```text
string格式
长度
允许字符
```

### Prepare/runtime environment validation

```text
user存在
group存在
path可访问
binary存在
```

---

# 40. Validator 尽量保持纯

它不应该：

```text
getpwnam
getgrnam
access
open
network call
```

否则：

```text
check command
reload
unit test
```

都变成 environment-dependent。

---

# 41. 当前 validator 直接 `LOG_ERROR()`，因此不是纯函数

例如：

```c
LOG_ERROR("API_PORT must...");
```

这意味着：

```text
validation
```

同时修改/写 logger状态。

---

# 42. 这会导致几个问题

- unit test必须捕获日志；
- `atpd check` 不容易输出 structured errors；
- reload想把错误写 status snapshot时还得重新解析日志；
- config validator 和 logger形成不必要依赖。

---

# 43. 推荐 structured validation errors

例如：

```c
typedef enum {
    CFG_ERR_UNKNOWN_KEY,
    CFG_ERR_BAD_TYPE,
    CFG_ERR_RANGE,
    CFG_ERR_TOO_LONG,
    CFG_ERR_INVALID_FORMAT,
    CFG_ERR_CONFLICT
} config_validation_code_t;

typedef struct {
    config_validation_code_t code;
    int line;
    char key[64];
    char message[160];
} config_validation_error_t;
```

---

# 44. 不需要 enterprise error framework

可以固定：

```text
最多 16/32 errors
```

一次收集。

例如：

```c
typedef struct {
    config_validation_error_t items[32];
    size_t count;
    bool truncated;
} config_validation_report_t;
```

---

# 45. Validator只填 report

然后：

```text
CLI check → print report
reload → status/log report summary
startup → log report
tests → inspect report
```

---

# 46. 这样 `config_validator.c` 可以删除 logger include

当前 include：

```text
logger.h
utils.h
net/if.h
arpa/inet.h
pwd.h
grp.h
errno.h
unistd.h
```

多数实际上未使用。

---

# 47. 最终 validator dependency 应非常小

大致：

```text
config schema
string/int helpers
limits
```

---

# 48. `config_validate_values(atp_config_t *cfg)` 没有 NULL check

当前第一句直接：

```c
cfg->api.port
```

传 NULL：

```text
crash
```

---

# 49. 改成 const

它本来不应修改 cfg：

```c
int config_validate_values(const atp_config_t *cfg, report...)
```

并：

```text
NULL → invalid argument
```

---

# 50. `validate_service_params()` 也应 const

当前只是读。

---

# 51. API host 需要语法校验吗

当前 `API_HOST` 没任何验证。

需要先决定允许：

```text
IPv4
IPv6
hostname
UDS future?
```

---

# 52. 不要错误地只用 `inet_pton` 限制

因为如果合法支持 hostname：

```text
localhost
```

也应该允许。

---

# 53. 推荐当前策略

如果 Native API 只允许：

```text
loopback TCP
```

那更严格：

```text
127.0.0.1
::1
localhost
```

甚至最好 endpoint固定为 UDS。

---

# 54. 如果允许任意 host

至少：

```text
non-empty
length-safe
no control chars
```

并把 resolve/connect失败留给 transport prepare。

---

# 55. API secret需要长度与控制字符策略

当前 field：

```text
128 bytes
```

但超长会截断。

必须：

```text
reject
```

同时不要把 secret完整打印在 validation error/log。

---

# 56. Service args/env 更需要明确语义

当前：

```text
char args[256]
char env[256]
```

后续 service parser如果把它们当 command-line/env语法：

validator至少应检查：

```text
长度
NUL/control chars
```

更完整的 quoting语义由 service parser own。

---

# 57. Path validation

需要检查：

```text
DATA_DIR
RUN_DIR
PID_FILE
LOG_FILE
```

基础 candidate validation：

```text
non-empty
length < PATH_MAX
no embedded newline/control
```

---

# 58. 是否要求 absolute path

当前 defaults：

```text
RUN_DIR = "run"
PID_FILE = "run/atpd.pid"
```

所以当前设计支持相对路径。

但前面 main/utils review已经指出：

```text
daemon不应依赖 cwd
```

---

# 59. 长期更推荐 config commit时 canonicalize 成 absolute path

然后 validator要求：

```text
runtime effective paths absolute
```

但不要在纯 validator里调用：

```text
realpath
```

因为 path可能尚不存在。

---

# 60. 可以分两层

```text
raw config syntax
→ string valid

prepare
→ resolve against data_dir/install root
→ effective absolute paths

effective config validation
→ path sizes/relationships
```

---

# 61. Cross-field validation 当前几乎没有

例如 service：

```text
grace_period <= stop_timeout
```

是不是应该满足？

通常合理。

---

# 62. 推荐至少检查

```text
grace_period_sec <= stop_timeout_sec
```

否则：

```text
grace=30
stop_timeout=5
```

语义矛盾。

---

# 63. Circuit 参数关系也要明确

例如：

```text
circuit_threshold <= max_failures?
```

取决于 service state machine设计。

如果 threshold就是 failure count的一种边界：

应检查关系。

---

# 64. 不要盲目加关系规则

需要以 `service.c` 当前语义为准。

Codex应做 callsite/field semantics audit再定。

---

# 65. Health interval 与 timeout关系

例如：

```text
health_check_interval_ms
```

如果比 startup/stop timeout大，不一定非法。

只是 operational policy。

不必过度限制。

---

# 66. Port 1–65535 当前合理

但端口 1–1023 对 non-root不适用。

ATPD root运行，无问题。

---

# 67. API port 0 是否允许 auto/dynamic

当前不允许。

如果产品不需要 dynamic port：

保持。

---

# 68. `RESTART_DELAY` 0–3600 合理

但 config int parser要在写入前就严格。

---

# 69. Service timeout 1–3600

合理。

这些具体 bounds可以保留。

---

# 70. Error message要带 line number

当前 value validator只知道：

```text
cfg field
```

不知道来源行。

所以错误：

```text
API_PORT must be...
```

没有：

```text
line 17
```

---

# 71. 推荐 parser阶段记录 source metadata

最简单：

```text
parse错误直接带 line number
```

对 cross-field error：

```text
key name
```

已足够。

不一定把每个 cfg field都存 source line。

---

# 72. Unknown key最好提供 suggestion

文件里已经定义：

```text
MAX_SUGGESTION_KEY
MAX_KEY_LEN
```

但没有任何 suggestion实现。

---

# 73. 这是 dead/incomplete design

要么：

```text
删除这些 macro
```

要么真正做 typo suggestion。

---

# 74. 是否值得做 suggestion

可以有，但不是 P0。

例如简单 Levenshtein：

```text
SERVICE_START_TIMOUT
→ did you mean SERVICE_START_TIMEOUT?
```

非常有用。

---

# 75. 但不要自己写复杂 fuzzy matcher作为首要任务

第一阶段：

```text
Unknown key 'X'
```

已经足够。

---

# 76. 如果实现 suggestion

key只有几十个。

简单 edit-distance helper即可。

测试要固定 threshold，避免乱建议。

---

# 77. Duplicate key 当前未检测

例如：

```text
API_PORT=9080
API_PORT=9090
```

当前：

```text
后一个覆盖前一个
```

并且没有 warning。

---

# 78. 对 daemon config 推荐 reject duplicate

重复 key通常表示：

```text
配置合并错误
旧值没删
```

last-wins容易产生误解。

---

# 79. Alias + canonical 也算 duplicate/conflict

例如：

```text
API_SECRET=a
CLASH_SECRET=b
```

应该：

```text
conflicting aliases
```

而不是取后一个。

---

# 80. 推荐 parser维护 seen bitset

key registry分配：

```text
key id
```

然后：

```text
seen[id]
```

发现第二次：

```text
duplicate error
```

---

# 81. 注释/空行处理当前基本正常

```text
# full-line comment
```

但不支持：

```text
KEY=value # inline comment
```

---

# 82. 不要隐式加入 inline comment除非定义 escaping

因为 secret/service args可能合法包含：

```text
#
```

当前 simple grammar更好。

---

# 83. Quoting处理当前比较宽松

如果 value：

```text
"abc"
```

首尾 quote一致才剥掉。

如果：

```text
"abc
```

则保留 quote并继续。

---

# 84. 未闭合 quote应该是 parse error

不要把：

```text
"abc
```

当普通字符串。

---

# 85. 空 quoted string

```text
KEY=""
```

应该得到：

```text
empty string
```

这没问题。

---

# 86. Quote escaping 当前不支持

例如：

```text
SERVICE_ARGS="--x=\"a\""
```

没有 escape parser。

---

# 87. 不要半支持 shell quoting

要么：

```text
明确简单 value grammar
```

要么以后用 JSON/TOML/YAML。

当前建议：

```text
保持简单：outer quote only，不支持 escapes
```

并文档明确。

---

# 88. 1024-byte line truncation属于 config loader问题，但 validator必须参与结果

当前：

```c
char line[1024];
fgets(...)
```

超长一行会被拆成两段。

这是前面 config plan已经发现的。

---

# 89. 修复后 parser应在 validator/report中输出

```text
line too long
```

而不是：

```text
后半段变成另一个 malformed line
```

---

# 90. 没有 `=` 的 non-comment line当前直接忽略

例如：

```text
API_PORT 9080
```

当前：

```text
continue
```

---

# 91. Strict config下必须是 syntax error

否则用户 typo仍然 silent。

---

# 92. 空 key

```text
=foo
```

应该 error。

---

# 93. 空 value是否允许取决于 key

例如：

```text
API_SECRET=
SERVICE_ARGS=
```

可能合法。

但：

```text
API_HOST=
DATA_DIR=
```

通常不合法。

---

# 94. 需要 per-key empty policy

schema可以加：

```text
allow_empty
```

---

# 95. Unknown/deprecated key不能只在 final struct阶段发现

因为一旦写 struct：

```text
原始 key alias/source line
```

都丢了。

所以 key-level validation必须在 parsing时。

---

# 96. 推荐 architecture

```text
read line
↓
syntax parse
↓
key lookup
↓
duplicate/deprecated check
↓
typed value parse
↓
write candidate
↓
cross-field validation
↓
candidate accepted
```

---

# 97. Validator 与 loader不应再完全分开

不是说要合成一个文件。

而是 API要形成：

```text
parser uses validator/schema
```

而不是：

```text
parser写完
validator事后随便检查几个字段
```

---

# 98. `config_validate_values()` 最终应接受 const candidate

例如：

```c
int config_validate_candidate(
    const atp_config_t *cfg,
    config_validation_report_t *report);
```

---

# 99. Parse-level接口可由 config.c调用

例如：

```c
const config_key_spec_t *config_schema_find(const char *key);
```

是否放 validator.c里可以。

---

# 100. 不建议引入 reflection/codegen

C里几十个 key。

一个 static schema table足够。

---

# 101. 与 transactional reload联动

最终：

```text
candidate parse + validation失败
→ runtime完全不动
```

这是 reload transaction第一道 gate。

---

# 102. Validator绝不能修改 active config

只处理：

```text
temporary candidate
```

---

# 103. 与 `config_sync_from_singbox_json()` 的顺序必须重新定义

当前：

```text
load file
validate
then sync sing-box JSON
```

即：

```text
从 JSON sync出来的新 API host/port/secret
```

没有再跑 validator。

---

# 104. 这是另一个明确 validation hole

当前：

```text
config_prepare()
= load_file + validate_values

然后：
config_sync_from_singbox_json(&tmp)
```

所以 sync后的：

```text
API port
host
secret
```

可能绕过 candidate validation。

---

# 105. 例如 sing-box JSON 中

```text
listen_port = 99999
```

当前：

```text
p > 0
```

就可能写入：

```text
cfg->api.port = 99999
```

之后没有再 validate_port。

---

# 106. 这是 P1 correctness bug

推荐顺序：

```text
defaults
↓
parse ATP config
↓
merge external sing-box source
↓
normalize/canonicalize
↓
validate final candidate
```

---

# 107. 但更长期应该减少“多源隐式 merge”

前面 config plan已经指出：

```text
根据 default equality猜用户是否显式配置
```

并不可靠。

presence metadata解决后：

```text
merge rule明确
```

---

# 108. Validator应只看到最终 candidate

这样：

```text
无论值来自 atp.conf
sing-box JSON
CLI override
default
```

最终都经过一次相同 validation。

---

# 109. `config_sync_from_singbox_json()` 还使用 `atoi`

这是 loader/merge问题。

应改严格 parse。

但最终 candidate validation仍是最后安全网。

---

# 110. 与 eBPF removal联动

删除：

```text
ENABLE_EBPF
ebpf.enabled
ebpf.ready
```

validator schema与 config struct一起收敛。

---

# 111. 与 logger方案联动

validator不再：

```text
LOG_ERROR
```

caller决定如何输出 report。

---

# 112. 与 CLI `check` 联动

`atpd check` 应输出：

```text
line 12: unknown key SERVICE_START_TIMOUT
line 18: API_PORT must be 1..65535
line 21: duplicate VPN_TARGET_MODE
```

然后：

```text
non-zero exit
```

---

# 113. 与 status/reload联动

reload失败时：

```text
status:
last_reload = failed
reason = validation
error_count = N
```

但 active runtime继续旧 config。

---

# 114. Secret handling

validation report绝不能直接 echo：

```text
API_SECRET value
SERVICE_ENV secrets
```

错误只显示：

```text
key name
reason
```

不显示敏感 value。

---

# 115. Test：unknown key

```text
API_P0RT=9080
```

必须：

```text
fail
```

---

# 116. Test：invalid integer

```text
API_PORT=abc
SERVICE_START_TIMEOUT=30x
```

必须：

```text
fail
```

不能 silent default。

---

# 117. Test：strict bool

```text
VPN_AUTO_MODE=2
UI_EMOJI_ENABLED=-1
```

必须 fail。

---

# 118. Test：string overflow

分别测试：

```text
API_HOST
API_SECRET
DATA_DIR
RUN_DIR
PID_FILE
LOG_FILE
SERVICE_ARGS
SERVICE_ENV
VPN modes
CORE_USER_GROUP
```

长度 == capacity：

```text
fail
```

---

# 119. Test：duplicate key

```text
API_PORT=9080
API_PORT=9090
```

fail。

---

# 120. Test：canonical + alias conflict

```text
API_SECRET=a
CLASH_SECRET=b
```

fail。

---

# 121. Test：deprecated alias

单独：

```text
CLASH_SECRET=a
```

如果兼容：

```text
success + deprecation warning
```

---

# 122. Test：malformed line

```text
API_PORT 9080
=foo
```

fail。

---

# 123. Test：unclosed quote

```text
API_HOST="127.0.0.1
```

fail。

---

# 124. Test：line >1024

必须：

```text
single clear line-too-long error
```

---

# 125. Test：external merge validation

sing-box JSON：

```text
listen_port = 99999
```

最终 candidate：

```text
fail
```

---

# 126. Test：cross field

如果 service semantics确认：

```text
grace_period > stop_timeout
```

fail。

---

# 127. Test：NULL

```text
config_validate_candidate(NULL,...)
```

不能 crash。

---

# 128. Test：no logging side effects

调用 validator：

```text
不写 logger
```

只返回 report。

---

# 129. Test：deterministic report

同一 input：

```text
错误顺序稳定
message稳定
```

这样 CLI golden test更容易。

---

# 130. Fuzz target

最值得 fuzz：

```text
config line parser
key lookup
quoted value parser
integer/bool parser
```

---

# 131. ASan/UBSan

对：

```text
long line
long key
long value
malformed quote
random bytes
```

不能：

```text
OOB
overflow
hang
```

---

# 132. 推荐 Commit 1

```text
config: reject unknown and malformed keys during parsing
```

先修 strict mode实际上没接入的问题。

---

# 133. Commit 2

```text
config: make parsing key-first and type-strict
```

修：

```text
API_PORT=abc silent default
bool arbitrary nonzero
```

---

# 134. Commit 3

```text
config: reject truncated strings and duplicate keys
```

---

# 135. Commit 4

```text
config: replace global strict mode with explicit schema policy
```

或者 production直接永远 strict。

---

# 136. Commit 5

```text
config: return structured validation reports instead of logging
```

删除 validator → logger dependency。

---

# 137. Commit 6

```text
config: validate final merged candidate
```

顺序：

```text
parse
merge
normalize
validate
```

---

# 138. Commit 7

```text
config: canonicalize deprecated key aliases
```

---

# 139. Commit 8

```text
config: remove obsolete eBPF configuration keys
```

随 eBPF removal。

---

# 140. Commit 9

```text
config: add parser fuzz and malformed-input regression tests
```

---

# 141. 不建议把 validator拆成多个文件

最终可能：

```text
config_validator.c 200–350 LOC
```

仍然很合理。

如果 schema table明显变大，

也最多：

```text
config_schema.c
```

但当前没有必要先拆。

---

# 142. 推荐最终 API

例如：

```c
typedef struct {
    config_validation_error_t items[32];
    size_t count;
    bool truncated;
} config_validation_report_t;

int config_validate_candidate(
    const atp_config_t *cfg,
    config_validation_report_t *report);
```

parse-level schema helper：

```c
const config_key_spec_t *
config_schema_find(const char *key);
```

---

# 143. 如果所有 parser/schema都在 config.c

validator可以更纯：

```text
只做 cross-field/effective candidate validation
```

也可以。

核心要求不是文件位置，而是：

```text
single key schema
strict typed parse
final candidate validation
```

---

# 144. 最终 Invariants

Codex最终必须保证：

```text
I1:
Every non-comment configuration line is either parsed successfully or produces an explicit error.

I2:
Unknown configuration keys are never silently ignored.

I3:
Expected value type is determined by the key, not guessed from the value.

I4:
Invalid integers/booleans never silently fall back to defaults.

I5:
Fixed-size string fields are never silently truncated.

I6:
Duplicate canonical keys and alias conflicts are detected.

I7:
Validator has no logger/runtime/environment side effects.

I8:
The final merged candidate is validated after all config sources are applied.

I9:
Validation errors never expose secrets.

I10:
Obsolete ATPD-owned eBPF keys are removed.

I11:
Transactional reload never mutates active runtime before candidate validation succeeds.

I12:
There is one authoritative key schema, not separate drifting parser/validator lists.
```

---

# 145. 最终验收标准

## Unknown key

```text
typo
→ fail
```

## Invalid type

```text
API_PORT=abc
→ fail
```

## Boolean

```text
VPN_AUTO_MODE=2
→ fail
```

## Strings

```text
oversized value
→ fail, no truncation
```

## Duplicate

```text
same key twice
→ fail
```

## Alias

```text
legacy key
→ explicit deprecated handling
```

## Merge

```text
invalid sing-box-derived value
→ final candidate fail
```

## Purity

```text
validator does not LOG
does not access filesystem/users/network
```

## Reload

```text
invalid candidate
→ active config/runtime untouched
```

---

# 146. 最终结论

`config_validator.c` 当前看起来很简单，但它现在还没有真正承担“配置安全门”的角色。

最关键的两个确定问题是：

```text
1. config_validate_key() 根本没有接入 config_load_file()
2. parse_key_value() 先根据 value 猜类型，导致 API_PORT=abc 这类错误被静默忽略
```

再加上：

```text
string静默截断
global strict mode
validator直接写日志
merge后不重新 validation
```

当前 config candidate 还不能被视为“验证完成”。

最终应该形成：

```text
line
→ syntax
→ key schema
→ strict typed parse
→ candidate
→ merge/normalize
→ final validation
→ transaction prepare/commit
```

做到这一点之后，前面设计的 transactional reload 才真正有可靠基础。
