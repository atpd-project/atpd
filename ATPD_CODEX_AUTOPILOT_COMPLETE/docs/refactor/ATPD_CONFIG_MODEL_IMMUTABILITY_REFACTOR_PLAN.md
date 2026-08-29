# ATPD `atp_config.h / config.h` 配置模型纯化与不可变快照方案

## 1. 结论

当前：

```text
include/atp_config.h   65 lines
include/config.h       25 lines
```

`atp_config_t` 目前混合了四类完全不同的数据：

```text
1. 持久配置
2. CLI / 启动选项
3. runtime observed state
4. synchronization primitive
```

这使得 config candidate 不是一个真正的“纯值对象”，进而让：

```text
load
validate
copy
reload
snapshot
rollback
```

全部变复杂。

本轮建议的核心不是拆文件，而是：

> 把 `atp_config_t` 纯化成一个可复制、可比较、无 mutex、无 runtime readiness 的 immutable configuration value。

---

# 2. 当前 `core_config_t` 混入 CLI 状态

当前：

```c
typedef struct {
    bool foreground;
    bool verbose;
    bool no_color;
    bool ui_emoji_enabled;
    bool dry_run;
    bool log_timestamp;
    int restart_delay;
    ...
} core_config_t;
```

其中：

```text
foreground
verbose
no_color
dry_run
```

都不是 daemon 持久配置本体。

---

# 3. `foreground`

它是：

```text
进程启动方式
```

属于：

```text
CLI run mode / launcher policy
```

不是 config。

最终应由：

```text
cli_options_t
```

进入：

```text
main/startup options
```

而不是 `atp_config_t`。

---

# 4. `verbose`

它是：

```text
CLI logging intent
```

前面 CLI/logger 方案已经要求：

```text
cli_verbosity_t
→ startup maps to logger level
```

所以：

```text
core.verbose
```

应删除。

---

# 5. `no_color`

它是：

```text
presentation preference
```

前面 UI 方案已经确定：

```text
ui_render_ctx.color
```

而不是 config global。

所以：

```text
core.no_color
```

应删除。

---

# 6. `dry_run`

需要 callsite audit。

如果只用于：

```text
check/test CLI
```

也应属于：

```text
command execution mode
```

不是 active config。

---

# 7. 如果 dry_run 有 daemon config语义

例如：

```text
daemon启动但不 apply runtime
```

那应重新定义清楚。

当前更像旧 CLI/test 状态。

优先 grep caller。

---

# 8. `ui_emoji_enabled`

这个字段可以有两种合理 owner。

### A：保留在 persisted user preferences

如果用户通过 atp.conf 配置：

```text
UI_EMOJI_ENABLED
```

可以继续存在。

### B：移到 CLI/UI preference

如果 daemon不应该决定 client presentation：

长期更合理。

---

# 9. 推荐阶段性处理

第一阶段：

```text
保留 ui_emoji_enabled
```

因为已有 config key。

但 UI renderer：

```text
只接受 copy-out render option
```

不直接读 global config。

后面再决定是否彻底移出 daemon config。

---

# 10. `log_timestamp`

同理。

它是 logger policy，

可以作为 persisted config存在。

但 config只保存：

```text
desired logging policy
```

实际 logger state由 logger owner维护。

---

# 11. `restart_delay`

属于 service supervisor policy。

它现在放在：

```text
core
```

语义不准确。

---

# 12. 推荐移动到 `service_config_t`

例如：

```c
int restart_delay_sec;
```

与：

```text
start_timeout
stop_timeout
failure threshold
circuit cooldown
```

在一起。

---

# 13. `core_config_t` 应只剩真正 core/path identity

例如：

```text
data_dir
run_dir
core_user
core_group
pid_file
log_file
```

甚至：

```text
pid_file/log_file
```

也可进一步由 run_dir派生。

---

# 14. `interface_config_t.current_vpn_iface` 明确不是 config

当前：

```c
typedef struct {
    char current_vpn_iface[IFNAMSIZ];
    bool vpn_auto_mode;
    char vpn_target_mode[64];
    char vpn_fallback_mode[64];
} interface_config_t;
```

其中：

```text
current_vpn_iface
```

