# ATPD 资源稳定性与内存泄漏测试增强实施任务书

## 1. 任务背景

项目：`atpd-project/atpd`

目标分支：`ebpf-native-api`

ATPD 是一个长期驻留的 C11 daemon，使用单线程 epoll reactor、UDS
IPC、Netlink/XFRM 监听并负责 sing-box 生命周期管理。当前项目已经具备性能
benchmark，但资源稳定性测试仍然不足。

现有 `tests/benchmark_atpd.sh` 已包含：

-   ATPD 启动后的 Baseline RSS
-   `atpd status` 查询性能
-   Netlink interface flap
-   Native API 健康状态
-   sing-box Goroutines 遥测

当前主要缺陷：

1.  ATPD 内存只在启动后读取一次 `VmRSS`
2.  压力测试结束后没有再次测量 ATPD 内存
3.  没有记录 `VmHWM`
4.  没有记录 PSS
5.  没有检测 RSS 是否持续增长
6.  没有检测 FD 泄漏
7.  没有检测 daemon 自身线程数量异常增长
8.  Goroutines 只检测"是否存在数值"，没有判断前后变化
9.  restart/reload 生命周期没有资源泄漏循环
10. 性能指标超标目前主要作为报告，缺少可靠的 CI hard-fail gate

本任务不是重写现有 benchmark，而是在保持现有测试兼容性的基础上，为 ATPD
增加资源稳定性、泄漏和 soak 测试能力。

------------------------------------------------------------------------

## 2. 总体目标

建立以下三级测试能力。

### Level 1 --- Fast Benchmark

适用于 PR / CI。

运行时间目标：约 1--3 分钟。

检测：

-   ATPD baseline RSS
-   ATPD peak RSS
-   ATPD recovery RSS
-   ATPD VmHWM
-   ATPD FD 数量
-   ATPD 线程数量
-   UDS status 压力
-   Netlink event 压力
-   资源是否在压力结束后恢复
-   指标超过硬阈值时 CI 必须失败

### Level 2 --- Resource Stress Test

运行时间目标：约 5--10 分钟。

检测：

-   status 高频请求
-   reload 循环
-   restart 循环
-   Netlink storm
-   RSS 增长趋势
-   FD 泄漏
-   thread 泄漏
-   sing-box Goroutine 增长
-   ATPD daemon 是否异常退出
-   sing-box 是否异常退出
-   UDS 是否始终可用

### Level 3 --- Android Soak Test

用于 Android root 真机。

运行时间：默认 30 分钟，可配置到数小时。

检测长期：

-   RSS
-   PSS
-   VmHWM
-   FD
-   threads
-   CPU
-   uptime
-   UDS status
-   Native API
-   sing-box goroutines
-   网络切换后的资源恢复
-   VPN / Netlink 事件后的资源恢复

该测试暂时不要求进入普通 GitHub Hosted Runner 的 mandatory CI
gate，但脚本必须提交到仓库，可以在 Android / Termux / adb 环境执行。

------------------------------------------------------------------------

## 3. 第一阶段：增强 benchmark_atpd.sh

修改：

`tests/benchmark_atpd.sh`

尽量保留现有 benchmark 输出，不破坏现有调用方式。

### 3.1 新增统一 ATPD 资源采集函数

实现类似：

`collect_atpd_resources()`

输入：

-   ATPD PID
-   当前 phase 名称

采集：

#### Memory

优先读取：

`/proc/$PID/status`

至少采集：

-   VmRSS
-   VmHWM
-   VmSize

如果存在：

`/proc/$PID/smaps_rollup`

额外采集：

-   Pss
-   Private_Clean
-   Private_Dirty

PSS 不存在时显示 `N/A`，但测试不能因此失败。

Android 某些内核环境可能限制 `smaps_rollup`，因此必须 graceful
fallback。

### 3.2 FD 数量

采集：

`/proc/$PID/fd`

例如：

``` bash
find /proc/$PID/fd -mindepth 1 -maxdepth 1 | wc -l
```

如果 `/proc/$PID/fd` 无法访问，显示 `N/A`，不能误判为 `0`。

### 3.3 Thread 数量

优先读取 `/proc/$PID/status` 中的：

`Threads:`

同时可用 `/proc/$PID/task` 交叉检查。

ATPD 当前设计应保持非常低的 thread count，因此异常增长需要被记录。

------------------------------------------------------------------------

## 4. 建立资源采样阶段

benchmark 至少增加以下 checkpoint。

### Phase A --- BASELINE

ATPD 启动并稳定后采样。

不要只固定 `sleep 1` 后立刻测试。

