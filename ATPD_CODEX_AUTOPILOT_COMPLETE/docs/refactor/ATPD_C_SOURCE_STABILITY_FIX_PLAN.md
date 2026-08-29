# ATPD C 源码稳定性缺陷修复实施任务书

## 1. 任务目标

项目：`atpd-project/atpd`

目标分支：`ebpf-native-api`

本任务针对 ATPD 长期驻留 daemon 的稳定性进行源码级修复，重点处理：

-   FD 泄漏
-   子进程 zombie / reap 生命周期
-   timer 创建失败后的状态泄漏
-   reactor 注册失败后的错误状态
-   UDS 空闲连接资源耗尽
-   PID 误匹配风险
-   nonblocking UDS partial write / EAGAIN
-   reactor 初始化错误处理
-   config reload 的一致性问题

实施前必须重新阅读当前分支实际源码，以当前代码为准，不要仅根据本文中的代码片段直接修改。

重点阅读：

``` text
src/service.c
src/reactor.c
src/uds.c
src/netlink.c
src/config.c
src/singbox_api.c
include/
tests/
```

原则：

**先修复可确认的资源与生命周期问题，再通过测试验证；不要通过放宽测试阈值、增加
sleep、隐藏错误或强制释放 allocator cache 来掩盖问题。**

------------------------------------------------------------------------

# 2. P0：修复 UDS accept 后 reactor 注册失败造成的 FD 泄漏

重点检查：

`src/uds.c`

当前风险模型：

``` text
accept4()
  ↓
client_fd 创建成功
  ↓
reactor_add_fd()
  ↓
注册失败
  ↓
client_fd 无 owner
  ↓
FD 泄漏
```

必须检查 `reactor_add_fd()` 返回值。

期望逻辑：

``` c
if (reactor_add_fd(...) != 0) {
    LOG_WARN(...);
    close(client_fd);
    return;
}
```

具体代码按当前 API 签名实现。

同时检查 UDS 模块中其他所有：

``` text
socket()
accept()
accept4()
dup()
open()
```

成功后是否存在对应的：

``` text
close()
reactor_remove_fd()
```

错误路径。

## 验收标准

通过故障注入使 `reactor_add_fd()` 失败时：

-   新接受的 client fd 必须关闭
-   ATPD fd count 不持续增长
-   reactor handler 不残留
-   daemon 不 crash

------------------------------------------------------------------------

# 3. P0：修复 sing-box 子进程 SIGKILL 后潜在 zombie

重点检查：

`src/service.c`

重点搜索：

``` text
kill()
SIGTERM
SIGKILL
waitpid()
WNOHANG
SIGCHLD
child_pid
```

当前风险：

``` text
kill(child, SIGKILL)
 ↓
waitpid(child, ..., WNOHANG)
 ↓
child 此刻尚未真正退出
 ↓
waitpid 返回 0
 ↓
代码立即 child_pid = -1
 ↓
child 稍后退出
 ↓
SIGCHLD 到达
 ↓
原 PID 已被丢失
 ↓
可能产生 zombie
```

## 修复要求

不要在确认 child 已经被 reap 之前永久丢弃其生命周期信息。

SIGCHLD 处理建议采用：

``` c
while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    ...
}
```

然后：

-   根据返回 PID 判断是否为当前 sing-box
-   正确更新 service state
-   清理对应 PID
-   防止重复 restart
-   防止旧 child 的 SIGCHLD 干扰新 child

必须考虑 race：

``` text
old sing-box 被 kill
new sing-box 已启动
old SIGCHLD 延迟到达
```

不能因为旧 PID 的退出错误地把新实例标记为 stopped。

## 必须考虑

-   normal exit
-   SIGTERM exit
-   SIGKILL exit
-   stop timeout
-   restart
-   rapid restart
-   daemon shutdown
-   child 在 timer callback 之前自行退出
-   SIGCHLD 和 timeout callback 同时发生

## 验收标准

至少执行：

``` text
restart ×100
forced SIGKILL ×100
```

之后：

-   无 sing-box zombie
-   无 ATPD zombie
-   不存在多个残留 sing-box
-   ATPD 状态与实际 child 一致

------------------------------------------------------------------------

# 4. P0：检查所有 reactor_add_timer() 返回值

重点检查全项目：

``` text
reactor_add_timer(
```

特别是：

`src/service.c`

风险：