是：

```text
当前观测到的系统 VPN interface
```

它会随 runtime变化。

---

# 15. 这属于 observed state

应该进入：

```text
netlink/vpn snapshot
```

或：

```text
status runtime snapshot
```

绝不能保存在 config。

---

# 16. 为什么这很重要

如果 reload candidate：

```text
config_set_defaults()
```

会把：

```text
current_vpn_iface = ""
```

然后 commit config，

就可能把 runtime observation清掉。

---

# 17. 更根本：

```text
configuration
```

应该回答：

```text
如果检测到 VPN，该采取什么策略？
```

而不是：

```text
现在 VPN interface 是谁？
```

---

# 18. 推荐重命名 `interface_config_t`

这个 struct实际主要是 VPN policy。

可以改为：

```c
typedef struct {
    bool auto_mode;
    char target_mode[64];
    char fallback_mode[64];
} vpn_policy_config_t;
```

---

# 19. 这样 owner更清楚

runtime：

```text
vpn_observation_t
```

config：

```text
vpn_policy_config_t
```

API reconcile：

```text
policy + observation
→ desired mode
```

---

# 20. `ebpf_config_t` 应整体删除

当前：

```c
typedef struct {
    bool enabled;
    bool ready;
} ebpf_config_t;
```

两字段都不应该继续存在。

---

# 21. `enabled`

如果 dataplane是：

```text
sing-box ebpf-in
```

是否启用应由：

```text
sing-box config
```

决定。

ATPD不再维护第二份：

```text
ENABLE_EBPF
```

---

# 22. `ready`

更明显是 runtime readiness。

它属于：

```text
sing-box Native API / service health snapshot
```

不是 config。

---

# 23. 所以删除：

```text
ebpf_config_t
atp_config_t.ebpf
ENABLE_EBPF
cfg_ebpf_*
config_apply_deltas里的 ebpf_probe()
```

---

# 24. 当前 `config.c` 仍然：

```c
cfg->ebpf.enabled = 1;
cfg->ebpf.ready = 1;
```

这会制造一个完全虚假的状态：

```text
默认配置一加载
→ ready = true
```

即使 sing-box根本没启动。

---

# 25. 这是 config/runtime state混淆的直接后果

`ready` 必须由：

```text
真实 runtime observation
```

产生。

---

# 26. `service_config_t`

当前：

```c
typedef struct {
    int start_timeout_sec;
    int stop_timeout_sec;
    int grace_period_sec;
    int max_failures;
    int circuit_threshold;
    int circuit_cooldown_sec;
    int health_check_interval_ms;
    char args[256];
    char env[256];
} service_config_t;
```

这部分基本属于真正 config。

---

# 27. 但 `args[256]` / `env[256]` 太 opaque

两个字段只是：

```text
未解析字符串
```

后续 service owner再解释。

这会让：

```text
quoting
escaping
multiple args
env syntax
secret handling
```

全部延后。

---

# 28. 第一阶段不用重构成 vector

为了 C 简洁和兼容，

可以先保留。

但必须：

```text
strict length validation
明确 grammar
不通过 shell执行
```

---

# 29. `SERVICE_ENV` 风险更高

如果格式是：

```text
A=1 B=2
```

还是：

```text
A=1;B=2
```

必须明确。

---

# 30. service supervisor最好最终接受结构化 env

例如：

```c
service_env_entry_t env[MAX...];
```

但这不是当前 release blocker。

---

# 31. `api_config_t`

当前：

```c
typedef struct {
    int port;
    char host[64];
    char secret[128];
} api_config_t;
```

本身是合理配置。

---

# 32. 但 API transport owner已在变化

前面的 API plan建议：

```text
endpoint + auth
只由 singbox_api transport owner维护
```

因此 config可以保留 desired endpoint，

但 runtime API ctx copy后：

```text
不再反复读 global config。
```

---

# 33. `secret` copy要避免到处散播

因为整个：

```text
atp_config_t
```

当前被 snapshot/copy。

意味着：

```text
secret
```

跟着整个 config对象到处复制。

---

# 34. 这是可接受的 C value semantics，但要限制 snapshot用途

不要把完整 config：

