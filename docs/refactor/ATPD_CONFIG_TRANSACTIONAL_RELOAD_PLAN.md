# ATPD `config.c` 配置事务与 Reload 一致性加固方案

## 1. 模块结论

当前 `config.c` 约 443 行，`config_validator.c` 约 144 行。

整体职责分层方向是对的：

```text
config.c
    defaults
    file load
    sing-box JSON sync
    runtime reload
    runtime serialization

config_validator.c
    value validation
```

目前不建议拆 `config.c`。

真正需要修复的是：

> 当前 reload 在 API 名字上叫 atomic，但运行时语义并不 atomic。

核心问题：

```text
new config parse/validate
        ↓
先 copy 到 live cfg
        ↓
再 apply runtime deltas
        ↓
apply 如果失败
        ↓
live cfg 已经是新值
runtime 却可能仍然是旧状态
```

此外：

```text
config_reload_atomic()
```

当前只是：

```c
return config_reload(cfg);
```

而：

```text
config_rollback()
```

当前直接返回成功但没有任何行为。

因此必须建立真正的：

```text
prepare
validate
plan
apply
commit
rollback/fail
```

事务模型。

---

# 2. 当前最重要的问题：配置“声明状态”和“运行状态”可能分叉

当前 `config_reload()` 的流程大致是：

```text
load new config
validate
sync sing-box JSON
snapshot old config
copy new config → live cfg
apply deltas
return result
```

如果：

```text
config_apply_deltas()
```

失败：

```text
cfg = new config
runtime = old/partial state
```

这是长期 daemon 中非常危险的状态。

例如未来 reload 修改：

```text
eBPF enable
service timeout
API host/port
VPN mode
run dir
service args
```

如果 runtime apply 中途失败，status 读取的 config 和系统实际状态会不一致。

---

# 3. P0/P1：`config_reload_atomic()` 名字必须真实

有两种选择。

## 方案 A：实现真正 atomic reload

推荐。

```text
config_reload_atomic()
```

必须保证：

```text
成功：
config + runtime 都是 new

失败：
config + runtime 都保持 old
```

---

## 方案 B：如果暂时不实现事务

那就删除/改名：

```text
config_reload_atomic
config_rollback
```

避免假 API。

但 ATPD 已经是长期 root daemon，推荐直接走方案 A。

---

# 4. 推荐事务阶段

完整 reload：

```text
LOAD
  ↓
VALIDATE
  ↓
NORMALIZE
  ↓
DIFF
  ↓
PREPARE
  ↓
APPLY
  ↓
COMMIT
```

失败：

```text
PREPARE/APPLY FAIL
  ↓
ROLLBACK PREPARED CHANGES
  ↓
KEEP OLD CONFIG
```

---

# 5. 关键原则：不要先修改 live cfg

当前：

```c
config_copy_content(cfg, &new_config);
config_apply_deltas(cfg, &old_config);
```

顺序应该反过来。

推荐：

```text
old = snapshot(live)
new = parsed candidate

build apply plan(old,new)

apply plan using candidate
        ↓
all succeeds
        ↓
commit live cfg = new
```

也就是说：

> runtime apply 应使用 candidate config，而不是先污染 live config。

---

# 6. Candidate config

建议引入明确语义：

```c
typedef struct {
    atp_config_t old_cfg;
    atp_config_t new_cfg;

    config_delta_t delta;

    bool prepared;
    bool applied;
} config_transaction_t;
```

不一定必须公开。

可以完全内部使用。

---

# 7. Config diff

不要未来把所有 runtime apply 都塞成：

```c
config_apply_deltas(cfg, old)
```

然后自己比较。

建议显式：

```c
typedef struct {
    bool api_changed;
    bool service_changed;
    bool ebpf_changed;
    bool interface_changed;
    bool core_paths_changed;
    bool logging_changed;
} config_delta_t;
```

---

# 8. 更细粒度 diff

未来可继续细化：

```text
API:
host
port
secret

SERVICE:
timeouts
args
env
user/group

EBPF:
enabled

INTERFACE:
vpn_auto_mode
target_mode
fallback_mode

CORE:
data_dir
run_dir
pid_file
log_file
```

这样可以定义每一类 reload policy。

---

# 9. 每个字段必须明确 reload policy

不是所有配置都应该支持 hot reload。

建议分类：

```text
HOT
RESTART_REQUIRED
IMMUTABLE_AT_RUNTIME
```