``` text
state = calloc()
 ↓
reactor_add_timer(...)
 ↓
timer allocation/register 失败
 ↓
返回 NULL
 ↓
state 无 owner
 ↓
内存泄漏
```

更严重的是 async state machine 可能永久停住。

例如：

``` text
service stop
 ↓
创建 stop state
 ↓
timer 创建失败
 ↓
函数却返回 success
 ↓
completion 永远不会发生
```

## 修复要求

每一个 `reactor_add_timer()` 必须明确处理失败。

例如：

``` c
timer = reactor_add_timer(...);

if (!timer) {
    free(state);
    ...
    return error;
}
```

对于重新调度：

``` text
callback
 ↓
reactor_add_timer(... same state ...)
```

如果失败：

-   必须 free state
-   必须进入明确的 fallback
-   必须保持 service state 一致

不能让 state 成为 orphan。

## 对 stop/restart 等关键流程

timer 创建失败时，应根据当前状态采用安全 fallback，例如：

-   同步完成必要 cleanup
-   返回明确错误
-   或进入 fail-safe shutdown

具体策略应依据当前 service state machine 设计决定。

不要简单忽略错误。

## 验收标准

通过 fault injection 模拟：

``` text
malloc failure
timer allocation failure
timerfd creation failure（如果适用）
```

确认：

-   无 state leak
-   stop/restart 不永久卡住
-   daemon 状态一致
-   返回错误可观察

------------------------------------------------------------------------

# 5. P0/P1：全项目检查 reactor_add_fd() 返回值

搜索：

``` text
reactor_add_fd(
```

所有调用点必须检查返回结果。

重点：

``` text
UDS client
Netlink
XFRM
signal fd
event fd
Native API fd
```

原则：

任何：

``` text
resource creation succeeded
+
reactor registration failed
```

都必须 rollback。

典型模式：

``` c
fd = create_resource();

if (fd < 0)
    return error;

if (reactor_add_fd(...) != 0) {
    close(fd);
    rollback_state();
    return error;
}
```

------------------------------------------------------------------------

# 6. P1：修复 XFRM 注册失败但状态显示 registered

重点检查：

`src/netlink.c`

风险模型：

``` text
XFRM fd 创建成功
 ↓
reactor_add_fd() 失败
 ↓
代码仍设置
g_xfrm_registered = 1
 ↓
以后认为 XFRM 已注册
 ↓
不会 retry
 ↓
VPN/IPsec 事件永久丢失
```

## 修复要求

只有 reactor 注册真正成功后才能：

``` text
g_xfrm_registered = 1
```

失败时：

-   不得标记 registered
-   正确 close / rollback fd
-   保持未来 retry 能力
-   输出明确日志

如果存在：

``` text
g_xfrm_reactor
g_xfrm_fd
```

等全局状态，也必须保持一致。

## 验收标准

故障注入 reactor registration failure 后：

``` text
registered == false
```

后续 retry 成功时能够正常恢复 XFRM 监听。

------------------------------------------------------------------------

# 7. P1：增加 UDS idle connection 防护

重点检查：

`src/uds.c`

当前 UDS 是本地 socket，虽然攻击面受权限限制，但长期 daemon 仍需要防止
FD exhaustion。

风险：

``` text
client connect
 ↓
不发送 command
 ↓
connection 长期保持
 ↓
不断重复
 ↓
FD + epoll handler 持续增加
```

## 建议实现

增加：

``` text
MAX_UDS_CLIENTS
```

建议初始：

``` text
32 或 64
```

具体值结合实际 CLI 并发需求确定。

同时增加：

``` text
UDS_CLIENT_IDLE_TIMEOUT
```

建议：

``` text
2–5 秒
```

如果 ATPD 的协议设计明确为：

``` text
connect
send one command
receive response
close
```

则 idle timeout 可以较短。

## 要求

达到 connection limit：

-   新连接立即拒绝/关闭
-   daemon 不 crash
-   已有正常 client 不受影响

idle timeout：

-   自动关闭没有完整 command 的 client
-   reactor handler 必须移除
-   fd 必须关闭
-   timer 必须释放

注意：

不要为了实现 timeout 引入新的 timer/state leak。

------------------------------------------------------------------------

# 8. P1：加强 PID validation

重点检查：

`src/service.c`

搜索：

``` text
validate_process
/proc/%d/exe
/proc/%d/comm
strstr
basename
PID file
```

当前如果存在 substring 判断，例如：

``` text
strstr(exe, "sing-box")
```

