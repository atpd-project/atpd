# ATPD `main.c` 生命周期编排与 CLI 收敛方案

## 1. 结论

当前：

```text
src/main.c ~665 lines
```

`main.c` 已经承担了太多职责：

```text
CLI command dispatch
config discovery/load
timezone init
daemonize
PID file/lock
PID identity validation
signal dispatch
reactor create/run/destroy
netlink attach
UDS init
service attach/start/stop/reap
reload transaction（目前并不 transactional）
status UDS client
offline status
eBPF probe/status
startup/shutdown cleanup
```

它现在不是“入口文件”，而是第二个 lifecycle manager。

本轮最终目标：

> `main.c` 只负责 CLI dispatch + 调用 daemon lifecycle / control-client API。

建议最终控制在：

```text
~150–300 LOC
```

但不要机械拆分；应随着 `atpd_init`, `service`, `config`, `status`, `global`, `eBPF` 各自收敛自然变薄。

---

# 2. 已确认 P0/P1：daemonize 父进程过早返回成功

当前：

```c
if (opts->daemon && !opts->foreground) {
    daemonize();
}
if (write_pid_file(pp) < 0) ...
atpd_init_run(...)
...
service_start_async(...)
```

而 `daemonize()`：

```c
pid = fork();
if (pid > 0) exit(0);
...
pid = fork();
if (pid > 0) exit(0);
```

也就是说 shell / service manager 看到：

```text
parent exit code = 0
```

时，真正 daemon 尚未完成：

```text
PID lock
config/runtime init
reactor creation
signal registration
UDS bind
sing-box start
```

---

# 3. 实际错误场景

例如：

```text
atpd start --daemon
↓
parent exit(0)
↓
shell认为成功
↓
real daemon:
    PID lock失败
or
    reactor_create失败
or
    UDS bind失败
or
    service_start失败
↓
daemon退出
```

用户/安装脚本仍然得到：

```text
success
```

这是启动可靠性问题。

---

# 4. 正确 daemon startup handshake

如果继续保留 classic double-fork：

```text
original parent
   │
   ├─ pipe/socketpair
   │
daemon child
   ↓
complete required startup
   ↓
READY / FAILED(code, reason)
   ↓
parent exits with matching status
```

parent只有收到：

```text
READY
```

才返回0。

---

# 5. 更简单的部署方案

如果 Android KernelSU / Magisk / APatch service manager本身已经负责后台运行：

可以考虑：

```text
默认 foreground
由 service manager管理进程
```

减少 double-fork复杂度。

但这是部署决策。

如果 CLI仍支持 `--daemon`：

必须有 startup handshake。

---

# 6. P0：reactor create失败仍可让 `do_start()` 返回成功

当前：

```c
run_event_loop();
ret = 0;
```

而：

```c
g_reactor = reactor_create();
if (!g_reactor) {
    LOG_ERROR(...);
    return;
}
```

所以：

```text
reactor_create failure
→ run_event_loop void return
→ do_start ret=0
```

这是已经确认的假成功。

---

# 7. 同样 service start失败也可能被吞成成功

`run_event_loop()`：

```c
if (service_start_async(g_svc) < 0) {
    ...
    return;
}
```

然后 `do_start()`：

```c
run_event_loop();
ret = 0;
```

仍返回成功。

---

# 8. `run_event_loop()` 必须返回 typed/int result

最低限度：

```c
static int run_event_loop(...);
```

例如：

```text
0 clean shutdown
>0 / enum runtime/start failure
```

`do_start()`传播。

长期更推荐：

```text
reactor lifecycle移进 daemon runtime/init
```

main根本不直接创建 reactor。

---

# 9. `main()` 与 `do_start()` 双重 `atpd_context_init()`

当前：

```text
main:
    atpd_context_init()
    state INITIALIZING

do_start:
    atpd_context_init()
    atpd_init_run()
```

这是前面 context/init review已经确认的问题。

必须只初始化一次。

---

# 10. 推荐 daemon lifecycle的唯一入口

```text
do_start
→ daemon_run(opts)
```

由 `daemon_run()`：

```text
context init once
config load once
logger/init transaction
reactor
service
API
UDS
RUN
shutdown
```

`main()` 不提前初始化 daemon runtime state。

---

# 11. `main()` 当前所有命令都会初始化 context

包括：

```text
version
help
stop
reload
check
status
```

完全没必要。

例如：

```text
atpd --version
```

不应该创建/改变 daemon runtime context。

---

# 12. Config也被所有 command预加载

`main()`：

```text
config_set_defaults
resolve config
config_load
```

然后才：

```text
switch command
```

所以：

```text
version/help
```

也依赖 config parsing。

---

# 13. 这是 CLI dependency inversion

理想：

```text
parse command
↓
command-specific prerequisites
```

而不是：

```text
load entire daemon world
↓
再判断只是 --version
```

---

# 14. 推荐 command prerequisite matrix