例如：

### HOT

可能包括：

```text
service health interval
VPN target/fallback mode
API secret（视实现）
```

### RESTART_REQUIRED

可能包括：

```text
service args
service env
service user/group
API bind host/port
```

### IMMUTABLE

可能包括：

```text
run_dir
pid_file
```

具体以当前 ATPD architecture 决定。

---

# 10. 不要假装所有字段都能热加载

当前 loader 可以改变很多字段。

但 runtime apply 目前只有：

```c
if (cfg->ebpf.enabled) {
    ebpf_probe();
}
```

这意味着绝大多数 reload field 实际：

```text
内存配置变了
runtime 没应用
```

这是比 atomic 命名更广泛的一致性问题。

---

# 11. 配置字段必须有 apply coverage

Codex修改前先建立表格：

```text
field
default
validator
source
runtime owner
reload policy
apply function
rollback function
```

例如：

```text
API_PORT
→ singbox_api
→ restart/reconnect API transport

SERVICE_START_TIMEOUT
→ service supervisor
→ hot apply

ENABLE_EBPF
→ ebpf
→ enable/disable semantics

VPN_TARGET_MODE
→ interface/controller
→ hot apply
```

---

# 12. `config_apply_deltas()` 不应该只 probe eBPF

当前：

```c
if (cfg->ebpf.enabled)
    ebpf_probe();
```

并没有真正代表 runtime configuration apply。

建议拆成协调器：

```c
static int config_apply_transaction(
    const atp_config_t *old_cfg,
    const atp_config_t *new_cfg,
    const config_delta_t *delta,
    config_apply_log_t *log);
```

内部调用各 owner：

```text
service_apply_config
singbox_api_apply_config
ebpf_apply_config
runtime/interface apply
logger apply
```

---

# 13. Ownership 原则

`config.c` 不应直接了解各模块内部实现细节。

例如：

```text
service config
→ service_apply_config()

API config
→ singbox_api_apply_config()

eBPF config
→ ebpf_apply_config()
```

配置模块负责：

```text
parse
validate
diff
transaction ordering
```

具体应用由 owner module负责。

---

# 14. Apply 顺序很重要

例如：

```text
API config
service config
eBPF config
VPN policy
```

需要明确依赖。

建议原则：

> 先 prepare 不破坏旧状态的新资源，再切换，最后清理旧资源。

例如 API endpoint：

```text
prepare new API manager/connection
↓
如果准备成功
commit switch
↓
close old
```

不要：

```text
先 close old
再尝试 new
```

---

# 15. Rollback 不一定意味着“反向调用所有 API”

最理想是：

```text
prepare → commit
```

而不是：

```text
apply old
apply new
失败
再 apply old
```

后者复杂且容易失败。

因此推荐优先：

> two-phase prepare/commit。

---

# 16. 两阶段应用模型

每个复杂模块可以提供：

```c
int module_prepare_config(
    const module_config_t *new_cfg,
    module_config_plan_t *plan);

int module_commit_config(
    module_config_plan_t *plan);

void module_abort_config(
    module_config_plan_t *plan);
```

第一阶段：

```text
allocate
validate runtime capability
open required resource
```

不破坏旧状态。

全部 prepare 成功后：

```text
commit
```

---

# 17. 对简单字段不需要过度工程

例如：

```text
health_check_interval_ms
```

可以直接：

```text
atomic/struct update
reschedule timer
```

失败语义简单。

不要为了所有 int 配置都建立庞大 plan object。

事务设计应分复杂度。

---

# 18. `config_rollback()` 当前是假实现

现在：

```c
int config_rollback(atp_config_t *cfg) {
    (void)cfg;
    return ATP_OK;
}
```

这必须处理。

推荐：

### 如果实现真正 transaction

删除这个 public API。

rollback 应是 reload transaction内部机制。

因为：

```text
“回滚到哪个版本？”
```

外部 API 当前没有参数，语义不完整。

---

# 19. Snapshot 当前基本未使用

`g_snapshot` 包含：

```text
has_backup
backup_path
version
load_time
```

但：

```text
snapshot_update()
```

还是 unused。

说明 snapshot/backup design 当前没有完成。

建议：

> 不要继续维持一个看起来支持 backup rollback、实际没使用的 subsystem。

---

# 20. Snapshot 建议重新定义

如果需要配置 observability：

