# ATPD `service.c` 重构方案

## 1. 重构目标

当前 `service.c` 已经承担了太多职责：

- 对外 service API
- service 状态机
- sing-box fork/exec
- PID 管理
- `/proc` 进程校验
- SIGCHLD / waitpid
- stop / kill
- restart / backoff
- circuit breaker
- startup readiness
- health check
- timer 管理
- user/group 降权
- argv / env 构造
- pid file
- log rotation

这导致：

- 文件超过 1000 行
- child ownership 与状态机逻辑分散
- process、supervisor、health 相互混在一起
- 修改一个生命周期逻辑容易影响其他路径
- 单元测试难以针对单一职责
- 后续继续增加功能会使 `service.c` 继续膨胀

本次重构目标不是重写 supervisor，而是：

> 保留现有 C 版 service 行为和 separate-process 架构，将 `service.c` 拆成职责明确的几个模块，使 `service.c` 本体只负责 service API 与状态机。

---

# 2. 最终文件结构

建议拆成：

```text
src/
├── service.c
├── service_process.c
├── service_supervisor.c
├── service_health.c
└── service_log.c            # 可选，第二阶段再拆

include/
├── service.h
└── service_internal.h
```

第一轮推荐先只拆：

```text
service.c
service_process.c
service_supervisor.c
service_health.c
service_internal.h
```

`service_log.c` 暂时可不拆。

---

# 3. 模块职责

## 3.1 `service.c`

### 定位

`service.c` 是 service subsystem 的：

> Public API + State Machine Coordinator

它只负责：

- 对外接口
- service 状态
- desired state
- 状态转换
- 启动/停止/重启流程编排
- 调用 process / supervisor / health 子模块
- 暴露只读 status snapshot

### 应保留的函数

例如：

```c
int service_init(service_ctx_t *ctx, ...);

int service_start(service_ctx_t *ctx);

int service_stop(service_ctx_t *ctx);

int service_restart(service_ctx_t *ctx);

void service_cleanup(service_ctx_t *ctx);

service_state_t service_get_state(
    const service_ctx_t *ctx
);

int service_get_status(
    const service_ctx_t *ctx,
    service_status_t *out
);
```

内部：

```c
void service_set_state(
    service_ctx_t *ctx,
    service_state_t next,
    service_reason_t reason
);

void service_mark_running(
    service_ctx_t *ctx
);

void service_mark_failed(
    service_ctx_t *ctx,
    service_reason_t reason,
    int err
);
```

### 不应该再包含

`service.c` 中不再直接出现：

```text
fork
execv
waitpid
setuid
setgid
readlink
/proc
socket connect
poll
kill(SIGKILL)
argv tokenizer
pidfile low-level I/O
```

### 目标长度

```text
250–400 行
```

---

# 4. `service_process.c`

## 定位

负责：

> sing-box process creation and process identity

只处理“进程本身”。

### 职责

- fork
- exec
- setsid
- user/group
- chdir
- argv 构造
- environment
- stdout/stderr 准备
- PID file
- process identity
- `/proc/<pid>/exe`
- `/proc/<pid>/stat`
- child signal helper
- exec error reporting

### 建议接口

```c
int service_process_spawn(
    service_ctx_t *ctx,
    service_child_t *out
);

int service_process_send_signal(
    const service_child_t *child,
    int sig
);

bool service_process_is_same(
    const service_child_t *child
);

int service_process_write_pidfile(
    const service_child_t *child
);

void service_process_remove_pidfile(
    service_ctx_t *ctx
);
```

### Child Object

建议新增：

```c
typedef struct {
    pid_t pid;

    uint64_t generation;

    unsigned long long starttime;

    bool owned;
    bool exec_confirmed;
} service_child_t;
```

后续如需要可增加：

```c
dev_t exe_dev;
ino_t exe_ino;
```

### 核心原则

`service_process.c`：

- 不决定是否 restart
- 不决定 state 应变成什么
- 不管理 backoff
- 不管理 health policy

它只回答：

```text
进程是否成功创建？
进程是谁？
能否安全发送 signal？
process identity 是否仍然匹配？
```

---

# 5. `service_supervisor.c`

## 定位

负责：

> Child lifecycle and recovery mechanism

这是拆分后最重要的模块。

### 职责

- SIGCHLD
- waitpid
- reap
- SIGTERM
- SIGKILL
- stop timeout
- kill timeout
- restart timer
- backoff
- circuit breaker
- child generation
- retry scheduling
- unexpected exit handling
- shutdown 时禁止 restart

### 建议接口