```text
dump status
serialize logs
generic debug print
```

否则 secret容易泄漏。

---

# 35. 推荐显式 redaction contract

任何：

```text
config debug/status serializer
```

都默认：

```text
secret = <redacted>
```

---

# 36. 最大结构问题：`pthread_mutex_t mutex` 放在 config value里

当前：

```c
typedef struct {
    ...
    pthread_mutex_t mutex;
} atp_config_t;
```

这是最值得删除的字段之一。

---

# 37. 为什么不合理

一个配置对象本来应该：

```text
可 memset
可 copy
可 compare
可放 stack
可作为 candidate
```

一旦包含 mutex：

```text
不能普通 memcpy
必须 init/destroy
生命周期复杂
snapshot必须特殊 copy
```

---

# 38. 当前代码已经因此产生专门 helper

```c
config_copy_content()
config_snapshot_content()
```

它们本质只是：

```text
复制所有字段，但故意不复制 mutex
```

---

# 39. 这是明显的 design smell

如果 config没有 mutex：

```c
*dst = *src;
```

就够了。

---

# 40. 现在每个 temporary config都必须：

```text
config_set_defaults()
→ pthread_mutex_init

failure
→ pthread_mutex_destroy
```

---

# 41. reload代码因此不断出现

```c
pthread_mutex_destroy(&new_config.mutex);
```

这些 lifecycle完全不是 configuration 本身需要的。

---

# 42. 锁应该保护“config store / active pointer”

而不是保护每一个 config value。

---

# 43. 推荐结构

例如：

```c
typedef struct {
    pthread_mutex_t mutex;
    atp_config_t active;
    uint64_t generation;
} config_store_t;
```

---

# 44. 更好：reactor单线程下甚至不需要 config mutex

如果：

```text
所有 config commit只在 reactor/main owner线程
```

active config可以：

```text
owner-thread only
```

其他线程只读 copy-out snapshot。

---

# 45. 如果仍有跨线程读取

使用：

```text
store lock
→ copy active config
→ unlock
```

而不是：

```text
每个 config带 mutex。
```

---

# 46. 最终 `atp_config_t` 应成为纯 POD-ish value

包含：

```text
bool
int
fixed char arrays
nested config structs
```

没有：

```text
mutex
FILE*
fd
pid
runtime state
pointers
```

---

# 47. 这对 transactional reload非常重要

流程才能真正变成：

```text
atp_config_t candidate;
config_defaults(&candidate);
parse(&candidate);
merge(&candidate);
validate(&candidate);
prepare(candidate);

lock store;
old = active;
active = candidate;
generation++;
unlock;
```

---

# 48. candidate failure时

只是：

```text
丢弃 stack object
```

无需：

```text
pthread_mutex_destroy
cleanup internal runtime state
```

---

# 49. 当前 `config_snapshot_t` 命名有误导性

`config.h`：

```c
typedef struct {
    int has_backup;
    char backup_path[PATH_MAX];
    uint64_t version;
    uint64_t load_time;
} config_snapshot_t;
```

这其实不是：

```text
configuration snapshot
```

---

# 50. 它只是 reload/backup metadata

而且当前：

```text
snapshot_update()
```

还被标记：

```c
__attribute__((unused))
```

---

# 51. 所以 `config_snapshot_t` 很可能是 dead legacy

它不包含：

```text
atp_config_t
```

却叫 snapshot。

容易和前面的：

```text
immutable config snapshot
```

概念冲突。

---

# 52. 推荐 callsite audit

如果：

```text
config_get_snapshot()
```

没有实际 consumer：

删除整套：

```text
g_snapshot
g_snapshot_mutex
snapshot_update
snapshot_get
config_snapshot_t
config_get_snapshot
```

---

# 53. 如果确实需要 reload metadata

重命名：

```c
config_reload_meta_t
```

字段例如：

```text
generation
last_commit_time
last_reload_result
```

但更适合放：

```text
daemon/status lifecycle state
```

---

# 54. `backup_path / has_backup`

当前 config rollback：

```c
int config_rollback(...) {
    return ATP_OK;
}
```

是空实现。

所以这套 backup metadata基本也是半成品。

---