```c
typedef struct {
    uint64_t generation;
    uint64_t loaded_at_ms;

    bool last_reload_ok;

    int last_error;

    char source_path[PATH_MAX];
} config_status_t;
```

比当前：

```text
backup_path/has_backup
```

更符合实际需要。

---

# 21. 如果产品真的需要文件版本 rollback

那另做完整设计：

```text
config generation
backup file
checksum
atomic write
restore
runtime apply
```

不要和 runtime reload transaction混为一谈。

---

# 22. P1：`config_sync_from_singbox_json()` 的来源优先级必须显式

当前逻辑：

```text
ATPD config defaults/file
↓
如果某字段仍等于 default
↓
从 sing-box config.json推导 API
```

这是一种隐式优先级规则：

```text
explicit ATPD config > sing-box JSON > default
```

这个方向合理。

但要明确写成文档和测试。

---

# 23. 不要用“等于 default value”判断用户是否显式配置

当前例如：

```c
if (p > 0 && cfg->api.port == DEFAULT_API_PORT)
    cfg->api.port = p;
```

问题：

如果用户明确写：

```text
API_PORT=9090
```

而：

```text
DEFAULT_API_PORT == 9090
```

代码无法区分：

```text
用户明确指定 9090
```

和：

```text
未配置，恰好默认 9090
```

---

# 24. 增加 explicit-set metadata

推荐 parse 时保存：

```c
typedef struct {
    bool api_host;
    bool api_port;
    bool api_secret;
    bool data_dir;
    ...
} config_presence_t;
```

或 bitset：

```c
uint64_t present_mask;
```

然后 sync：

```text
if !explicit(API_PORT)
→ allow sing-box JSON override
```

而不是比较数值。

---

# 25. 这是配置系统常见且重要的 correctness 问题

否则未来默认值变化时：

```text
source precedence
```

会表现不稳定。

应建立：

```text
default
< derived
< explicit config
< CLI override（若存在）
```

明确 precedence。

---

# 26. `config_sync_from_singbox_json()` listen parsing 较脆弱

当前使用：

```c
strrchr(listen_str, ':');
```

解析 host:port。

IPv6：

```text
[::1]:9090
::1
```

会比较麻烦。

如果项目当前只支持 IPv4：

明确 validator：

```text
IPv4 only
```

并拒绝其他格式。

如果要 IPv6：

使用专门 endpoint parser。

---

# 27. `atoi()` 不应继续用于 port

当前 sing-box JSON listen解析：

```c
int port = atoi(colon + 1);
```

应统一使用：

```text
strict integer parser
```

并验证：

```text
1..65535
```

避免：

```text
"9090foo" → 9090
```

---

# 28. `yyjson_get_int()` 也要检查 port range

当前：

```text
p > 0
```

还应：

```text
p <= 65535
```

最好复用：

```c
validate_port(p)
```

---

# 29. Secret precedence

当前：

```text
ATPD explicit secret
```

应优先于 sing-box JSON secret。

这个方向是合理的。

但：

```text
status/log/save runtime
```

必须永远 redact。

---

# 30. `parse_key_value()` 当前对未知 key 静默忽略

这在兼容性上有好处，但容易 typo。

例如：

```text
SERVCE_START_TIMEOUT=5
```

会被悄悄忽略。

建议：

```text
unknown key → WARN
```

而不是失败。

---

# 31. 可增加 strict mode

未来：

```text
config validate --strict
```

未知 key：

```text
ERROR
```

daemon普通启动：

```text
WARN
```

兼顾兼容和排错。

---

# 32. 行长度 1024 的语义

当前：

```c
char line[1024];
fgets(...)
```

如果配置行 >1023：

会被拆成两行读取。

可能产生错误解析。

必须检测：

```text
line 未包含 newline 且未 EOF
→ line too long
→ fail
```

不要默默解析半行。

---

# 33. SERVICE_ENV / SERVICE_ARGS 很容易超过简单配置需求

当前一行最大约 1024。

如果允许：

```text
复杂 env/args
```

需要明确长度限制并 validator。

不要因为 buffer 截断导致部分配置被接受。

---

# 34. Quoted value parser 非 shell parser

当前仅支持：

```text
"abc"
'abc'
```

去掉首尾 quote。

不支持：

```text
escape
embedded quote
multiline
```

这没问题，只需文档明确。

不要让用户误以为支持 shell syntax。

---

# 35. `=` inside value 当前支持