```c
int service_supervisor_init(
    service_ctx_t *ctx
);

void service_supervisor_cleanup(
    service_ctx_t *ctx
);

int service_supervisor_request_stop(
    service_ctx_t *ctx
);

int service_supervisor_schedule_restart(
    service_ctx_t *ctx,
    service_reason_t reason
);

void service_supervisor_cancel_restart(
    service_ctx_t *ctx
);

void service_supervisor_on_sigchld(
    service_ctx_t *ctx
);

void service_supervisor_on_child_reaped(
    service_ctx_t *ctx,
    pid_t pid,
    int status
);
```

### supervisor 不负责

- fork/exec 的细节
- argv/env
- `/proc` parsing
- API health protocol
- UI / status formatting

### 核心 ownership 规则

只有：

```c
waitpid(...) == child_pid
```

才允许：

```text
child.owned = false
child.pid = -1
```

禁止：

```text
SIGKILL sent
→ child considered gone
```

---

# 6. `service_health.c`

## 定位

负责：

> sing-box readiness and health policy

### 职责

- startup readiness
- API availability
- health state
- health failure counters
- health grace period
- Native API runtime cache state读取
- health timer
- health degradation

### 建议接口

```c
int service_health_init(
    service_ctx_t *ctx
);

void service_health_cleanup(
    service_ctx_t *ctx
);

bool service_health_is_ready(
    const service_ctx_t *ctx
);

bool service_health_is_healthy(
    const service_ctx_t *ctx
);

void service_health_on_started(
    service_ctx_t *ctx
);

void service_health_on_stopped(
    service_ctx_t *ctx
);
```

### 原则

`service_health.c`：

- 不直接 fork
- 不直接 waitpid
- 不直接决定 restart
- 不直接修改 child ownership

如果发现 persistent unhealthy：

```text
health
  ↓
report failure
  ↓
service.c / supervisor
  ↓
决定是否 restart
```

### Blocking I/O

禁止 reactor callback 中存在：

```text
poll(..., 2000)
poll(..., 3000)
```

优先从：

```text
singbox_api runtime state
```

读取 readiness / health。

---

# 7. `service_log.c`（可选）

第一轮不强制拆。

如果 `service_process.c` 仍然偏大，可再拆：

```text
service_log.c
```

职责：

- log path
- rotation
- open stdout/stderr target
- log directory
- file safety

接口：

```c
int service_log_prepare(
    service_ctx_t *ctx
);

int service_log_open_child_output(
    service_ctx_t *ctx
);

int service_log_rotate(
    service_ctx_t *ctx
);
```

不要第一轮为了行数强行拆。

---

# 8. `service.h`

## 定位

公共头文件。

只暴露其他 ATPD 模块真正需要的 API。

例如：

```c
typedef struct service_ctx service_ctx_t;

typedef enum {
    SERVICE_STOPPED,
    SERVICE_STARTING,
    SERVICE_RUNNING,
    SERVICE_STOPPING,
    SERVICE_KILLING,
    SERVICE_BACKOFF,
    SERVICE_FAILED
} service_state_t;

int service_init(...);
int service_start(...);
int service_stop(...);
int service_restart(...);
void service_cleanup(...);

service_state_t service_get_state(...);
int service_get_status(...);
```

不要把：

```text
waitpid helper
timer callback
spawn helper
pid validator
health timer callback
```

放进公共头。

---

# 9. `service_internal.h`

## 定位

service subsystem 内部共享定义。

只允许：

```text
service.c
service_process.c
service_supervisor.c
service_health.c
service_log.c
```

使用。

### 可以包含

```c
typedef struct service_child {
    pid_t pid;
    uint64_t generation;
    unsigned long long starttime;
    bool owned;
    bool exec_confirmed;
} service_child_t;
```

以及：

```c
typedef enum {
    SERVICE_REASON_NONE,
    SERVICE_REASON_USER_STOP,
    SERVICE_REASON_RELOAD,
    SERVICE_REASON_PROCESS_EXIT,
    SERVICE_REASON_START_TIMEOUT,
    SERVICE_REASON_HEALTH_FAILURE,
    SERVICE_REASON_EXEC_FAILURE,
    SERVICE_REASON_SHUTDOWN
} service_reason_t;
```

内部 helper prototype：

```c
void service_set_state(...);
void service_record_failure(...);
void service_on_child_exit(...);
```

---

# 10. `service_ctx_t` 重组

建议最终：