条件过宽。

可能误匹配：

``` text
sing-box-test
my-sing-box-helper
sing-box-backup
```

对于一个可能执行：

``` text
kill(pid)
```

的 daemon，应采用更严格验证。

## 推荐验证顺序

### 第一优先

比较：

``` text
realpath(/proc/PID/exe)
```

与：

``` text
realpath(expected_binary_path)
```

完全一致。

### 第二层

可比较：

``` text
st_dev
st_ino
```

确认 executable identity。

### 第三层

如果使用 PID file，建议记录/验证 process starttime。

Linux：

``` text
/proc/PID/stat
```

中的 starttime 可用于降低 PID reuse 风险。

## 原则

不能因为验证失败就 kill 一个"看起来名字差不多"的进程。

宁可：

``` text
refuse to kill + log warning
```

也不要误杀。

------------------------------------------------------------------------

# 9. P1：修复 UDS nonblocking partial write / EAGAIN

重点：

`src/uds.c`

当前 client socket 为 nonblocking。

检查：

``` text
send()
write()
EAGAIN
EWOULDBLOCK
partial write
```

风险：

``` text
status response 较大
 ↓
send 只发送部分
或 EAGAIN
 ↓
代码直接失败/close
 ↓
客户端得到截断 JSON/文本
```

随着 telemetry 增长，这个风险会增加。

## 推荐方案

优先实现正规的 EPOLLOUT state machine。

每个 client state 至少保存：

``` text
response buffer
response length
write offset
```

流程：

``` text
send
 ↓
partial
 ↓
记录 offset
 ↓
监听 EPOLLOUT
 ↓
继续发送
 ↓
全部完成
 ↓
remove fd
close
free state
```

如果当前架构为了保持极简不希望增加完整 write state
machine，可采用有限时间 poll fallback，但必须：

-   有 timeout
-   不阻塞 reactor 太久
-   不 busy loop

优先推荐 event-driven EPOLLOUT。

------------------------------------------------------------------------

# 10. P2：reactor_create() 必须完整处理初始化失败

重点：

`src/reactor.c`

检查 reactor 初始化过程中：

``` text
epoll fd
signalfd
eventfd
timer infrastructure
reactor_add_fd()
```

如果任何内部 fd 创建或注册失败：

``` text
reactor_create()
```

不能返回一个"部分可用"的 reactor。

## 修复要求

采用统一 error path：

``` c
goto fail;
```

fail 中：

-   remove 已注册 handler
-   close signalfd
-   close eventfd
-   close epoll fd
-   free handler
-   free reactor
-   清理 timer
-   清理 signal mask 状态（如果需要）

最终：

``` text
return NULL
```

调用者能够明确知道初始化失败。

------------------------------------------------------------------------

# 11. P2：config_reload_atomic() 语义修复

重点：

`src/config.c`

如果：

``` text
config_reload_atomic()
```

实际上只是：

``` text
config_reload()
```

而：

``` text
config_rollback()
```

没有真正 rollback，则 API 名称和行为不一致。

## 目标

reload 应至少做到：

``` text
parse new config
 ↓
validate
 ↓
prepare delta
 ↓
apply runtime changes
 ↓
commit config
```

如果 runtime apply 失败：

``` text
rollback runtime changes
保持 old config
```

不能：

``` text
先覆盖 live config
 ↓
runtime apply 失败
 ↓
留下半新半旧状态
```

## 如果完整 transaction 当前成本过高

允许第一阶段：

-   移除/更名误导性的 `atomic`
-   明确文档说明非 transactional
-   确保失败不会破坏现有运行配置

但最终建议实现真正 transactional reload。

------------------------------------------------------------------------

# 12. 全项目 ignored return value 审计

除了：

``` text
reactor_add_fd
reactor_add_timer
```

还应检查：

``` text
close
fcntl
epoll_ctl
send
recv
read
write
kill
waitpid
socket
bind
listen
accept
timerfd_settime
eventfd
signalfd
unlink
rename
fsync
```

重点不是机械地检查所有返回值。

重点寻找：

**失败会造成资源泄漏、错误状态或生命周期失控的返回值。**

------------------------------------------------------------------------

# 13. Cleanup ownership 规则

建议此次修复顺便明确资源 ownership。

原则：

## FD

创建者在成功 transfer ownership 前负责 close。

例如：

``` text
socket()
 ↓
local owner
 ↓
reactor_add_fd success
 ↓
ownership → reactor/client state
```