因为：

```text
strchr(line,'=')
```

只切第一个。

所以：

```text
SERVICE_ENV=TOKEN=a=b
```

value仍为：

```text
TOKEN=a=b
```

这是好的。

---

# 36. Comments inside quoted value

当前只有行首：

```text
#
```

识别 comment。

不会把：

```text
VALUE=abc#def
```

截断。

这是简单且可预测的语义，可以保留。

---

# 37. Config mutex 生命周期

`config_set_defaults()`：

```c
pthread_mutex_init(&cfg->mutex, NULL);
```

这意味着调用方必须保证：

```text
同一个 cfg 只初始化 mutex 一次
```

Codex应审计所有：

```text
config_set_defaults(cfg)
```

是否可能对已初始化 live cfg再次调用而未 destroy。

---

# 38. Snapshot/copy 不复制 mutex是正确的

当前：

```text
config_copy_content
config_snapshot_content
```

只复制子结构，不 memcpy 整个：

```text
pthread_mutex_t
```

这个方向正确。

必须保留。

---

# 39. `config_load()` 对 destination mutex有前置要求

当前：

```c
pthread_mutex_lock(&cfg->mutex);
```

说明 caller必须先：

```text
config_set_defaults(cfg)
```

或其他方式初始化 mutex。

这个 API contract应写进 header。

否则：

```text
uninitialized mutex
```

是 UB。

---

# 40. 更好的 API

长期可以：

```c
int config_init(atp_config_t *cfg);
int config_load_into(...);
void config_destroy(...);
```

明确生命周期。

比：

```text
config_set_defaults
```

同时承担初始化 mutex更清晰。

---

# 41. `config_set_mode()` 当前也是 no-op success

当前：

```c
int config_set_mode(...) {
    return ATP_OK;
}
```

没有实际行为。

这和前一个 `singbox_api_exec_cli()` 类似：

> 未实现函数不应该假成功。

Codex应全仓检查调用点。

---

# 42. `config_set_mode()` 处理

如果已废弃：

```text
删除
```

如果未实现：

```text
ATP_ERR_NOTSUP
```

如果未来需要：

真正操作：

```text
interface target/fallback/current mode
```

不要保持 no-op success。

---

# 43. P1：`ebpf_probe()` return value

当前：

```c
if (cfg->ebpf.enabled) {
    ebpf_probe();
}
return ATP_OK;
```

如果：

```text
ebpf_probe()失败
```

reload仍成功。

Codex必须检查 `ebpf_probe()` signature/return。

如果有返回值：

必须处理。

---

# 44. Probe 与 Apply 语义也要区分

```text
probe
```

通常意味着：

```text
检查能力
```

不等于：

```text
apply configuration
```

所以 `config_apply_deltas()` 调 `ebpf_probe()` 本身语义可疑。

未来应由：

```text
ebpf_apply_config()
```

负责。

---

# 45. Disable eBPF 的 reload

当前：

```text
if enabled → probe
```

如果从：

```text
enabled=1
→ enabled=0
```

什么都不做。

说明 disable semantics当前没有应用。

必须明确：

```text
hot-disable supported?
requires restart?
unsupported?
```

不能只修改 cfg flag。

---

# 46. Service config reload coverage

当前 config 包含：

```text
start_timeout
stop_timeout
grace_period
max_failures
circuit threshold
cooldown
health interval
args
env
```

这些应该明确分成：

```text
hot mutable
next-start only
restart-required
```

---

# 47. API config reload coverage

```text
host
port
secret
```

改变时 Native API client必须：

```text
close/reconnect
```

否则：

```text
cfg显示 new API
transport仍连接 old API
```

---

# 48. RUN_DIR / PID_FILE / LOG_FILE 热修改风险很高

这类路径一旦 daemon已经启动：

```text
pidfile
UDS
logger
runtime file
```

都可能已经绑定旧路径。

推荐默认：

```text
restart-required
```

甚至：

```text
immutable after startup
```

不要热切换。

---

# 49. CORE_USER_GROUP 同样建议 restart-required

已经运行的 daemon/child用户身份不能简单热修改。

未来只影响：

```text
next child spawn
```

也必须明确语义。

---

# 50. Config validation应尽量纯函数

`config_validator.c` 当前相对独立，方向不错。

保持：

```text
validate values
no runtime side effects
```

不要让 validator：

```text
open socket
modify eBPF
signal service
```