```text
START
    full config + daemon lifecycle

CHECK
    config only

STATUS
    UDS path/control client
    optional offline config

STOP
    PID/control endpoint identity only

RELOAD
    control endpoint/PID identity only

VERSION
    none

HELP
    none
```

---

# 15. `stop/reload` 不应该因为配置内容坏了而无法控制现有 daemon

当前 main会先：

```text
config_load()
```

如果 config被用户编辑坏：

```text
atpd stop
```

可能在进入 `do_stop()` 前就：

```text
Invalid configuration
return 1
```

这是运维陷阱。

---

# 16. 典型故障

```text
daemon正在运行
↓
用户写坏 config
↓
想 stop/reload/check
```

`check`失败合理。

但：

```text
stop
```

必须仍能工作。

---

# 17. `reload` 也应能把失败交给运行 daemon

CLI只负责：

```text
发送 reload request
```

真正 daemon：

```text
load candidate
validate
失败 → 保持旧 runtime
```

CLI不需要先解析 candidate config。

---

# 18. PID/socket路径是个特殊问题

当前：

```text
pid_file
run_dir
data_dir
```

可能来自 config。

所以 stop/status需要找到 endpoint。

长期建议：

```text
控制 endpoint位置尽量稳定
```

例如：

```text
/run/atpd/atpd.sock
/run/atpd/atpd.pid
```

不要让“坏 config”阻止找到 daemon。

---

# 19. 如果必须允许自定义 control path

CLI可以：

```text
只做最小 tolerant path config读取
```

或者 command option：

```text
--pid-file
--socket
```

但不要要求完整 runtime config验证通过。

---

# 20. `on_signal(SIGHUP)` 过早把 runtime state设为 RELOADING

当前：

```c
atpd_runtime_state_transition(RELOADING);
g_reload = 1;
```

但真正 reload还没开始。

signal callback只是：

```text
收到 request
```

状态不应该提前表达：

```text
transaction正在执行
```

---

# 21. 正确语义

signal：

```text
reload_requested = true
```

idle/control loop真正开始 transaction时：

```text
RUNNING → RELOADING
```

完成：

```text
success → RUNNING/DEGRADED
failure → old runtime RUNNING/DEGRADED
```

---

# 22. 当前 reload失败错误地进入 FAILED

`on_idle()`：

```c
if (config_reload(...) != ATP_OK) {
    ...
    atpd_runtime_state_transition(FAILED);
}
```

我们已经确定：

```text
candidate reload失败
```

只要旧 runtime仍可用：

```text
不能把 daemon标成 FAILED
```

---

# 23. 正确 reload失败

```text
old config/runtime保持
last_reload_result = failed
runtime state恢复 RUNNING or DEGRADED
```

status显示：

```text
last reload failed
```

而不是 daemon FAILED。

---

# 24. 当前 reload不是 atomic

成功 config_load 后：

```c
service_apply_config(g_svc, &g_config);
api_init(&g_api_ctx, &g_config);
```

问题：

```text
live config已变
service apply可能失败
api被重新 init/memset
无 rollback
```

必须完全交给：

```text
config_reload_transaction()
```

main不逐 module apply。

---

# 25. Reload再次调用 `atp_timezone_init()`

当前：

```text
每次 SIGHUP
→ atp_timezone_init()
```

timezone在前面的 utils/timezone方案中已经确定：

```text
startup explicit init once
```

reload不应该重做：

```text
tzdata detection
setenv
temp file
```

除非 timezone本身就是 reloadable config，并明确设计。

---

# 26. Signal request处理顺序有问题

`on_idle()` 当前：

```text
GC
reload
status
if !running → stop
```

如果同一 reactor轮次收到：

```text
SIGHUP
SIGTERM
```

可能：

```text
先做完整 reload
再 shutdown
```

浪费且增加 race。

---

# 27. 推荐优先级

```text
shutdown request
    highest

reload
    next

status
    last
```

即：

```c
if (shutdown_requested) {
    begin_shutdown();
    return;
}
```

---

# 28. `g_running` 命名错误

当前：

```text
g_running = 0
```

实际表示：

```text
shutdown requested
```

不代表：

```text
daemon已经停止
```

应替换为：

```text
shutdown_requested
```

---

# 29. 当前在 cleanup前就设置 STOPPED

`on_idle()`：

```c
atpd_runtime_state_transition(STOPPED);
reactor_stop(r);
```

但之后还有：

```text
session GC
UDS cleanup
service stop/reap
reactor destroy
netlink cleanup
API cleanup
free service
PID cleanup
logger...
```

所以 STOPPED 是假的。

---

# 30. 正确：

```text
STOPPING
↓
all teardown complete
↓
STOPPED
```

而且：

```text
STOPPED
```

最好在最后 teardown owner里设置。

---

# 31. `service_stop_sync()` 是严重 ownership violation

`main.c` 直接访问：

```text
ctx->reactor
ctx->monitor_timer
ctx->retry_timer
ctx->health_timer
ctx->state
ctx->child_pid
ctx->validated_pid
```

甚至自己：