建议实现等待机制：

最多等待 10 秒，每 200--500 ms 检查：

-   ATPD process alive
-   PID file exists
-   UDS status 可响应
-   sing-box 达到预期状态

准备完成后，再额外等待约 1 秒作为稳定期。

记录：

-   baseline_rss
-   baseline_pss
-   baseline_hwm
-   baseline_fd
-   baseline_threads
-   baseline_goroutines

### Phase B --- STATUS_STRESS

执行默认 `2000` 次：

`atpd status`

提供环境变量：

`STATUS_QUERIES`

默认：

`2000`

CI 快速模式可设置：

`200`

执行结束立即采样：

-   rss
-   pss
-   hwm
-   fd
-   threads
-   goroutines

### Phase C --- NETLINK_STRESS

保留现有 dummy interface flap 测试。

将 cycles 参数化：

`NETLINK_CYCLES`

默认：

`100`

CI 可以设置：

`30`

测试后采样资源。

### Phase D --- RECOVERY

所有压力结束后等待 recovery window。

环境变量：

`RECOVERY_SECONDS`

默认：

`10`

期间每秒采样一次。

最终计算：

-   recovery_rss
-   recovery_fd
-   recovery_threads
-   recovery_goroutines

------------------------------------------------------------------------

## 5. RSS 判断方式

不能只判断 `RSS < 3 MB`。

必须同时判断：

### Baseline RSS

目标：

`<= 3.0 MB`

保持现有项目 SLO。

### Peak RSS

Peak 主要作为报告指标。

初期建议：

`<= 5.0 MB`

暂时可设为 WARN 阈值，而不是立即强制 fail。

### Recovery RSS Delta

计算：

`recovery_rss - baseline_rss`

建议初始硬阈值：

`<= 512 KB`

环境变量：

`MAX_RSS_GROWTH_KB`

默认：

`512`

必须允许未来调参。

------------------------------------------------------------------------

## 6. 增加 RSS 趋势检测

新增资源压力脚本时需要记录时间序列。

格式推荐 CSV：

``` text
timestamp,phase,rss_kb,pss_kb,hwm_kb,fd_count,threads,goroutines
```

例如：

``` text
1710000000,status_stress,2380,2200,2496,8,1,17
```

不要只输出终端文本。

输出目录建议：

`${BENCH_DIR}/results/`

文件：

`resources.csv`

这样 GitHub Actions 后续可以作为 artifact 保存。

------------------------------------------------------------------------

## 7. RSS 泄漏趋势算法

不要只比较头尾两个值。

建议实现简单 least-squares linear regression。

如果不希望依赖 Python，可以使用 awk 实现。

输入：最后 N 次 recovery / steady-state RSS sample。

输出：

`RSS slope KB/min`

建议 threshold：

`MAX_RSS_SLOPE_KB_PER_MIN=64`

如果：

`slope > threshold`

则测试失败。

注意 Linux allocator / page reclaim
会造成短期波动，因此不能用单个瞬间值判断泄漏。

推荐至少采样 10 个点以后才执行趋势判断。

------------------------------------------------------------------------

## 8. FD Leak 检测

记录：

-   baseline_fd
-   peak_fd
-   recovery_fd

判断：

`recovery_fd - baseline_fd`

默认允许：

`<= 1`

环境变量：

`MAX_FD_GROWTH=1`

如果重复 status / reload / Netlink 后 FD 稳定增长，必须 FAIL。

FD 是本次测试的重点指标之一。

ATPD 使用 epoll、UDS、Netlink、Native API / process supervision，因此 FD
leak 对长期 daemon 风险很高。

------------------------------------------------------------------------

## 9. Thread Leak 检测

记录：

-   baseline_threads
-   recovery_threads

默认：

`MAX_THREAD_GROWTH=0`

ATPD 设计是单线程 reactor，因此如果 ATPD
自身线程数持续增加，应视为异常。

如果未来实现发生变化，可通过环境变量调整阈值。

不要把 sing-box threads 算入 ATPD threads。

------------------------------------------------------------------------

## 10. Goroutine 检测

这里检测的是 sing-box，而不是 ATPD 本身。

当前 benchmark 只要求 Goroutines 可读取，需要改成记录：

-   baseline
-   peak
-   recovery

默认允许 recovery 相对 baseline：

`MAX_GOROUTINE_GROWTH=5`

如果：

`recovery_goroutines - baseline_goroutines > 5`

输出 WARN 或 FAIL。

建议第一阶段先 WARN，Resource Stress Test 再设置 FAIL。