---

# 51. Cross-field validation

仅验证单字段不够。

需要检查：

```text
start_timeout > 0
stop_timeout > 0
health interval sensible
circuit threshold <= max failures?（按语义）
target/fallback mode valid
path relationships
```

具体以现有 validator覆盖为准。

---

# 52. Runtime capability validation 与 value validation 分离

例如：

```text
user exists
group exists
directory writable
eBPF feature supported
API endpoint reachable
```

这些不是 pure config value validation。

应该放：

```text
prepare phase
```

而不是 parser/validator。

---

# 53. `config_save_runtime()` 已经比普通写法安全

当前有：

```text
write tmp
fflush
fsync(file)
fclose
rename
```

这是好的基础。

但还差一个严格 durability步骤。

---

# 54. Atomic rename 后目录 fsync

如果文件 crash durability很重要：

```text
rename(tmp, path)
```

之后应：

```text
fsync(parent directory)
```

确保 directory entry持久化。

对于 runtime status file未必必须。

如果这个文件用于 crash recovery，建议补。

---

# 55. 临时文件权限

当前：

```c
fopen(tmp_path,"w")
```

权限受 daemon umask影响。

如果内容包含敏感配置：

应显式：

```text
0600
```

使用：

```text
open(O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC,0600)
fdopen
```

当前 runtime save只有 eBPF状态，风险较低，但形成安全模式更好。

---

# 56. Symlink/path safety

Root daemon写配置/runtime文件时：

```text
tmp path
target path
```

应审计是否可能由不可信用户控制。

必要时：

```text
O_NOFOLLOW
lstat
root-owned parent
```

不要对任意 symlink目标写文件。

---

# 57. Runtime file key names不一致

当前保存：

```text
PROXY_MODE=4
EBPF_ENABLED
EBPF_READY
```

而 parser读取：

```text
ENABLE_EBPF
```

需要确认这个 runtime文件是否会被 config parser读回。

如果不会：

可以保留独立格式。

如果会：

这是 schema mismatch。

---

# 58. Config source path

`config_reload()`通过：

```text
cfg->core.data_dir + ATP_CONF_FILE
```

重新计算路径。

如果初次加载使用了：

```text
custom path
```

reload可能不是同一个文件。

建议 live config manager记录：

```text
source_path
```

reload永远从原 source reload，除非显式改变。

---

# 59. 这是实际使用上的重要一致性问题

`config_load(path, cfg)` 明确接受：

```text
任意 path
```

但：

```text
config_reload(cfg)
```

没有保存这个 path。

因此：

> load(path) 与 reload() 的 source identity 并不天然一致。

建议修正。

---

# 60. Config generation

每次成功 commit：

```text
generation++
```

status：

```text
Config generation: 12
Last reload: success
```

这样其他 subsystem也可以记录：

```text
applied_generation
```

---

# 61. 模块 applied generation

长期很有价值：

```text
config generation = 12
service applied = 12
api applied = 12
ebpf applied = 12
```

如果：

```text
某模块 = 11
```

立即知道 runtime divergence。

---

# 62. Reload result应包含详细原因

不要只：

```text
ATP_OK / ATP_ERR_*
```

内部保存：

```text
stage
module
errno
message
```

例如：

```text
Reload failed
Stage: PREPARE
Module: singbox_api
Reason: invalid API host
```

---

# 63. Reload status snapshot

建议：

```c
typedef struct {
    uint64_t generation;
    uint64_t last_reload_ms;

    bool last_reload_ok;

    char source_path[PATH_MAX];

    char failed_stage[32];
    char failed_module[32];

    int last_error;
} config_runtime_status_t;
```

---

# 64. Secret 永远不进入 status snapshot

只显示：

```text
secret configured: yes/no
```

不要输出值。

---

# 65. Test：atomic apply failure

构造：

```text
old config A
new config B
module prepare/apply fail
```

验证：

```text
live cfg == A
runtime == A
generation unchanged
```

这是最核心测试。

---

# 66. Test：successful reload

验证：

```text
live cfg == B
runtime == B
generation +1
```

---

# 67. Test：partial module prepare failure

例如：

```text
service prepare success
api prepare fail
```

验证：

```text
service prepared resource abort
old runtime unaffected
```

---

# 68. Test：commit ordering

如果模块间有依赖：

模拟 failure。

确保没有：

```text
部分 commit
```

或者明确哪些简单字段 commit不可失败。