```text
kill
waitpid
unlink service pidfile
```

---

# 32. 必须删除

service自己提供：

```text
service_quiesce()
service_stop_and_reap()
service_destroy()
```

main不能知道内部 timer/child fields。

---

# 33. 当前 shutdown顺序重复

`run_event_loop()`退出后：

```text
session GC
uds_cleanup
service_stop_sync
reactor_destroy
```

然后 `do_start cleanup` 又：

```text
netlink_cleanup
uds_cleanup
api_cleanup
free service
```

---

# 34. `uds_cleanup()` 明确调用两次

如果当前幂等：

不一定 crash。

但这是 lifecycle ownership不清楚的直接证据。

---

# 35. service object也在不同层 stop/free

```text
run_event_loop:
    stop

do_start:
    free
```

如果未来 service cleanup需要更多资源：

很容易漏掉。

应该：

```text
service_destroy
```

唯一 owner。

---

# 36. Reactor destruction位置错误风险

当前 service-start failure：

```c
reactor_destroy(g_reactor);
g_reactor = NULL;
return;
```

但在此前：

```text
netlink_set_reactor
netlink_xfrm_init
service_set_reactor
UDS init
```

多个 subsystem已经保存 reactor引用。

之后 `do_start cleanup`：

```text
netlink_cleanup
uds_cleanup
...
```

可能访问已经释放的 reactor。

这是前面 init review指出的 UAF风险。

---

# 37. Reactor必须接近最后 destroy

shutdown顺序应确保：

```text
所有注册者 detach/cancel
↓
reactor destroy
```

不能 failure path里提前 free reactor。

---

# 38. Signal watch失败当前只 WARN

代码：

```c
reactor_watch_signal(...)
→ WARN
```

对于：

```text
SIGTERM
SIGINT
SIGCHLD
```

这不是普通 optional feature。

---

# 39. Required signals

至少：

```text
SIGTERM
SIGINT
SIGCHLD
```

watch失败应：

```text
startup fail
```

SIGHUP/SIGUSR1是否 required可以定义。

通常也应 required，因为它们是公开 CLI能力。

---

# 40. `reactor_add_fd(netlink)` return被忽略

当前：

```c
reactor_add_fd(...)
```

无检查。

随后：

```text
系统可能宣称 netlink已运行
```

但 fd根本没注册。

应由 netlink attach API自己保证 truthful state。

main不要裸 add。

---

# 41. Netlink又被 attach三次概念混杂

当前：

```text
reactor_add_fd(netlink fd)
netlink_set_reactor
netlink_xfrm_init
```

应该封装成：

```text
netlink_attach_reactor()
```

返回：

```text
OK / DEGRADED / FAIL
```

---

# 42. UDS init失败只 WARN

当前：

```text
Failed to initialize UDS command socket
```

但 CLI status/stop/control高度依赖 UDS。

需要明确：

```text
UDS required?
```

推荐 production daemon：

```text
control UDS required
```

bind/register失败 → startup失败。

如果允许 fallback：

必须 runtime DEGRADED。

---

# 43. `service_start_async(g_svc)` 没检查 `g_svc == NULL`

当前 init应该创建 service。

但 robust main不应依赖隐式保证。

最终 service start属于 init transaction owner，

main不直接调即可。

---

# 44. `netlink_refresh_state()` 紧接 service spawn

这是假设：

```text
child spawn后 Native API/datapath已经 ready
```

实际上 service `STARTING` 与 `READY` 是不同状态。

---

# 45. 已存在 VPN reconcile应在 service readiness后

合理：

```text
service READY generation N
↓
netlink current snapshot
↓
VPN policy reconcile
```

而不是：

```text
fork成功后马上 refresh
```

---

# 46. `do_start()` 无条件打印 “Pure eBPF active”

当前：

```c
LOG_INFO("Engine: Pure eBPF active ...");
```

但此时：

```text
sing-box可能还在 STARTING
eBPF inbound可能失败
Native API未ready
```

这是状态夸大。

---

# 47. 而且 ATPD本身不再拥有 eBPF

应该改为：

```text
sing-box child started / ready
datapath status来自 Native API
```

不再由 main宣告：

```text
Pure eBPF active
```

---

# 48. `main.c` 仍 include `ebpf.h`

并实现：

```text
do_ebpf_probe
do_ebpf_status
```

这些都应随 eBPF removal plan删除。

---

# 49. CLI `ebpf-probe/status` 也应处理

如果这些命令已公开：

建议：

```text
删除
```

或改为：

```text
sing-box datapath status
```

但不要继续做 ATPD direct BPF capability probe。

---

# 50. `do_ebpf_status()` 仍把 process FD count显示成 telemetry

当前：

```text
Process File Descriptors: tel.active_conns
```

前面已经确认：

```text
这是假 active connection telemetry
```

应删除。

---

# 51. `process_is_atpd()` identity判断过宽

当前：

```c
return strncmp(base, "atpd", 4) == 0;
```

所以这些都会被认为是 ATPD：