原因：sing-box 某些连接或 Native API stream 会存在正常 goroutine 波动。

------------------------------------------------------------------------

## 11. Benchmark 必须具有真正 CI Gate

需要增加：

`FAIL_COUNT`

每项检查：

-   PASS
-   WARN
-   FAIL

最终：

如果 `FAIL_COUNT > 0`，执行 `exit 1`；否则 `exit 0`。

不要把所有现有性能 SLO 都立刻变成 hard fail。

建议分类：

### Hard FAIL

-   ATPD process exited
-   UDS unavailable
-   Native API unavailable
-   RSS recovery growth 超过阈值
-   FD leak
-   thread leak
-   baseline RSS 超过明确 hard limit
-   lifecycle failure

### WARN

-   latency 超过目标
-   QPS 低于目标
-   peak RSS 偏高
-   Goroutine 波动较大
-   PSS unavailable

这样可以避免 GitHub Hosted Runner 性能抖动导致大量 false-negative。

------------------------------------------------------------------------

## 12. 新增 Resource Stress Test

创建：

`tests/stress_atpd_resources.sh`

目标：专门检测长期资源稳定性。

### Step 1 --- Baseline

启动 ATPD，等待 ready，采 baseline。

### Step 2 --- Status Storm

执行：

`5000 × atpd status`

支持 concurrency。

环境变量：

``` text
STATUS_STRESS_QUERIES=5000
STATUS_STRESS_CONCURRENCY=4
```

并发实现应避免依赖 GNU parallel，可使用后台 shell worker。

### Step 3 --- Reload Loop

执行：

`100 × atpd reload`

每次：

-   确认 ATPD PID 没有变化
-   确认 `atpd status` 仍正常

每 10 次采样资源。

如果 reload 导致 PID 改变：FAIL。

如果 UDS 丢失：FAIL。

------------------------------------------------------------------------

## 13. Restart 生命周期压力

执行：

`100 × atpd restart`

环境变量：

`RESTART_CYCLES=100`

每次：

1.  记录旧 PID
2.  restart
3.  等待新 PID ready
4.  确认新 PID != old PID
5.  确认旧 PID 已退出
6.  确认新 UDS 正常
7.  确认 sing-box 生命周期正常
8.  检查遗留进程

每 10 次统计：

-   ATPD RSS
-   ATPD FD
-   ATPD threads
-   sing-box PID
-   sing-box goroutines

Restart test 不适合直接比较同一 PID 的 RSS，因为 ATPD PID 会变化。

restart leak 应检测：

### Process Leak

系统中不能存在多个 ATPD daemon。

### sing-box Process Leak

不能残留多个 sing-box instance。

### FD / Socket 状态

`run/atpd.sock` 必须属于当前 daemon，不存在 stale socket。

------------------------------------------------------------------------

## 14. Netlink Storm

执行更大规模 interface events，例如 200--1000 events。

不要创建无限数量的 dummy interface。

推荐循环复用少量名字，例如：

-   `atpd_test0`
-   `atpd_test1`

不断：

`add → up → down → delete`

测试结束必须 cleanup。

即使脚本异常退出，也必须：

``` bash
trap cleanup EXIT INT TERM
```

------------------------------------------------------------------------

## 15. Failure Injection

Resource Stress Test 增加 sing-box crash recovery。

执行：

`kill -9 <singbox pid>`

验证 ATPD supervisor：

-   检测死亡
-   按既有策略 restart
-   新 sing-box ready
-   ATPD 自身不重启
-   UDS 始终工作
-   FD 不持续增长
-   ATPD RSS 最终恢复

重复建议 10 次。

环境变量：

`SINGBOX_KILL_CYCLES=10`

------------------------------------------------------------------------

## 16. Native API Failure

条件允许时，暂时使 Native API 不可连接，例如杀掉 sing-box 后观察 ATPD。

不得为了测试修改 ATPD 的业务设计。

验证：

-   ATPD 不 crash
-   status command 不 hang
-   API 状态正确降级
-   API 恢复后状态能够恢复
-   FD 不泄漏

------------------------------------------------------------------------

## 17. Socket / IPC 稳定性

Stress Test 必须检查：

`atpd.sock`

压力期间：

-   文件存在
-   socket 类型正确
-   status command 不长期 hang

每个 status 请求建议加入 timeout，例如：

``` bash
timeout 2 atpd status
```

避免 CI 永久卡死。

如果系统不存在 GNU timeout，提供兼容 fallback。

Android toybox 有 `timeout` 时优先使用。

------------------------------------------------------------------------

## 18. 新增 Android Soak Test

创建：