---

# 69. Test：explicit default value precedence

假设：

```text
DEFAULT_API_PORT=9090
```

ATPD config显式：

```text
API_PORT=9090
```

sing-box JSON：

```text
port=10000
```

预期：

```text
最终仍 9090
```

这要求 presence metadata。

---

# 70. Test：derived value when unspecified

ATPD没设置 API_PORT。

sing-box JSON：

```text
10000
```

预期：

```text
10000
```

---

# 71. Test：unknown key typo

```text
SERVCE_START_TIMEOUT=5
```

预期至少：

```text
WARN unknown key
```

strict mode：

```text
fail validation
```

---

# 72. Test：overlong config line

发送：

```text
>1023 bytes
```

必须：

```text
fail with line-too-long
```

不能拆成多个伪配置行。

---

# 73. Test：invalid integer

```text
API_PORT=9090foo
SERVICE_START_TIMEOUT=1x
```

必须 reject/ignore并报告。

不能 silently转换。

---

# 74. Test：port boundary

```text
0
-1
65535
65536
```

---

# 75. Test：sing-box JSON invalid port

例如：

```text
"listen": "127.0.0.1:9090foo"
```

不能通过 `atoi`变成9090。

---

# 76. Test：custom load path + reload

```text
config_load("/tmp/custom.conf")
```

之后：

```text
config_reload()
```

必须重新读取：

```text
/tmp/custom.conf
```

而不是 data_dir默认文件。

---

# 77. Test：config_set_mode no-op

当前未实现时：

```text
不能返回 ATP_OK
```

如果删除 API：

更新调用点。

---

# 78. Test：eBPF enable/disable transition

分别：

```text
0 → 1
1 → 0
```

验证：

```text
实际 runtime与 config一致
```

如果不支持 hot transition：

reload必须明确返回：

```text
restart required / unsupported
```

---

# 79. Test：service restart-required field

改变：

```text
SERVICE_ARGS
SERVICE_ENV
CORE_USER_GROUP
```

行为必须明确：

```text
reject live reload
or
mark restart required
or
controlled service restart
```

不要只是改内存。

---

# 80. Test：API endpoint change

```text
host/port A
→ host/port B
```

成功后：

```text
singbox_api transport使用B
```

失败：

```text
仍使用A
live config仍A
```

---

# 81. Test：reload storm

连续：

```text
1000 reload
```

包括：

```text
valid
invalid
valid
```

检查：

```text
mutex
FD
RSS
module resource
generation
```

稳定。

---

# 82. Test：concurrent status + reload

如果 status读取 config snapshot：

```text
reload过程中
```

只能看到：

```text
old完整配置
or
new完整配置
```

不能看到半更新字段组合。

---

# 83. Test：concurrent reload

最好明确：

```text
only one reload transaction at a time
```

用：

```text
reload mutex
```

第二个：

```text
serialize
or
EBUSY
```

不要两个 reload交错 apply。

---

# 84. 当前 cfg mutex不等于 reload mutex

`cfg->mutex` 只保护：

```text
config struct copy
```

不能保护：

```text
多模块 runtime transaction
```

所以需要独立：

```text
config manager/reload lock
```

---

# 85. Sanitizers

```text
ASan
UBSan
```

主要检查：

```text
parser
line handling
temporary configs
rollback resource
```

---

# 86. 推荐 Commit 1

```text
config: remove false atomic and no-op APIs
```

内容：

- `config_set_mode` 不再假成功
- `config_rollback` 删除或明确 unsupported
- 暂时避免 misleading API

如果直接进入真正 transaction，也可以保留 `reload_atomic`作为最终 API。

---

# 87. Commit 2

```text
config: track source and explicit field presence
```

内容：

- source_path
- presence mask
- precedence tests
- strict port parsing

---

# 88. Commit 3

```text
config: build typed reload delta
```

内容：

- config_delta_t
- field reload policy
- unknown key warning
- line length detection

---

# 89. Commit 4

```text
config: prepare runtime changes before commit
```

内容：

- candidate config
- module prepare
- abort on failure
- old config remains live

---

# 90. Commit 5

```text
config: commit reload atomically
```

内容：

- serialized reload
- module commit
- live cfg copy only after runtime ready
- generation increment

---

# 91. Commit 6

```text
config: expose reload health and generation
```

内容：

- last success/failure
- stage/module error
- source path
- generation
- no secret output