如果 transfer 失败：

``` text
creator close
```

## Heap state

``` text
calloc state
 ↓
timer/register success
 ↓
ownership → callback/reactor
```

失败：

``` text
creator free
```

## Child PID

PID ownership 只有在：

``` text
waitpid successfully reaped
```

后才能彻底释放生命周期记录。

------------------------------------------------------------------------

# 14. 新增测试：UDS FD Exhaustion

增加测试：

``` text
tests/test_uds_resource_limits.sh
```

场景：

1.  启动 ATPD
2.  记录 baseline FD
3.  创建大量 UDS connections
4.  不发送 command
5.  超过 MAX_UDS_CLIENTS
6.  等待 idle timeout
7.  再测 FD

要求：

``` text
recovery_fd ≈ baseline_fd
```

允许少量固定噪声，但不能线性增长。

同时验证：

``` text
atpd status
```

在攻击/压力结束后仍正常。

------------------------------------------------------------------------

# 15. 新增测试：Zombie / Child Reaping

增加：

``` text
tests/test_child_reaping.sh
```

覆盖：

``` text
normal stop
SIGTERM
SIGKILL
restart
rapid restart
stop timeout
```

循环：

``` text
100 次
```

每轮检查：

``` text
/proc/PID/stat
ps
```

不能存在：

``` text
state = Z
```

的 sing-box child。

同时：

-   ATPD 自身不能退出
-   当前 child PID 必须与实际进程一致
-   不能残留多个 sing-box

------------------------------------------------------------------------

# 16. 新增测试：Reactor Failure Injection

需要一种只用于测试的 failure injection 方法。

目标：

模拟：

``` text
reactor_add_fd failure
reactor_add_timer failure
malloc/calloc failure
```

不要在生产行为默认开启。

可以使用：

``` text
#ifdef ATPD_TESTING
```

或者 linker wrapper / test-specific mock。

不要把故障注入环境变量暴露成生产 daemon 的常规接口，除非项目已有统一测试
hook 机制。

验证：

``` text
registration failure
 ↓
resource rollback
 ↓
无 FD leak
无 heap state leak
state machine 不假成功
```

------------------------------------------------------------------------

# 17. Sanitizer 测试

如果当前 build system 支持 GCC/Clang sanitizer，增加测试构建：

``` text
-fsanitize=address
-fsanitize=undefined
-fno-omit-frame-pointer
```

运行至少：

``` text
unit tests
UDS tests
service lifecycle
restart/reload tests
resource stress
```

建议增加独立 CI job：

``` text
asan-ubsan
```

不要求 Android production binary 开 sanitizer。

仅测试构建使用。

------------------------------------------------------------------------

# 18. LeakSanitizer

Linux CI 中如果环境支持：

``` text
ASAN_OPTIONS=detect_leaks=1
```

重点覆盖：

``` text
timer state
UDS client state
reactor handler
config reload
service lifecycle
```

如果某些系统库产生已知 false positive：

必须精确 suppression。

禁止：

``` text
detect_leaks=0
```

直接关闭整个 leak detection 来让 CI 通过。

------------------------------------------------------------------------

# 19. 编译器 Warning

检查当前编译参数。

建议测试/CI 至少启用：

``` text
-Wall
-Wextra
-Wpedantic
-Wshadow
-Wconversion
-Wformat=2
-Wundef
-Wpointer-arith
-Wcast-qual
-Wwrite-strings
```

是否全部设为 `-Werror` 应根据现有代码噪声决定。

至少新修改代码不得引入新的 warning。

------------------------------------------------------------------------

# 20. 修复顺序

严格建议按以下顺序实施。

## Commit 1 --- FD / Reactor Ownership

修复：

``` text
uds accept reactor_add_fd failure
XFRM reactor_add_fd failure
reactor_create internal registration failure
其他 reactor_add_fd ignored result
```

并增加对应测试。

------------------------------------------------------------------------

## Commit 2 --- Child Reaping

重构：

``` text
SIGCHLD
waitpid
SIGKILL
stop timeout
restart lifecycle
```

增加：

``` text
test_child_reaping.sh
```

------------------------------------------------------------------------

## Commit 3 --- Timer Ownership

修复所有：

``` text
reactor_add_timer()
```

失败路径。

增加 failure injection test。

------------------------------------------------------------------------

## Commit 4 --- UDS Resource Limits

实现：