`tests/android_soak_atpd.sh`

必须能够在：

-   adb shell
-   Termux/root shell
-   Android service environment

运行。

默认：

`SOAK_DURATION=1800`

即 30 min。

参数：

`SAMPLE_INTERVAL=5`

每 5 秒记录：

-   timestamp
-   ATPD PID
-   ATPD uptime
-   RSS
-   PSS
-   VmHWM
-   FD
-   threads
-   status latency
-   sing-box PID
-   sing-box memory
-   sing-box goroutines
-   Native API status

输出：

`atpd_soak_<timestamp>.csv`

------------------------------------------------------------------------

## 19. Android Soak Workload

不要只 idle。

测试周期建议混合：

### Idle Phase

5 min。

### Status Phase

大量 status 查询。

### Reload Phase

周期 reload。

### Network Event Phase

观察真实网络状态变化。

如果不能主动修改 Android network，至少记录 Netlink event。

### Recovery Phase

继续运行并观察 RSS 是否回落。

------------------------------------------------------------------------

## 20. Android 特殊要求

脚本不要依赖：

-   systemd
-   sudo
-   GNU-specific sed
-   GNU-specific date
-   GNU parallel

尽量兼容 Android toybox。

必须考虑 `/proc/$PID/smaps_rollup` 不存在或 permission denied。

这种情况：

`PSS=N/A`

继续测试。

------------------------------------------------------------------------

## 21. 输出格式

所有测试最终统一输出 summary。

推荐格式：

``` text
ATPD RESOURCE STABILITY REPORT

ATPD PID:
Duration:

Memory:
Baseline RSS:
Peak RSS:
Recovery RSS:
RSS Growth:
RSS Slope:
VmHWM:
PSS:

Resources:
Baseline FDs:
Peak FDs:
Recovery FDs:
FD Growth:

Baseline Threads:
Recovery Threads:
Thread Growth:

sing-box:
Baseline Goroutines:
Peak Goroutines:
Recovery Goroutines:

Stress:
Status Queries:
Reload Cycles:
Restart Cycles:
Netlink Events:
sing-box Kill Tests:

Result:
PASS / FAIL
```

同时产生 CSV。

------------------------------------------------------------------------

## 22. GitHub Actions Integration

检查现有：

`.github/workflows/benchmark.yml`

在不显著增加普通 PR 时间的前提下集成增强版 benchmark。

建议普通 benchmark 运行 Fast mode，例如：

`BENCH_MODE=ci`

参数：

-   status 200
-   netlink 30
-   recovery 5 sec

Resource stress 不要每个 commit 都完整跑。

建议：

-   `workflow_dispatch`
-   scheduled nightly
-   或对特定 branch push

完整 stress：5--10 min。

Android soak 不进入 GitHub Hosted Runner，保留给 self-hosted Android
runner 或手工测试。

------------------------------------------------------------------------

## 23. 建议环境变量

至少支持：

``` text
STATUS_QUERIES
NETLINK_CYCLES
RECOVERY_SECONDS

MAX_BASELINE_RSS_KB
MAX_RSS_GROWTH_KB
MAX_RSS_SLOPE_KB_PER_MIN

MAX_FD_GROWTH
MAX_THREAD_GROWTH
MAX_GOROUTINE_GROWTH

STATUS_STRESS_QUERIES
STATUS_STRESS_CONCURRENCY

RELOAD_CYCLES
RESTART_CYCLES
SINGBOX_KILL_CYCLES

SOAK_DURATION
SAMPLE_INTERVAL
```

所有变量必须有合理默认值。

------------------------------------------------------------------------

## 24. 默认初始阈值

第一版使用：

``` text
MAX_BASELINE_RSS_KB=3072
MAX_RSS_GROWTH_KB=512
MAX_RSS_SLOPE_KB_PER_MIN=64
MAX_FD_GROWTH=1
MAX_THREAD_GROWTH=0
MAX_GOROUTINE_GROWTH=5
```

这些是初始工程阈值。

实现后需要实际跑 Linux + Android 数据。

如果发现正常运行存在稳定噪声，基于真实 benchmark 数据调整。

不要为了让 CI 变绿而随意放宽。

------------------------------------------------------------------------

## 25. Cleanup 要求

所有新增测试必须：

``` bash
trap cleanup EXIT INT TERM
```

cleanup 必须至少完成：

-   stop ATPD
-   stop 测试启动的 sing-box
-   删除 dummy interface
-   删除临时 socket
-   删除测试 sandbox
-   不影响测试前已有的用户进程

禁止使用无条件：

``` bash
pkill -9 sing-box
pkill -9 atpd
```