```c
struct service_ctx {
    reactor_t *reactor;

    service_state_t state;
    bool desired_running;
    bool shutting_down;

    service_child_t child;

    uint64_t next_generation;

    unsigned int fail_count;
    unsigned int restart_count;

    service_reason_t last_reason;

    int last_exit_code;
    int last_signal;

    bool healthy;

    reactor_timer_t *startup_timer;
    reactor_timer_t *stop_timer;
    reactor_timer_t *retry_timer;
    reactor_timer_t *health_timer;
    reactor_timer_t *monitor_timer;

    /* existing config/path/backoff fields */
};
```

目标是：

> 所有 service runtime state 仍然集中在一个 `service_ctx_t`，而不是每个新文件创建自己的全局变量。

---

# 11. 依赖方向

必须保持单向依赖。

推荐：

```text
                  service.c
              API + state machine
               /      |      \
              /       |       \
             ▼        ▼        ▼
 service_process  supervisor  health
                               │
                               ▼
                          singbox_api

 supervisor
      │
      ▼
   reactor
```

禁止：

```text
service_process
   ↕
supervisor
   ↕
health
```

形成循环调用。

---

# 12. 调用原则

### `service.c`

可以调用：

```text
service_process_*
service_supervisor_*
service_health_*
```

### `service_process.c`

不应该主动调用：

```text
service_start()
service_restart()
service_stop()
```

### `service_health.c`

发现 failure 时：

不要：

```c
service_restart(ctx);
```

而是返回/报告：

```text
SERVICE_HEALTH_FAILED
```

由 service coordinator 决策。

### `service_supervisor.c`

可以通过内部 callback 通知：

```text
child reaped
retry timer fired
stop timeout
```

但最终状态转换由统一 helper 完成。

---

# 13. State Machine

最终推荐：

```text
STOPPED
STARTING
RUNNING
STOPPING
KILLING
BACKOFF
FAILED
```

### 启动

```text
STOPPED
   ↓ service_start
STARTING
   ↓ process_spawn
   ↓ health ready
RUNNING
```

### 停止

```text
RUNNING
   ↓
STOPPING
   ↓ SIGTERM
   ↓ timeout
KILLING
   ↓ SIGKILL
   ↓ waitpid/reap
STOPPED
```

### Crash

```text
RUNNING
   ↓ SIGCHLD
   ↓ reap
BACKOFF
   ↓ retry timer
STARTING
```

### 连续失败

```text
BACKOFF
   ↓ threshold
FAILED
```

---

# 14. `desired_running`

建议新增：

```c
bool desired_running;
```

作用：

```text
true:
    ATPD 希望 sing-box 存活

false:
    user stop / daemon shutdown
```

SIGCHLD 后：

```text
desired_running == true
→ BACKOFF

desired_running == false
→ STOPPED
```

这样避免多个地方通过当前 state 猜：

```text
这个 child exit 是否应该 restart？
```

---

# 15. Child Generation

建议：

```c
uint64_t next_generation;
```

spawn：

```c
ctx->child.generation = ++ctx->next_generation;
```

所有 child-specific timer userdata 保存：

```text
pid
generation
```

callback：

```c
if (pid != ctx->child.pid ||
    generation != ctx->child.generation) {
    // stale callback
    return;
}
```

防止：

```text
旧 child timer
影响新 child
```

---

# 16. SIGCHLD

统一放到：

```text
service_supervisor.c
```

实现：

```c
for (;;) {
    pid_t pid = waitpid(-1, &status, WNOHANG);

    if (pid > 0) {
        service_supervisor_on_child_reaped(
            ctx,
            pid,
            status
        );
        continue;
    }

    if (pid == 0)
        break;

    if (errno == EINTR)
        continue;

    break;
}
```

不要再在多个路径分别 `waitpid()` 后直接清 PID。

---

# 17. Restart 统一入口

所有 restart：

```text
unexpected child exit
startup timeout
persistent health failure
manual restart
```

最终进入同一个 supervisor 路径。

例如：

```c
service_supervisor_schedule_restart(
    ctx,
    reason
);
```

禁止多个 callback：

```text
直接 spawn
直接改 FAILED
直接 schedule timer
```

---

# 18. Timer Ownership

所有 service timer 都存到 ctx：

```text
startup_timer
stop_timer
retry_timer
health_timer
monitor_timer
```

每个 timer 必须明确：

```text
谁创建
谁取消
谁置 NULL
userdata 谁释放
```

所有：

```c
reactor_add_timer()
```

必须检查返回。

---

# 19. Process Identity

移到：

```text
service_process.c
```

不再用宽松 substring 作为 destructive signal 的信任依据。

最低要求：

```text
PID
+
/proc/PID/stat starttime
```

可进一步：

```text
/proc/PID/exe dev/inode
```

这样防止 PID reuse。

---

# 20. Exec Handshake