```text
atpd
atpd-old
atpd-helper
atpd2
atpd-malicious
```

---

# 52. `stop/reload` 因此可能 signal错误进程

特别是在 stale PID file + PID reuse场景：

```text
PID被另一个以 atpd 开头的 executable占用
↓
process_is_atpd=true
↓
SIGTERM/SIGHUP
```

---

# 53. 最低限度应 exact basename

```c
strcmp(base, "atpd") == 0
```

但这仍然不解决：

```text
另一个合法 atpd实例 / PID reuse
```

---

# 54. 更强 PID identity

PID file应记录：

```text
pid
/proc/PID/stat starttime
optional executable device/inode
```

control CLI验证：

```text
pid + starttime + executable identity
```

---

# 55. 更推荐 UDS作为主控制面

如果 daemon UDS可用：

```text
stop
reload
status
```

都通过：

```text
authenticated UDS command
```

这样 PID file只用于：

```text
single-instance lock / fallback
```

---

# 56. UDS stop比发 signal更容易确认结果

例如：

```text
STOP accepted
shutdown generation...
```

不过 command response不应等完整 shutdown太久。

可以：

```text
accepted
```

然后 CLI轮询 socket/PID disappearance。

---

# 57. PID file write还有几个问题

好的部分：

```text
O_NOFOLLOW
O_CLOEXEC
fcntl write lock
```

应保留。

---

# 58. `fsync(g_pid_fd)` 返回值被忽略

PID file不是关键 durable database，

不一定需要 fsync。

二选一：

```text
不需要 durability → 删除 fsync
需要 → 检查结果
```

不要调用又吞错误。

---

# 59. `write()` 没处理 EINTR/partial write

虽然只有几十字节 regular file，

仍应使用：

```text
write_all
```

或者 `dprintf` + check。

---

# 60. PID file open后没有验证 regular file/link count

logger已经做了：

```text
fstat regular + nlink
```

PID lock也建议类似。

`O_NOFOLLOW` 只防 final symlink，

不验证：

```text
FIFO/device/hardlink
```

---

# 61. PID directory创建依赖 `mkdir_recursive`

前面 utils review已经要求：

```text
路径截断检查
EEXIST必须是 directory
symlink policy
```

这里会受益。

---

# 62. `g_pid_fd` 正常 shutdown没有 close

当前：

```text
unlink(pp)
```

但：

```text
g_pid_fd
```

没有显式：

```text
close
```

进程退出后 OS会回收，

但 lifecycle测试/embedding/clean shutdown语义不完整。

---

# 63. 应：

```text
close lock fd
unlink pid path
```

顺序要定义。

一般：

```text
unlink while lock still held
close
```

或 close后 unlink，

需考虑 second instance race。

---

# 64. 更安全 single-instance teardown

推荐：

```text
while daemon alive lock stays held
shutdown complete
unlink PID file
close lock fd
```

确保旧 daemon未完全 teardown时新实例不会进来。

---

# 65. `daemonize()` 没 `chdir("/")`

classic daemon通常会：

```text
chdir("/")
```

当前不做可能是故意，因为代码大量依赖相对 `data_dir`.

这反而说明：

```text
ATPD不应依赖 cwd
```

---

# 66. 前面 utils已指出 `get_app_dir → "."` fallback危险

所有 runtime paths都应该：

```text
absolute or deployment-root resolved
```

这样 daemonize是否 chdir不影响 correctness。

---

# 67. `daemonize()` 的 `dup2()` return未检查

如果 `/dev/null` open/dup失败：

daemon继续运行，

可能保留终端 FD。

应该：

```text
失败 → startup failure handshake
```

---

# 68. `do_stop()` 用 `atoi`

PID parse：

```c
pid_t pid = (pid_t)atoi(buf);
```

问题：

```text
overflow
trailing garbage
"123abc" → 123
```

应使用：

```text
strtol
full-consumption
range check
```

---

# 69. `do_reload()` 同样

统一用：

```text
pidfile_read_identity()
```

不要复制 parser。

---

# 70. `stop` stale PID file时直接 unlink

合理，但只有在：

```text
identity确定不匹配
```

时才能删。

如果只是：

```text
权限不足/readlink transient fail
```

不应武断删除。

---

# 71. `do_stop()` 等待退出只用 `kill(pid,0)`

PID退出后可能快速 reuse。

如果 reuse发生：

```text
kill(pid,0)仍成功
```

CLI会认为旧 daemon没退出，

甚至后面：

```text
SIGKILL reuse后的无关进程
```

这是危险的。

---

# 72. 尤其当前 timeout后：

```c
kill(pid, SIGKILL);
```

没有再次 identity验证。

这是 control-plane safety问题。

---

# 73. Stop循环必须验证同一 process generation

每次检查：

```text
PID + starttime
```

如果：

```text
PID消失
or generation changed
```

表示原 daemon已退出。

绝不能 SIGKILL 新 generation。

---

# 74. `SIGKILL` 后没有确认真的退出

当前：

