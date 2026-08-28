# ATPd Android 真机测试方案

## 目标与边界

本方案只验证 ATPd 的进程托管、Native API、状态查询、重载、异常恢复、VPN 感知和开机启动。`config.json` 的路由与节点由人工确认可用，不在本轮评价代理规则正确性。

测试使用 `/data/adb/atp` 作为唯一运行根目录，并要求 `RUN_DIR=run`、`PID_FILE=run/atpd.pid` 保持默认值。sing-box 的 eBPF 对象由进程持有，不应以 `/sys/fs/bpf` 中是否存在固定 Map 作为通过条件。

## 部署前隔离

1. 在 root 管理器中禁用旧 ATP 模块或旧 `atp.sh` 开机入口，但先不要删除，以便回滚。
2. 重启手机，确认旧进程已退出：

   ```sh
   su -c 'pidof atpd; pidof sing-box'
   ```

   两条命令都应无输出。若仍有 sing-box，先查清所属模块或应用；新启动脚本会拒绝与它并行运行。
3. 备份旧配置、日志和模块包。测试失败时先执行新脚本的 `stop`，再恢复旧模块并重启。

## 真机目录

```text
/data/adb/atp/
├── atpd
├── atp.conf
├── config.json
├── bin/sing-box
└── run/
```

部署并安装启动入口：

```sh
su
mkdir -p /data/adb/atp/bin /data/adb/atp/run /data/adb/service.d
cp atpd /data/adb/atp/atpd
cp sing-box /data/adb/atp/bin/sing-box
cp atp.conf config.json /data/adb/atp/
cp atpd_service.sh /data/adb/service.d/atpd_service.sh
chmod 0755 /data/adb/atp/atpd /data/adb/atp/bin/sing-box
chmod 0755 /data/adb/service.d/atpd_service.sh
```

若继续使用现有 Magisk/KernelSU/APatch 模块，也可以用同一个 `atpd_service.sh` 替换模块的 `service.sh`；不要再从旧 `atp.sh` 启动 sing-box。

## 阶段一：只读预检

```sh
su -c '/data/adb/service.d/atpd_service.sh check'
su -c '/data/adb/atp/atpd -c /data/adb/atp/atp.conf ebpf probe'
su -c '/data/adb/atp/bin/sing-box tools ebpf status --mode local --cgroup /sys/fs/cgroup'
```

通过标准：

- 两个二进制都能运行，sing-box 版本输出包含 `with_ebpf`。
- ATPd 配置检查成功，`ebpf probe` 输出 `supported=1`。
- sing-box 的能力探针成功；它是实际 eBPF 数据路径的权威结果。

## 阶段二：ATPd 核心功能

### T01 首次启动与状态

```sh
su -c '/data/adb/service.d/atpd_service.sh start'
su -c '/data/adb/service.d/atpd_service.sh status'
su -c 'cat /data/adb/atp/run/atpd.pid /data/adb/atp/run/sing-box.pid'
```

通过标准：两个 PID 均存活；状态包含 sing-box 版本、Native API、正整数 Goroutines、Clash Mode 和 eBPF capability；`run/atpd.sock` 存在。

随后用浏览器或 Termux 产生一次 TCP 和一次 DNS/UDP 流量。只确认网络仍可用、sing-box 日志显示流量进入 `ebpf-in`，不评价人工提供的节点速度或分流规则。

### T02 重复启动

记录两个 PID，再次执行 `start`。通过标准：命令成功、两个 PID 不变、没有第二个 atpd 或 sing-box。

### T03 重启闭环

```sh
su -c '/data/adb/service.d/atpd_service.sh restart'
```

通过标准：atpd 与 sing-box 获得新 PID；旧 PID 消失；Native API 和状态查询在 45 秒内恢复。

### T04 热重载

先备份 `atp.conf`，只修改 `SERVICE_HEALTH_CHECK_INTERVAL` 等 ATPd 参数，然后执行：

```sh
su -c '/data/adb/atp/atpd -c /data/adb/atp/atp.conf reload'
```

通过标准：atpd 与 sing-box PID 不变；`run/atp.log` 出现 `Config reload completed successfully`；状态仍可查询。完成后恢复原配置并再次 reload。

### T05 sing-box 异常恢复

```sh
su -c 'kill -9 $(cat /data/adb/atp/run/sing-box.pid)'
```

通过标准：ATPd PID 不变；sing-box 在退避重试后产生新 PID；Native API、Goroutines 和版本信息恢复；日志记录退出与重试，没有残留旧 PID。

### T06 VPN 状态切换

先记录当前 Clash Mode，再连接配置支持的 Google VPN、WireGuard、WARP 或 Tailscale，随后断开。

通过标准：`atpd status` 显示对应 VPN 接口；READY 时切到 `VPN_TARGET_MODE`；IDLE 时恢复本次连接前的模式。连续执行两个连接周期，第二次不得恢复第一次保存的旧模式。

### T07 网络抖动

在 Wi-Fi 与移动数据间切换 10 次，每次间隔至少 3 秒。

通过标准：ATPd PID 保持不变；sing-box 不进入持续重启；状态查询始终能在 3 秒内返回；日志没有 circuit breaker 永久打开或重复 eBPF attach 错误。

## 阶段三：开机与收尾

### T08 开机启动

保持旧模块禁用并重启手机。解锁后执行：

```sh
su -c '/data/adb/service.d/atpd_service.sh status'
su -c 'cat /data/adb/atp/run/boot.log'
```

通过标准：只存在一个 atpd 和一个由其托管的 sing-box；`boot.log` 出现 `ready`；没有旧 `atp.sh` 进程或旧防火墙脚本重新执行。

### T09 正常停止与清理

```sh
su -c '/data/adb/service.d/atpd_service.sh stop'
```

通过标准：两个进程均退出；`atpd.pid`、`sing-box.pid`、`atpd.sock` 被清理；再次执行 `stop` 仍安全成功。随后可再次执行 `start` 恢复测试环境。

## 证据采集

每个失败点保存以下内容：

```sh
su -c '/data/adb/atp/atpd -c /data/adb/atp/atp.conf -n status' > atpd-status.txt
su -c 'cat /data/adb/atp/run/boot.log' > atpd-boot.log
su -c 'cat /data/adb/atp/run/atp.log' > atpd.log
su -c 'cat /data/adb/atp/sing-box.log' > sing-box.log
su -c 'dmesg | tail -n 300' > dmesg-tail.txt
```

最终通过条件是 T01–T09 全部通过，连续运行 30 分钟无异常重启，ATPd 的 PID、FD 数和 RSS 无持续增长。代理节点速度、分流命中率和规则内容不作为 ATPd 验收结论。

常见失败应先按边界归因：eBPF 探针的 `operation not permitted` 指向 root/SELinux/内核能力；只有 atpd PID 而无 sing-box PID 时查看 `sing-box.log`；PID 正常但没有 Native API/Goroutines 时核对 API 地址、端口和 secret。不要通过全局 permissive 或清空防火墙来掩盖失败。