建议 `service_process_spawn()` 后续引入：

```text
pipe2(O_CLOEXEC)
```

child：

```text
setup
→ exec
```

exec 成功：

```text
CLOEXEC closes pipe
```

exec 失败：

```text
write stage + errno
```

parent 能明确知道：

```text
execv ENOENT
setuid EPERM
chdir EACCES
```

而不是统一：

```text
exit 127
```

---

# 21. Health 与 Process Lifecycle 分离

必须明确：

```text
process alive
≠
application healthy
```

推荐：

```text
service_process.c
    process exists / identity

service_health.c
    sing-box API readiness / health

service_supervisor.c
    lifecycle recovery
```

例如：

```text
process alive
Native API temporarily down
```

应允许：

```text
RUNNING / DEGRADED
```

而不是立即 kill。

---

# 22. 不阻塞 Reactor

`service_health.c` 中所有健康检查必须满足：

```text
non-blocking
```

不要继续在 reactor callback 中：

```c
poll(..., 2000);
poll(..., 3000);
```

优先：

```text
singbox_api cached runtime state
```

---

# 23. Status Snapshot

service subsystem 应向 `status.c` 提供：

```c
typedef struct {
    service_state_t state;

    pid_t child_pid;
    uint64_t generation;

    bool desired_running;
    bool healthy;

    uint64_t restart_count;
    uint64_t fail_count;

    int last_exit_code;
    int last_signal;

    service_reason_t last_reason;

    int backoff_ms;
    bool circuit_open;
} service_status_t;
```

接口：

```c
int service_get_status(
    const service_ctx_t *ctx,
    service_status_t *out
);
```

要求：

```text
read-only
non-blocking
no /proc
no socket
no side effects
```

---

# 24. 第一阶段拆分顺序

不要直接新建四个文件然后大搬家。

推荐：

## Step 1

先创建：

```text
service_internal.h
```

把内部共享类型整理好。

不改变行为。

---

## Step 2

拆：

```text
service_process.c
```

移动：

- fork
- exec
- user/group
- argv/env
- pidfile
- validate process
- signal helper

要求：

```text
行为完全不变
测试完全通过
```

---

## Step 3

拆：

```text
service_health.c
```

移动：

- startup probe
- API health
- health timer
- readiness 判断

第一步仍保持原行为。

拆完测试。

---

## Step 4

拆：

```text
service_supervisor.c
```

移动：

- SIGCHLD
- waitpid
- stop
- kill
- timeout
- retry
- backoff
- circuit breaker

拆完测试。

---

## Step 5

此时 `service.c` 只剩：

```text
public API
state transitions
coordination
status snapshot
```

再开始修 P0/P1 生命周期问题。

---

# 25. 为什么先拆再修

建议本次采用：

```text
mechanical split
→ behavior unchanged
→ tests
→ lifecycle fixes
```

不要：

```text
拆文件
+
重写状态机
+
改 health
+
改 PID identity
```

一次完成。

否则很难定位回归来源。

---

# 26. Mechanical Split 要求

第一轮拆分 PR：

### 允许

```text
移动函数
static → internal declaration
include 调整
Makefile source list 调整
```

### 不允许

```text
改变 timeout
改变 restart 策略
改变 backoff
改变 PID 行为
改变 health semantics
改变 CLI
改变 config
```

目标：

> 只改变物理结构，不改变运行行为。

---

# 27. 拆分后的第二轮 correctness 修复

文件边界稳定后再做：

### `service_supervisor.c`

修：

- zombie / reap
- old/new child race
- SIGKILL ownership
- timer failure
- restart single path
- `desired_running`
- generation

### `service_health.c`

修：

- blocking poll
- readiness cache
- degraded health

### `service_process.c`

修：

- PID identity
- exec error pipe
- structured argv

---

# 28. 推荐目标行数

不作为硬限制。

```text
service.c             250–400
service_process.c     250–350
service_supervisor.c  300–400
service_health.c      150–250
```

如果某文件略超，不为了数字继续碎片化。

---

# 29. 不推荐的拆法

不要拆成：

```text
service_start.c
service_stop.c
service_restart.c
service_timer.c
service_pid.c
```

原因：

同一个 child ownership 会被分散到多个文件。

也不要为了每 100 行建一个文件。

拆分单位应该是：

```text
状态
进程
生命周期
健康
```

而不是：

```text
函数数量
文件行数
```

---

# 30. Build 修改

Makefile 增加：

```text
src/service_process.c
src/service_supervisor.c
src/service_health.c
```

同时确保：

```text
service_internal.h
```

不是公共安装头。