```text
kill(SIGKILL)
usleep(100ms)
unlink pid
printf "Daemon stopped successfully"
return 0
```

即使：

```text
kill失败
process仍在
```

也报告成功。

---

# 75. 必须检查

```text
kill result
identity disappearance
bounded wait
```

最终才能：

```text
success
```

否则：

```text
stop failed
```

---

# 76. `do_restart()` 完全忽略 stop返回值

当前：

```c
do_stop(opts);
usleep(500000);
return do_start(opts);
```

如果：

```text
stop失败
旧 daemon仍在
```

依然尝试启动新 daemon。

---

# 77. 这会表现为

```text
new start PID lock失败
```

或者更糟：

```text
路径/lock配置不一致 → 双实例
```

---

# 78. 修复

```c
int rc = do_stop(opts);
if (rc != 0) return rc;
return do_start(opts);
```

并且不需要固定：

```text
500ms sleep
```

stop应该只在确认旧实例退出后返回 success。

---

# 79. `do_reload()` 只代表 signal发送成功

打印：

```text
Reload signal sent
```

这一点倒是比较准确。

不要改成：

```text
Reload successful
```

除非通过 UDS得到 transaction结果。

---

# 80. 更推荐 UDS reload response

```text
request accepted
```

如果要同步结果：

```text
generation + result
```

但 CLI不应阻塞很久。

---

# 81. `do_status()` UDS client也有 I/O边界问题

socket：

```text
blocking
3s send/recv timeout
```

send只调用一次：

```c
send(...) == strlen
```

命令很短，通常没问题。

但 robust client仍应：

```text
write_all bounded
```

---

# 82. Response framing依赖 server close

当前：

```text
recv until <=0
```

所以 protocol实际是：

```text
response complete when server closes
```

如果未来 UDS改 persistent connection：

client会不兼容。

---

# 83. UDS方案里已经建议 newline/request framing + bounded output

control client应共享：

```text
uds_client_request()
```

不要在 main里手写 socket protocol。

---

# 84. UDS path写入 `sun_path`会静默截断

当前：

```c
strncpy(sun.sun_path, uds_path, sizeof(sun.sun_path)-1);
```

Unix socket path通常只有约108 bytes。

如果配置路径更长：

```text
CLI会尝试连接截断后的不同 socket
```

---

# 85. 必须显式：

```text
strlen(path) < sizeof(sun.sun_path)
```

否则报：

```text
socket path too long
```

server端也一样。

---

# 86. `setsockopt` return被忽略

如果 timeout设置失败：

client可能：

```text
无限阻塞
```

一般失败概率低，

但 bounded-control-path invariant要求检查。

或者直接：

```text
nonblocking + poll deadline
```

更确定。

---

# 87. Offline status构造假的 service/API runtime对象

当前 UDS失败后：

```c
service_ctx_t local_svc;
service_init(&local_svc, &g_config);

api_ctx_t local_api;
api_init(&local_api, &g_config);

status_show(&g_config, &local_svc, &local_api);
```

---

# 88. 这里语义很混乱

`status` 在 daemon停止时：

```text
创建一个从未运行过的 service_ctx
创建 API ctx
```

然后把它们当作 runtime status。

这不是“当前状态”。

---

# 89. `service_init()` 未来可能有更多副作用

如果它注册资源/分配内存：

offline status还需要 cleanup。

当前代码：

```text
没有 service cleanup
没有 api_cleanup(local_api)
```

可能形成资源泄漏，虽然 CLI很快退出。

---

# 90. Offline status应独立

应该：

```text
daemon unavailable
config summary
install/version
maybe PID/socket info
```

明确标记：

```text
runtime status unavailable
```

不要构造假 RUNNING subsystem object。

---

# 91. Status返回码也应该区分

当前 UDS失败：

```text
offline status_show
return 0
```

所以监控脚本可能：

```text
daemon已死
atpd status exit 0
```

误认为服务健康。

---

# 92. 推荐 exit semantics

例如：

```text
0 daemon reachable, status returned
1 daemon not running/unreachable
2 control/protocol error
```

如果用户只是查看 offline config：

另设 command/flag。

---

# 93. `do_check()` 只验证已经预加载的 `g_config`

本身简单。

未来 config validator应该：

```text
load candidate from selected path
validate
print errors
```

而不是依赖 main global config。

---

# 94. `version/help` 不应触碰 timezone

当前 main第一句：

```c
atp_timezone_init();
```

即：

```text
atpd --version
```

可能执行 Android property/tzdata/file操作。

完全不需要。

---

# 95. Timezone init应只在真正需要 logger/runtime timestamp时执行

甚至 logger可以直接：

```text
UTC/system localtime
```

CLI help/version不应有 platform side effects。

---

# 96. Main include数量就是架构信号

当前 include：

```text
atpd_global
context
init
logger
config
utils
service
api
netlink
status
ui
cli
version
ebpf
cleanup
reactor
uds
session
singbox_api
config_validator
```

入口文件知道几乎所有 subsystem。