# 55. 前面 config transaction方案已经要求

不要做：

```text
假 rollback API
```

真正 rollback是：

```text
prepare失败不 commit
apply失败恢复 old generation
```

不是靠：

```text
backup_path
```

文件级 rollback。

---

# 56. 所以建议直接删除 fake snapshot/rollback API

包括：

```text
config_reload_atomic()
```

目前也只是：

```c
return config_reload(cfg);
```

---

# 57. Fake API比没有 API更危险

看到：

```text
atomic
rollback
snapshot
```

维护者会以为：

```text
已有 transaction guarantee
```

实际上没有。

---

# 58. `config_set_mode()`

当前：

```c
(void)cfg;
(void)mode;
return ATP_OK;
```

也是 fake API。

---

# 59. 如果没有真实 caller

直接删除。

如果 caller依赖：

应该由：

```text
VPN/API desired mode owner
```

实现，而不是 config setter no-op。

---

# 60. Public `config_load_file()`

`config.h` 注释：

```text
Load config file without mutex
```

这是旧 mutex-centric API暴露。

---

# 61. Config parser API不应该对 caller说：

```text
with mutex / without mutex
```

mutex应该是 store implementation detail。

---

# 62. 推荐 API语义

```text
config_parse_file(path, candidate, report)
```

就是纯解析 candidate。

---

# 63. Active store API

```text
config_store_commit(...)
config_store_snapshot(...)
```

如果真的需要。

---

# 64. `config_load()` 当前职责过重

它：

```text
defaults
parse
validate
sync sing-box JSON
lock active cfg
copy
log
```

---

# 65. 这与 transactional方案冲突

应该拆成阶段函数：

```text
config_build_candidate()
config_validate_candidate()
config_commit()
```

不是文件拆分，而是职责拆分。

---

# 66. `atp_config_t` 纯化以后这些阶段会简单很多

---

# 67. Defaults 也不应访问环境

当前：

```c
get_app_dir(...)
getgrnam("net_admin")
```

发生在：

```text
config_set_defaults()
```

---

# 68. 这意味着 defaults不是纯函数

同一个 binary：

```text
不同 filesystem / group db
```

可能得到不同 default config。

---

# 69. 前面 utils/config方案已经指出

`get_app_dir()` 失败 fallback "." 很危险。

这里更进一步：

```text
raw defaults
```

和：

```text
environment resolution
```

应分开。

---

# 70. 推荐：

```text
config_init_defaults()
→ fixed deterministic defaults

config_resolve_environment()
→ app dir / user/group / platform paths

config_validate_effective()
```

---

# 71. Android/Linux default group差异

当前 Linux：

```text
如果 net_admin group存在 → net_admin
否则 root
```

这属于：

```text
platform resolution
```

不是 struct declaration问题。

但 pure config model应该允许把这个步骤明确分开。

---

# 72. Config generation / source presence metadata

当前 sync sing-box JSON使用：

```text
cfg->api.port == DEFAULT_API_PORT
```

判断用户是否显式配置。

---

# 73. 这是错误的 presence inference

用户完全可能显式写：

```text
API_PORT=9080
```

但系统会误认为：

```text
没有显式设置
```

然后 sing-box JSON覆盖它。

---

# 74. Config model需要 presence metadata

可以在 parsing candidate旁边维护：

```c
config_presence_t
```

例如 bitset：

```text
API_PORT seen
API_HOST seen
API_SECRET seen
...
```

---

# 75. 不一定放进最终 `atp_config_t`

推荐：

```text
parse/build context
```

持有 presence。

最终 committed config只保存 effective values。

---

# 76. 这样 merge规则：

```text
if !presence.api_port
    use sing-box derived port
```

不会再用：

```text
value == default
```

猜。

---

# 77. Config generation

active store应该有：

```text
uint64_t generation
```

但它也不应放进：

```text
atp_config_t
```

因为 generation是：

```text
store/runtime metadata
```

不是 config value。

---

# 78. 推荐：

```c
typedef struct {
    atp_config_t value;
    uint64_t generation;
} config_snapshot_view_t;
```

用于 copy-out。

---

# 79. 注意这里的 snapshot才是真正 config snapshot