如果当前 include layout 不支持：

可使用：

```text
src/service_internal.h
```

而不是放到公开 `include/`。

优先推荐：

```text
src/service_internal.h
```

避免误当公共 API。

---

# 31. 测试要求

每一步拆分后必须运行现有：

```text
tests/test_singbox_lifecycle.sh
tests/benchmark_atpd.sh
```

以及相关 unit/integration tests。

拆分 PR 的验收标准：

```text
所有现有测试行为一致
```

---

# 32. 增加模块级测试

拆分后可逐步加入：

```text
test_service_process.c
test_service_supervisor.c
test_service_health.c
```

### process

- argv
- identity
- exec failure
- pidfile

### supervisor

- SIGCHLD
- stop
- kill
- restart
- backoff
- generation

### health

- ready
- degraded
- timeout
- recovery

---

# 33. Codex 实施顺序

推荐严格按：

```text
Commit 1
service: add internal service definitions

Commit 2
service: extract process management

Commit 3
service: extract health handling

Commit 4
service: extract supervisor lifecycle

Commit 5
service: reduce coordinator to state machine

Commit 6+
fix lifecycle correctness issues
```

不要一个 commit 完成全部拆分。

---

# 34. Codex 修改前先输出函数归属表

在改代码前，先扫描当前 `service.c`，列出：

```text
函数名
当前职责
目标文件
依赖的全局/ctx字段
调用者
被调用者
```

格式例如：

```text
service_spawn
→ service_process.c

service_probe_port
→ service_health.c

service_sigchld_cb
→ service_supervisor.c

service_start
→ service.c
```

先完成归类，再移动。

---

# 35. Codex 必须检查的耦合

重点检查：

```text
static function cross-file dependency
global variables
g_config
g_api_ctx
reactor timers
service_ctx_t private fields
callback function pointers
signal callbacks
```

如果一个 helper 同时被多个子模块使用：

优先判断它属于：

```text
service_internal helper
```

不要复制代码。

---

# 36. 重构后的设计原则

最终结构必须满足：

```text
service.c
    决定“该做什么”

service_process.c
    负责“进程怎么创建和识别”

service_supervisor.c
    负责“进程退出后怎么处理”

service_health.c
    负责“进程是否真正可用”
```

这是本次重构最重要的边界。

---

# 37. 最终验收标准

重构完成后：

### 结构

- `service.c` 显著缩短
- 没有超大万能 service 文件
- 无循环模块依赖
- 公共 API 没有无意义膨胀

### 行为

- CLI 行为不变
- config 行为不变
- sing-box 独立进程模型不变
- restart/backoff 行为第一阶段不变
- status 能继续工作

### 生命周期

后续 correctness fix 后：

- 0 zombie
- child 未 reap 前不 spawn 新 child
- shutdown 不 restart
- timer failure 可诊断
- Native API failure 不阻塞 reactor

### 可维护性

开发者看到函数时能直接判断应该去哪一个文件找：

```text
process?
→ service_process.c

SIGCHLD/restart?
→ service_supervisor.c

health?
→ service_health.c

state/API?
→ service.c
```

---

# 38. 本次重构不做的事情

不要在这次结构重构里同时：

- 改成 Go
- 嵌入 sing-box/libbox
- 改 Native API 协议
- 改 eBPF
- 改 Netlink
- 改 config 格式
- 大改 reactor
- 改 CLI protocol

这次只解决：

> `service.c` 过长、职责混杂、生命周期逻辑难维护的问题。

---

# 39. 推荐最终目录

```text
src/
├── service.c
├── service_internal.h
├── service_process.c
├── service_supervisor.c
├── service_health.c
│
├── reactor.c
├── singbox_api.c
├── api.c
└── ...

include/
├── service.h
├── reactor.h
├── singbox_api.h
└── ...
```

---

# 40. 最终结论

本次 `service.c` 重构不应以“减少行数”为唯一目标。

正确目标是：

> 把一个 1000+ 行的混合 supervisor 拆成四个清晰的问题域，同时保持 service runtime state 集中、child ownership 单一、依赖方向清晰。

推荐最终边界：

```text
service.c
    API + 状态机

service_process.c
    fork/exec/PID/identity

service_supervisor.c
    SIGCHLD/reap/stop/restart/backoff

service_health.c
    readiness/health/Native API state
```

先完成纯结构拆分并保证行为不变，再逐项处理 zombie、timer、blocking health、process identity 等 correctness 问题。

这会比直接在当前 1000+ 行 `service.c` 上继续堆修复更安全，也更适合作为 ATPD C 版进入稳定阶段之前的长期结构。