---

# 97. 最终 main应该只依赖少数 facade

理想：

```text
cli.h
daemon.h
control_client.h
config_check.h
version.h
```

大致即可。

---

# 98. 不一定要新增很多文件

可以把现有：

```text
atpd_init
```

演化成：

```text
daemon lifecycle facade
```

未必要叫：

```text
daemon.c
```

核心是 public boundary。

---

# 99. `cleanup.h` 应随旧 cleanup.c删除

main目前 include：

```text
cleanup.h
```

但真正 shutdown仍然手写。

前面已经建议：

```text
cleanup.c obsolete
```

删除。

---

# 100. `singbox_api.h` 在 main当前也没有必要直接 include

main不应该知道 Native API transport。

status/API facade负责。

完成后删除 include。

---

# 101. `config_validator.h` 也应由 config check facade负责

main只：

```text
config_check_file(path)
```

---

# 102. `malloc_trim(0)` 不应藏在 event-loop启动

当前：

```text
g_running = 1
malloc_trim(0)
```

这属于：

```text
platform optimization
```

而不是 main correctness。

---

# 103. 先测再决定是否保留

`malloc_trim` 可能：

```text
减少 glibc RSS
```

但 Android不走。

如果 Linux benchmark有价值：

放到：

```text
platform_memory_trim()
```

或 maintenance owner。

不是 main职责。

---

# 104. Runtime exit reason应该传播

当前 reactor退出：

```text
无论 clean signal
fatal reactor error
startup failure
```

`do_start`最后基本都：

```text
ret=0
```

---

# 105. 推荐 typed exit reason

例如：

```c
typedef enum {
    DAEMON_EXIT_CLEAN = 0,
    DAEMON_EXIT_STARTUP_FAILURE,
    DAEMON_EXIT_RUNTIME_FAILURE
} daemon_exit_reason_t;
```

转换成 CLI exit code。

---

# 106. Reactor自己也要区分 stop vs fatal

```text
reactor_stop requested
```

与：

```text
epoll_wait persistent failure
```

不能都表现：

```text
reactor_run returned
```

---

# 107. PID path cleanup必须只由 lock owner做

当前 `do_start()`：

```text
pid_written bool
→ unlink(pp)
```

但没有：

```text
验证当前 path仍是自己创建/锁定的 inode
```

在正常 secure directory里问题小。

更完整可以：

```text
pidfile owner object
```

持 fd/path。

---

# 108. 建议做 `pidfile_t`

如果 PID逻辑仍保留：

```c
typedef struct {
    int fd;
    char path[PATH_MAX];
    pid_t pid;
    uint64_t starttime;
} pidfile_t;
```

API：

```text
pidfile_acquire
pidfile_release
pidfile_read_identity
```

---

# 109. 但不要把 PID manager做太复杂

如果所有 control都迁 UDS，

pidfile只剩：

```text
single-instance lock
```

实现可以很小。

---

# 110. `process_is_atpd()` 应最终删除

由：

```text
pidfile identity helper
```

替代。

---

# 111. Startup READY点必须重新定义

当前：

```text
atpd_init_run成功
→ state RUNNING
→ service还没 start
```

然后：

```text
service_start_async
```

所以：

```text
RUNNING
```

设置太早。

---

# 112. 正确状态大致

```text
INITIALIZING
↓
reactor/control plane ready
↓
service STARTING
↓
service/native API ready
↓
RUNNING
```

如果 API optional：

```text
DEGRADED
```

---

# 113. `atpd_init_run()` 当前不包含 reactor/service start

所以名字“init complete”不等于：

```text
daemon ready
```

要么扩展 transaction，

要么清晰区分：

```text
construct
start
ready
```

---

# 114. 建议前面 init plan中的 phase

```text
CONFIG
LOGGER
CONTEXT
REACTOR
NETLINK
SERVICE_INIT
API
UDS
SERVICE_START
READY
RUN
```

main只调用：

```text
daemon_start_and_run()
```

---

# 115. Shutdown也必须唯一 owner

不要：

```text
on_idle一部分
run_event_loop一部分
do_start cleanup一部分
atexit/cleanup.c一部分
```

---

# 116. 推荐：

```c
daemon_shutdown(runtime, reason);
```

内部固定顺序。

main不知道每个 module细节。

---

# 117. 推荐 shutdown顺序

```text
state → STOPPING
reject reload/new control mutations
cancel async validation
UDS stop accepting
service quiesce
stop/reap sing-box
session close_all + GC
API detach/cleanup
netlink/XFRM detach/cleanup
UDS final cleanup
reactor destroy
service object destroy
PID unlink/close
logger flush/close
state → STOPPED
```

---

# 118. Signal callback应该非常薄

最终：

```text
SIGCHLD
→ service owner notification

SIGHUP
→ runtime request reload

SIGUSR1
→ runtime request status

SIGTERM/SIGINT
→ runtime request shutdown
```

不直接做 state transition/cleanup。

---

# 119. SIGCHLD ownership最好也再收缩