``` text
MAX_UDS_CLIENTS
idle timeout
partial write handling
```

增加：

``` text
test_uds_resource_limits.sh
```

------------------------------------------------------------------------

## Commit 5 --- PID Validation

加强：

``` text
/proc/PID/exe
realpath
inode/device
starttime
```

增加 stale PID / PID mismatch tests。

------------------------------------------------------------------------

## Commit 6 --- Config Reload

修复 reload consistency / transaction semantics。

增加：

``` text
invalid config
partial apply failure
rollback
```

测试。

------------------------------------------------------------------------

## Commit 7 --- Sanitizer CI

增加：

``` text
ASan
UBSan
LSan
```

并运行关键生命周期测试。

------------------------------------------------------------------------

# 21. 与资源稳定性测试任务配合

本任务完成后，应继续执行之前的：

**ATPD Resource Stability & Memory Leak Testing**

重点验证：

``` text
Baseline RSS
Peak RSS
Recovery RSS
RSS slope
FD baseline/peak/recovery
Threads
Goroutines
status storm
Netlink storm
reload ×100
restart ×100
sing-box kill/recovery
Android soak
```

源码修复和测试增强不能彼此替代。

正确顺序：

``` text
修复明确缺陷
 ↓
增加回归测试
 ↓
运行 resource stress
 ↓
运行 sanitizer
 ↓
运行 Android soak
```

------------------------------------------------------------------------

# 22. 禁止事项

禁止为了通过测试：

-   增大 FD leak 阈值掩盖泄漏
-   增大 RSS 阈值掩盖增长
-   忽略 `reactor_add_*` 错误
-   全局关闭 ASan/LSan
-   使用 `malloc_trim()` 掩盖 heap growth
-   加长 sleep 掩盖 race
-   删除失败日志
-   无条件 `pkill` 系统中的 atpd/sing-box
-   为避免 zombie 而忽略 child 状态
-   为避免 PID mismatch 而放宽 process validation

------------------------------------------------------------------------

# 23. 最终验收标准

完成全部 P0/P1 后必须满足：

### FD

-   `reactor_add_fd()` 失败不会泄漏 fd
-   UDS idle client 最终被回收
-   FD stress 后恢复到稳定区间

### Timer

-   `reactor_add_timer()` 失败不会泄漏 state
-   async state machine 不会因为 timer 创建失败永久挂起

### Process

-   SIGTERM / SIGKILL / restart 后 child 都被正确 reap
-   无 zombie
-   无 orphan sing-box
-   delayed SIGCHLD 不会破坏新 child 状态

### Netlink/XFRM

-   reactor registration 失败不会被错误标记为 registered
-   后续 retry 可以恢复

### UDS

-   client 数有上限
-   idle connection 有 timeout
-   partial write / EAGAIN 不造成截断或 daemon hang

### PID Safety

-   stale PID 不会导致误杀无关进程
-   PID reuse 有合理保护

### Reactor

-   部分初始化失败时 `reactor_create()` 必须整体失败并 rollback

### Config

-   reload failure 不得留下明显半更新状态

### Sanitizer

关键生命周期测试：

``` text
0 ASan errors
0 UBSan errors
0 confirmed LSan leaks
```

------------------------------------------------------------------------

# 24. Codex 最终交付报告要求

实现完成后，请输出：

1.  修改文件列表
2.  每个缺陷的 root cause
3.  实际修复方法
4.  新增测试列表
5.  failure injection 方法
6.  ASan/UBSan/LSan 结果
7.  restart ×100 结果
8.  forced SIGKILL ×100 结果
9.  UDS FD exhaustion 测试结果
10. resource stress 前后 RSS/FD/thread 数据
11. 是否发现额外缺陷
12. 尚未修复的风险
13. 哪些修改可能影响现有行为/API
14. 所有测试最终 PASS/FAIL 状态

如果实施过程中发现本文判断与当前源码不符：

**不要为了符合本文而修改正确代码。**

应：

1.  根据当前源码重新确认；
2.  在最终报告中说明该项为何不成立；
3.  不做无意义修改；
4.  继续完成其余可以确认的问题。

最终目标：

> ATPD 在资源紧张、IPC 压力、Netlink 事件、reload/restart、sing-box
> crash、timer/epoll 注册失败等异常条件下，仍能正确维护 FD、heap
> state、child process 和 reactor
> 生命周期，不出现持续资源泄漏、zombie、错误注册状态或不可恢复的 daemon
> 状态。