与当前：

```text
backup_path snapshot
```

完全不同。

---

# 80. Immutability contract

一旦 candidate commit：

```text
active generation N
```

owner不能在原地零散修改字段。

---

# 81. 当前 API如：

```text
config_set_mode()
```

就是潜在反例。

应该：

```text
新的 desired policy
→ new candidate / owner-specific state
```

而不是：

```text
修改 active config字段
```

---

# 82. 为什么 immutable config重要

否则 status/API/service分别读：

```text
不同时间点的字段
```

可能看到：

```text
一半 old
一半 new
```

---

# 83. 一次 snapshot copy可以保证跨字段一致

特别是：

```text
service config
api endpoint
vpn policy
paths
```

需要同 generation。

---

# 84. Secret比较

如果 reload candidate新 secret：

```text
generation变化
```

API owner收到 delta并重新配置 transport。

不要让 API callback直接：

```text
g_config.api.secret
```

---

# 85. 与 atpd_global deletion联动

最终：

```text
g_config
```

本身应该消失。

---

# 86. config owner/store可能仍是 daemon context成员

但不能通过：

```text
global macro alias
```

任意访问。

---

# 87. 与 context方案联动

`atpd_context` 不要重新变成：

```text
god object holding mutable config fields
```

只持：

```text
config store handle / current snapshot
```

---

# 88. 与 service方案联动

service在配置 commit时收到：

```text
service_config_t copy
```

之后内部拥有自己的 desired policy。

运行中不反复读全局 config。

---

# 89. 与 API方案联动

同理：

```text
api_config_t + vpn_policy_config_t
```

commit/reconcile时 copy。

---

# 90. 与 logger方案联动

logger收到：

```text
log policy
log file
```

delta。

不要每条 log读取：

```text
g_config
```

---

# 91. 与 UI方案联动

UI收到：

```text
emoji preference
```

local copy。

---

# 92. 与 main/CLI方案联动

这些字段从 config删除：

```text
foreground
verbose
no_color
dry_run
```

main拥有：

```text
cli_options_t
startup_options_t
```

---

# 93. 推荐最终 config struct示意

例如：

```c
typedef struct {
    bool log_timestamp;
    bool ui_emoji_enabled;
    char data_dir[PATH_MAX];
    char run_dir[PATH_MAX];
    char core_user[64];
    char core_group[64];
    char pid_file[PATH_MAX];
    char log_file[PATH_MAX];
} core_config_t;

typedef struct {
    bool auto_mode;
    char target_mode[64];
    char fallback_mode[64];
} vpn_policy_config_t;

typedef struct {
    int restart_delay_sec;
    int start_timeout_sec;
    int stop_timeout_sec;
    int grace_period_sec;
    int max_failures;
    int circuit_threshold;
    int circuit_cooldown_sec;
    int health_check_interval_ms;
    char args[256];
    char env[256];
} service_config_t;

typedef struct {
    int port;
    char host[64];
    char secret[128];
} api_config_t;

typedef struct {
    core_config_t core;
    vpn_policy_config_t vpn;
    service_config_t service;
    api_config_t api;
} atp_config_t;
```

---

# 94. 这只是第一阶段目标

后续还可以继续：

```text
UI preference移出
pid/log path派生
service args/env结构化
API UDS endpoint typed
```

但不需要一次完成。

---

# 95. 最重要的纯值 invariant

最终应该可以合法：

```c
atp_config_t a;
atp_config_t b;

b = a;
```

不需要：

```text
special copy
mutex init
mutex destroy
```

---

# 96. 这会直接删除很多 config.c 辅助复杂度

可删除：

```text
config_copy_content()
config_snapshot_content()
```

---

# 97. `memcmp` 能不能比较 config

不推荐依赖：

```text
memcmp struct
```

因为 padding可能不稳定。

---

# 98. Delta应该按子结构/字段语义比较

例如：

```text
service_config_equal()
api_config_equal()
vpn_policy_equal()
```

或者显式 strcmp/int compare。

---

# 99. 但 copy可以直接 assignment

这是关键区别。

---

# 100. Mutex owner

如果仍需要跨线程 config access：

推荐：