目前 main：

```c
service_sigchld_cb(r, sig, g_svc);
```

如果 reactor支持 userdata：

注册 signal时应把：

```text
service callback
```

直接交 service/runtime dispatch，

减少 main switch。

---

# 120. 但保留 centralized signal dispatch也可以

只要：

```text
不访问 service internals
```

---

# 121. `status` request不应调用同步 remote API

前面的 status/API计划已经覆盖。

在 main里最终：

```text
status request
→ snapshot renderer
```

不能卡 reactor。

---

# 122. Main source里的 “Pure eBPF” 文案应该整体清理

包括：

```text
file header
run log
restart text
check text
eBPF commands
```

当前真实架构：

```text
ATPD root control plane
sing-box owns ebpf-in dataplane
```

---

# 123. 建议文案

例如：

```text
ATPD control daemon
sing-box native ebpf-in dataplane
```

无需宣称：

```text
Pure eBPF Reactor Architecture
```

因为 reactor并不是 eBPF reactor。

---

# 124. Test：daemon startup handshake

模拟：

```text
PID lock fail
reactor fail
UDS bind fail
service start fail
```

执行：

```text
atpd start --daemon
```

parent exit code必须：

```text
non-zero
```

---

# 125. Test：daemon success

只有进入定义好的：

```text
READY/RUNNING
```

后 parent才 exit 0。

---

# 126. Test：reactor_create failure

```text
do_start
→ nonzero
```

直接 regression。

---

# 127. Test：service_start failure

同样：

```text
nonzero
no dangling reactor refs
```

---

# 128. Test：bad config does not block stop

```text
daemon running
corrupt config file
atpd stop
```

必须仍能停止 daemon。

---

# 129. Test：bad config reload

```text
daemon running old config
write invalid candidate
atpd reload
```

daemon：

```text
old runtime continues
state RUNNING/DEGRADED
last_reload_failed
```

---

# 130. Test：simultaneous shutdown + reload

注入：

```text
SIGHUP + SIGTERM
```

预期：

```text
shutdown wins
no config transaction begins after stop request
```

---

# 131. Test：STOPPED timing

在：

```text
service仍活
reactor仍存在
UDS仍accept
```

任何时刻：

```text
state != STOPPED
```

---

# 132. Test：restart stop failure

mock：

```text
do_stop fails
```

`restart`：

```text
must not call start
```

---

# 133. Test：PID basename prefix

PID target executable：

```text
atpd-helper
atpd-old
atpd2
```

必须：

```text
not considered current daemon
```

---

# 134. Test：PID reuse

模拟：

```text
pid N old atpd exits
pid N reused by another process
```

stop path：

```text
must not SIGKILL new process
```

---

# 135. Test：SIGKILL failure

如果 escalation：

```text
kill(SIGKILL) fails
```

CLI不能打印：

```text
Daemon stopped successfully
```

---

# 136. Test：UDS path > sun_path

server/client：

```text
explicit error
```

不能截断连接。

---

# 137. Test：offline status

daemon unavailable：

```text
exit nonzero
runtime unavailable clearly shown
```

不构造 fake service/API state。

---

# 138. Test：version/help no side effects

执行：

```text
atpd --version
atpd --help
```

应不：

```text
load config
init context
init timezone
mkdir
open log
touch PID
```

---

# 139. Test：1000 start/stop cycles

检查：

```text
PID fd
UDS fd
netlink fd
timer
child/zombie
RSS slope
```

无增长。

---

# 140. Test：all failure injection phases

对：

```text
config
logger
context
reactor
netlink
service init
API
UDS
service start
ready
```

逐 phase失败。

验证：

```text
reverse cleanup
no UAF
no leaked PID lock
correct exit code
```

---

# 141. 推荐 Commit 1

```text
main: propagate reactor and service startup failures
```

这是最小关键 correctness修复。

---

# 142. Commit 2

```text
main: make daemon mode report real startup result
```

parent-child startup handshake。

---

# 143. Commit 3

```text
main: separate command prerequisites
```

让：

```text
help/version/stop/reload
```

不再无条件 full-config/context init。

---

# 144. Commit 4

```text
runtime: replace running/reload/status globals with explicit requests
```

并设置：

```text
shutdown precedence
```

---

# 145. Commit 5

```text
config: move reload transaction out of main
```

删除：

```text
service_apply_config
api_init
timezone init
```

从 on_idle。

---

# 146. Commit 6

```text
service: remove main-owned stop/reap implementation
```

删除：

```text
service_stop_sync
```

---

# 147. Commit 7

```text
runtime: centralize startup and shutdown ownership
```

消除：

```text
run_event_loop cleanup
do_start cleanup
```

双层 teardown。

---

# 148. Commit 8

```text
control: harden PID identity and restart semantics
```

- exact/generation identity
- full PID parse
- no SIGKILL after PID reuse
- restart checks stop return

---

# 149. Commit 9

```text
status: move UDS client and offline status out of main
```

例如：