---

# 92. Commit 7

```text
config: harden runtime file persistence
```

可选：

- 0600 temp file
- O_NOFOLLOW
- directory fsync
- schema clarification

---

# 93. Codex 修改前必须建立字段矩阵

这是本模块最重要的前置工作。

对所有 config字段列：

```text
name
type
default
source
validator
explicit-presence bit
owner module
reload policy
prepare
commit
rollback/abort
status visibility
secret?
```

如果没有这个矩阵：

> 不要直接开始写 generic reload transaction。

---

# 94. Codex 必须全仓搜索

```text
config_reload
config_reload_atomic
config_rollback
config_set_mode
config_load
config_set_defaults

cfg->core
cfg->interface
cfg->ebpf
cfg->service
cfg->api

service_apply_config
ebpf_probe
singbox_api_init
API_HOST
API_PORT
SERVICE_ARGS
RUN_DIR
PID_FILE
LOG_FILE
```

目标：

> 找出每个字段真正的 runtime consumer。

---

# 95. 与 `service.c` 方案的关系

service refactor完成后最好提供：

```c
int service_prepare_config(...);
int service_apply_config(...);
```

或者对简单 hot字段继续：

```text
service_apply_config
```

但必须返回真实失败。

---

# 96. 与 `singbox_api.c` 方案的关系

API config改变：

```text
host
port
secret
```

应让：

```text
singbox_api manager
```

prepare/reconnect。

不能只改：

```text
cfg->api
```

---

# 97. 与 `ebpf.c` 的关系

本模块不要直接通过：

```text
ebpf_probe
```

假装已经应用 eBPF状态。

需要 `ebpf.c` 明确：

```text
enable
disable
prepare
apply
status
```

reload才能正确协调。

---

# 98. 与 status snapshot的关系

status显示：

```text
Config generation
Source
Last reload
Reload state
Restart required
```

但不直接读/打印 sensitive raw config。

---

# 99. 是否需要拆 `config.c`

当前：

```text
443 lines
```

完全不需要因为长度拆。

如果未来事务逻辑增长明显，可以拆：

```text
config.c
config_reload.c
config_internal.h
```

其中：

```text
config.c
    defaults
    load
    source merge

config_reload.c
    diff
    transaction
    generation/status
```

但第一轮不需要。

---

# 100. `config_validator.c` 建议继续独立

这个模块目前边界很好。

长期：

```text
parser
↓
normalized config
↓
pure validation
↓
runtime prepare
```

不要把 runtime side effect塞入 validator。

---

# 101. 最终 Invariants

Codex最终必须通过源码和测试保证：

```text
I1:
reload success
→ live config == runtime applied config

I2:
reload failure
→ live config remains old

I3:
only one reload transaction runs at a time

I4:
explicit config always beats derived sing-box config

I5:
user explicitly choosing a default-valued setting is still explicit

I6:
unknown/overlong/malformed input never silently changes unrelated fields

I7:
unsupported runtime change never reports success

I8:
config generation increments only after successful commit

I9:
secret is never emitted in logs/status

I10:
config mutex objects are never copied or double-initialized
```

---

# 102. 最终验收标准

## Atomicity

```text
apply failure
→ old config + old runtime preserved
```

## Reload coverage

```text
每个字段都有明确 reload policy
```

## Source precedence

```text
explicit > derived > default
```

## Source identity

```text
custom loaded path reloads same path
```

## Parser

```text
overlong line / invalid int / bad port
→ deterministic error
```

## Concurrency

```text
status during reload
→ only complete old/new snapshot
```

## Stress

```text
1000 reload cycles
FD/RSS stable
generation correct
```

## No fake success

```text
config_set_mode
config_rollback
unsupported hot field
```

均不能无操作返回成功。

---

# 103. 最终结论

`config.c` 当前真正的问题不是配置文件解析，而是：

> “配置对象已经改变”被当成了“整个 daemon 的运行配置已经成功改变”。

对 ATPD 这种 root 网络控制 daemon，这两个概念必须严格分开。

正确目标应该是：

```text
candidate config
        ↓
validate
        ↓
diff + reload policy
        ↓
prepare runtime
        ↓
commit runtime
        ↓
commit config generation
```

任何阶段失败：

```text
old config
+
old runtime
```

都保持有效。

完成这套事务语义后，ATPD 的 reload 才能真正称为：

> atomic runtime configuration reload。