```c
typedef struct {
    pthread_rwlock_t lock;
    atp_config_t active;
    uint64_t generation;
} config_store_t;
```

---

# 101. Mutex vs rwlock

不必过度优化。

普通：

```text
pthread_mutex_t
```

完全够。

因为 commit很少，snapshot copy很快。

---

# 102. 更推荐 reactor ownership时无锁

如果 architecture确认所有访问都可以：

```text
reactor owner thread
```

那更简单。

但这要全仓 callsite audit后决定。

---

# 103. 不要把 atomic pointer/RCU搞进来

ATPD规模不需要。

---

# 104. Config store API示意

```c
int config_store_snapshot(
    config_store_t *store,
    atp_config_t *out,
    uint64_t *generation);

int config_store_commit(
    config_store_t *store,
    const atp_config_t *candidate,
    uint64_t *new_generation);
```

---

# 105. 但 transaction apply顺序更复杂

最终更可能由：

```text
reload coordinator
```

先：

```text
prepare subsystem deltas
```

再：

```text
publish generation
```

---

# 106. 所以 store本身保持简单。

---

# 107. Fake config APIs清理

建议删除/替换：

```text
config_set_mode()
config_reload_atomic()
config_rollback()
config_get_snapshot()  // 若只对应 dead backup metadata
```

---

# 108. 为什么现在是好时机

因为这些名字会让 Codex后续重构误以为：

```text
已有 atomic/rollback contract
```

然后在错误基础上继续实现。

---

# 109. Public API命名

建议：

```text
config_parse_file()
config_build_candidate()
config_validate_candidate()
config_commit()
```

比：

```text
config_load()
config_reload_atomic()
```

语义更明确。

---

# 110. Tests：config POD/value semantics

验证：

```text
stack candidate无需 mutex init/destroy
simple assignment copy完整
```

---

# 111. Test：CLI fields removed

grep：

```text
foreground
verbose
no_color
dry_run
```

不再出现在：

```text
atp_config_t
```

---

# 112. Test：runtime fields removed

grep：

```text
current_vpn_iface
ebpf.ready
```

不再出现在 config headers。

---

# 113. Test：eBPF config removed

```text
ebpf_config_t
ENABLE_EBPF
cfg->ebpf
```

全部消失。

---

# 114. Test：mutex removed

```bash
grep -R "pthread_mutex_t.*mutex" include/atp_config.h
```

目标：

```text
0
```

---

# 115. Test：candidate lifecycle

构建/验证失败：

```text
直接离开 scope
```

没有：

```text
pthread_mutex_destroy(&candidate.mutex)
```

---

# 116. Test：presence semantics

用户显式：

```text
API_PORT=9080
```

即使它等于 default：

```text
sing-box JSON不能覆盖
```

---

# 117. Test：runtime observation survives config reload

当前 VPN iface：

```text
tun0
```

reload config。

结果：

```text
VPN observation仍然 tun0
```

因为它不再存 config。

---

# 118. Test：dataplane readiness

config load：

```text
绝不能让 eBPF/sing-box status变 READY
```

readiness只来自 runtime snapshot。

---

# 119. Test：service restart delay owner

修改：

```text
RESTART_DELAY
```

只产生：

```text
service policy delta
```

不是 core generic mutation。

---

# 120. Test：secret redaction

任何：

```text
status
config diagnostic
reload diff
```

不得打印 secret。

---

# 121. Test：fake APIs gone

```text
config_reload_atomic
config_rollback
config_set_mode
```

若无真实语义：

grep应为 0。

---

# 122. Test：header dependency

删除：

```text
pthread.h
```

后 `atp_config.h` 仍独立编译。

---

# 123. 这也是一个很好的 header cleanup结果

最终 `atp_config.h` 只需要：

```text
stdbool.h
limits.h
```

可能连：

```text
net/if.h
```

都因 current_vpn_iface删除而不再需要。

---

# 124. 所以 header dependency会明显下降

当前：

```text
pthread.h
net/if.h
```

都可能一起消失。

---

# 125. 推荐 Commit 1

```text
config: remove runtime eBPF state from config model
```

---

# 126. Commit 2

```text
config: move observed VPN interface out of configuration
```