影响系统中其他实例。

现有测试中的全局清场方式应尽量改成只清理本测试启动的 PID。

------------------------------------------------------------------------

## 26. 不允许为了测试修改 ATPD 业务逻辑

原则：测试应适配 ATPD。

不要为了让测试通过：

-   增加生产代码 sleep
-   人为降低功能
-   禁用 Netlink
-   禁用 Native API
-   修改 reactor 行为
-   隐藏资源数据
-   强制调用 `malloc_trim` 来掩盖泄漏

如果发现真实资源泄漏：

先让测试稳定复现，然后单独修复生产代码。

------------------------------------------------------------------------

## 27. 测试自身必须避免造成假泄漏

调用 `atpd status` 会产生短期 IPC 和进程开销。

但测量对象必须始终是 daemon PID，不能错误测量 CLI 子进程。

每次资源采样前确认：

`/proc/$ATPD_PID`

仍对应相同 ATPD daemon。

------------------------------------------------------------------------

## 28. 验收标准

### Functional

现有 tests 仍然通过。

ATPD：

-   start
-   status
-   reload
-   restart
-   stop

行为不受影响。

### Baseline

可以报告：

-   ATPD VmRSS
-   ATPD VmHWM
-   ATPD PSS（可用时）
-   ATPD FD
-   ATPD Threads

### Stress

完成至少 2000 次 status 后，能够报告压力前后资源差异。

### Leak Detection

人为把：

`MAX_RSS_GROWTH_KB=1`

时，测试应该能够触发 FAIL。

人为把：

`MAX_FD_GROWTH=0`

并制造 FD leak 时，必须 FAIL。

不能只生成报告，必须证明 gate 真正有效。

### Lifecycle

reload ×100：

-   ATPD 不退出
-   PID 不变化
-   UDS 正常

restart ×100：

-   每次正常恢复
-   无 ATPD zombie
-   无 sing-box process leak
-   无 stale UDS socket

### Recovery

压力结束后 RSS / FD / Threads 都必须进行 recovery measurement，不能只测
peak。

### CI

Hard failure 时 shell script exit code 必须非 0，GitHub Action
必须显示失败。

------------------------------------------------------------------------

## 29. 推荐实现顺序

### Commit 1

重构 `benchmark_atpd.sh`

增加：

-   resource collector
-   RSS/PSS/HWM
-   FD
-   Threads
-   checkpoints
-   recovery
-   hard fail gate
-   CSV

不要同时实现 soak。

### Commit 2

新增：

`stress_atpd_resources.sh`

实现：

-   status storm
-   reload loop
-   restart loop
-   netlink storm
-   sing-box kill/recovery
-   leak detection

### Commit 3

新增：

`android_soak_atpd.sh`

实现 Android 长时间资源趋势测试。

### Commit 4

更新 GitHub Actions 以及文档 `README` 或 `docs/testing.md`，说明：

-   Fast Benchmark
-   Resource Stress
-   Android Soak

的运行方法。

------------------------------------------------------------------------

## 30. CodeTips 执行要求

开始修改前，先阅读：

``` text
tests/benchmark_atpd.sh
tests/test_singbox_lifecycle.sh
tests/test_android_service.sh
.github/workflows/
src/
include/
```

了解当前：

-   PID 生命周期
-   UDS 创建方式
-   reload 行为
-   restart 行为
-   sing-box supervisor
-   Netlink reactor

然后再实现测试。

不要基于本文猜测不存在的接口。

如果仓库当前实现与本文细节存在冲突，以当前源码行为为准。

但必须维持本文测试目标：

**验证 ATPD 长期运行时不存在明显 Memory / FD / Thread / Process / IPC
资源泄漏。**

------------------------------------------------------------------------

## 31. 最终交付内容

CodeTips 完成后需要给出：

1.  修改文件列表
2.  每个文件的改动说明
3.  新增测试场景
4.  默认 thresholds
5.  Linux benchmark 实际结果
6.  stress test 实际结果
7.  如可运行 Android，则给 Android soak 结果
8.  尚未覆盖的测试风险
9.  是否发现实际 ATPD resource leak
10. 如果发现 leak，不要静默修改阈值，应明确指出问题并定位可能代码路径

最终目标：

将 ATPD 的资源测试从：

**"启动后内存是否低于 3 MB"**

升级为：

**"长期运行、频繁 IPC、Netlink 事件、reload/restart、sing-box
故障恢复之后，ATPD 的 Memory / FD / Thread / Process
资源仍保持稳定，并可以由 CI 自动阻止明显资源回归。"**