```text
control_client.c
```

如果已经有 CLI module合适，也可以放那里。

---

# 150. Commit 10

```text
main: remove ATPD-owned eBPF commands and legacy wording
```

随 eBPF removal plan。

---

# 151. Commit 11

```text
main: remove global/service/API/reactor direct dependencies
```

随着前面方案完成。

---

# 152. 最终 main草图

理想大致：

```c
int main(int argc, char **argv)
{
    atp_options_t opts;

    if (parse_arguments(argc, argv, &opts) != 0)
        return ATPD_EXIT_USAGE;

    switch (opts.command) {
    case CMD_START:
        return daemon_command_start(&opts);

    case CMD_STOP:
        return control_command_stop(&opts);

    case CMD_RESTART:
        return control_command_restart(&opts);

    case CMD_STATUS:
        return control_command_status(&opts);

    case CMD_RELOAD:
        return control_command_reload(&opts);

    case CMD_CHECK:
        return config_command_check(&opts);

    case CMD_VERSION:
        print_version();
        return 0;

    case CMD_HELP:
        print_usage(argv[0]);
        return 0;

    default:
        return ATPD_EXIT_USAGE;
    }
}
```

---

# 153. 这不是为了追求“短 main”

真正价值是：

```text
main不再拥有 subsystem lifecycle knowledge
```

以后 service/reactor/API内部改动：

```text
不需要改 main
```

---

# 154. Main最终不应该知道

```text
service timer
service child_pid
validated_pid
netlink raw fd
API native ctx
session GC internals
reactor fd registrations
eBPF capability
```

---

# 155. Main可以知道

```text
command
options
high-level exit result
```

仅此而已。

---

# 156. 与 `atpd_global` 方案联动

最终删除：

```text
g_config
g_api_ctx
g_reactor
g_svc
g_running
g_reload
g_show_status
```

main将是最大的受益者。

---

# 157. 与 `atpd_init` 方案联动

init transaction成为：

```text
daemon lifecycle authority
```

而不是 main + init各做一半。

---

# 158. 与 `service` 方案联动

main不再：

```text
kill/waitpid/timer cancel/state write
```

全部 service owner。

---

# 159. 与 `config` 方案联动

main不再：

```text
reload live config
apply service
re-init API
```

只发：

```text
reload transaction request
```

---

# 160. 与 `status` 方案联动

main不构建 subsystem对象。

status读取：

```text
daemon snapshot
```

control client负责 UDS。

---

# 161. 与 `logger/timezone` 方案联动

help/version没有：

```text
timezone/logger/platform side effects
```

daemon startup才初始化。

---

# 162. 与 eBPF removal联动

main彻底删除：

```text
ebpf.h
probe
status
fake telemetry
Pure eBPF claims
```

---

# 163. 最终 Invariants

Codex最终必须保证：

```text
I1:
Daemon-mode parent returns success only after required daemon startup succeeds.

I2:
reactor/service startup failure always produces a non-zero process exit.

I3:
main does not initialize daemon context/config for commands that do not need them.

I4:
stop remains usable even when runtime config is invalid.

I5:
reload failure preserves the previous working runtime state.

I6:
shutdown request has priority over reload/status requests.

I7:
STOPPED is published only after teardown is actually complete.

I8:
main never accesses service internal fields or performs child reap/kill lifecycle itself.

I9:
reactor is destroyed only after every dependent subsystem has detached.

I10:
restart never starts a new instance when stop failed.

I11:
PID reuse cannot cause ATPD to SIGKILL an unrelated process.

I12:
offline status never fabricates a runtime service/API state.

I13:
main contains no ATPD-owned eBPF capability/telemetry logic.

I14:
main contains no public global service-locator dependencies after migration.
```

---

# 164. 最终验收标准

## Exit correctness

```text
reactor fail
service start fail
PID lock fail
UDS required fail
→ nonzero
```

## Daemon mode

```text
parent 0 == daemon genuinely ready
```

## Control

```text
bad config still allows stop
restart honors stop failure
PID reuse safe
```

## Reload

```text
invalid candidate preserves old runtime
```

## Shutdown

```text
STOPPING during teardown
STOPPED only at end
no UAF
```

## Architecture

```text
main no service internals
main no raw reactor/netlink registration
main no Native API implementation detail
main no eBPF module
```

## Size

最终通常：

```text
~150–300 LOC
```

但 LOC只是结果，不是目标。

---

# 165. 最终结论

`main.c` 是前面几轮 ownership问题的汇合点。

目前最高优先级不是“拆成几个文件”，而是修正生命周期 truth：

```text
daemon parent不能提前报成功
reactor/service failure不能被吞成 success
reload失败不能把可工作的 daemon标 FAILED
STOPPED不能提前发布
restart不能忽略 stop失败
PID reuse不能导致杀错进程
```

然后随着：

```text
atpd_init
config
service
status
global
eBPF
```

各自完成重构，

`main.c` 会自然缩成一个真正的 command dispatcher。

这才是正确的最终形态。