---

# 127. Commit 3

```text
config: move CLI/startup options out of atp_config_t
```

：

```text
foreground
verbose
no_color
dry_run
```

---

# 128. Commit 4

```text
config: move restart policy into service config
```

---

# 129. Commit 5

```text
config: remove mutex from configuration values
```

建立 owner/store lock。

---

# 130. Commit 6

```text
config: add explicit presence metadata for source merging
```

修 default-equality inference。

---

# 131. Commit 7

```text
config: remove fake snapshot/atomic/rollback APIs
```

---

# 132. Commit 8

```text
config: publish immutable generation snapshots to subsystem owners
```

---

# 133. 不建议把 `atp_config.h` 继续做大

不要加入：

```text
runtime status
generation
last error
service state
API readiness
```

这些都应该在 owner snapshots。

---

# 134. 也不要把 CLI options搬进另一个 `core_config_v2`

直接回 owner。

---

# 135. Config是 desired state，不是 actual state

这是最终最重要的 architecture rule：

```text
config
→ desired policy

runtime snapshot
→ actual observed state
```

---

# 136. 例如 VPN

```text
config:
auto_mode=true
target_mode=Google VPN
fallback_mode=Rule

runtime:
current_vpn_iface=ipsec0
current_mode=...
```

---

# 137. 例如 service

```text
config:
timeout/restart policy

runtime:
pid
generation
state
failure count
next restart
```

---

# 138. 例如 API

```text
config:
host/port/secret

runtime:
connected
last snapshot
last failure
generation
```

---

# 139. 例如 eBPF

```text
config:
不在 ATPD

runtime:
sing-box dataplane readiness snapshot
```

---

# 140. 最终 Invariants

Codex最终必须保证：

```text
I1:
atp_config_t contains desired configuration only.

I2:
CLI/startup presentation flags are not stored in atp_config_t.

I3:
Observed VPN/interface state is not stored in configuration.

I4:
ATPD-owned eBPF enabled/ready state is absent from configuration.

I5:
atp_config_t contains no mutex or other lifecycle-bearing resource.

I6:
A configuration candidate can be stack-created, copied by assignment, and discarded without cleanup.

I7:
Synchronization protects the active config store, not every config value.

I8:
Explicit source-presence metadata determines merge precedence; equality with a default value never means “unset”.

I9:
Config generation metadata is separate from the config value.

I10:
Subsystems consume committed config copies/deltas instead of repeatedly reading a global mutable config.

I11:
Fake atomic/rollback/snapshot APIs do not remain.

I12:
Configuration represents desired state; runtime snapshots represent actual state.
```

---

# 141. 最终验收标准

## Pure config value

```text
sizeof/config copy
→ no mutex lifecycle
```

## CLI separation

```text
foreground/verbose/no_color/dry_run
→ outside atp_config_t
```

## Runtime separation

```text
current_vpn_iface
ebpf.ready
→ outside atp_config_t
```

## eBPF

```text
ebpf_config_t
cfg->ebpf
ENABLE_EBPF
→ removed
```

## Reload

```text
candidate failure
→ active/runtime untouched
```

## Merge

```text
explicit API_PORT equal to default
→ still treated as explicit
```

## Snapshot

```text
one committed generation
→ consistent cross-field copy
```

## Header

```text
atp_config.h
→ no pthread dependency
→ likely no net/if dependency
```

---

# 142. 最终结论

`atp_config.h` 当前最大的设计问题不是字段命名，而是：

```text
configuration value
```

被迫同时承担：

```text
CLI state
runtime observation
dataplane readiness
locking/lifecycle
```

其中最明显的例子就是：

```text
current_vpn_iface
ebpf.ready
pthread_mutex_t mutex
foreground/no_color/verbose
```

这些都不属于 committed configuration。

最终推荐把 `atp_config_t` 做成一个真正的纯值对象：

```text
defaults
→ parse
→ merge
→ validate
→ prepare
→ immutable commit generation
```

配置只描述 **desired state**；service/API/netlink/status/context 分别维护 **actual state**。

这一步完成后，前面设计的 transactional reload、global state deletion、status snapshot、API/service owner化会明显更容易落地。
