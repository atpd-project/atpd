# 从 `atp.sh` 迁移到 ATPd

本指南用于把旧 `atp.sh`/`atp4_service.sh` 托管方式迁移到 ATPd 的单一进程 supervisor。迁移期间不要同时运行新旧服务；二者都会尝试启动和管理 sing-box。

## 1. 备份并停用旧服务

先在 root 管理器中禁用旧 ATP 模块。若旧入口直接安装在 `/data/adb/service.d`，将它移到临时备份目录，防止下次开机再次启动：

```sh
su
mkdir -p /data/adb/atp-legacy-service
[ ! -f /data/adb/service.d/atp4_service.sh ] || mv /data/adb/service.d/atp4_service.sh /data/adb/atp-legacy-service/
[ ! -f /data/adb/service.d/atp.sh ] || mv /data/adb/service.d/atp.sh /data/adb/atp-legacy-service/
reboot
```

如果旧模块在自己的目录中提供 `service.sh`，不要只复制脚本，必须禁用该模块。随后重启手机，让旧 watchdog、sentinel 和 sing-box 完整退出：

```sh
adb wait-for-device
adb shell su -c 'pidof atpd; pidof sing-box'
```

两条命令都应无输出。若仍有 sing-box，先确认并停用其所属模块或应用；不要直接启动 ATPd 与它竞争。进程全部退出后再做一致备份：

```sh
su
STAMP="$(date +%Y%m%d-%H%M%S)"
BACKUP="/data/adb/atp-backup-${STAMP}"
mkdir -p "${BACKUP}/service.d"
cp -a /data/adb/atp/. "${BACKUP}/"
cp -a /data/adb/atp-legacy-service/. "${BACKUP}/service.d/"
```

## 2. 保留生产运行态

旧脚本常把 sing-box 工作目录设为 `/data/adb/atp/sing-box`，而 ATPd 使用 `/data/adb/atp` 作为统一工作目录。迁移 `cache.db` 才能保留 selector 选择；同时迁移 provider、rule-set 和面板资源，避免启动后重新下载或引用失效。

```sh
su
OLD_WORKDIR=/data/adb/atp/sing-box
NEW_ROOT=/data/adb/atp

[ ! -f "${OLD_WORKDIR}/cache.db" ] || cp -p "${OLD_WORKDIR}/cache.db" "${NEW_ROOT}/cache.db"
for name in providers rule_set dashboard zashboard; do
    if [ -d "${OLD_WORKDIR}/${name}" ]; then
        mkdir -p "${NEW_ROOT}/${name}"
        cp -a "${OLD_WORKDIR}/${name}/." "${NEW_ROOT}/${name}/"
    fi
done
```

如果旧配置使用其他工作目录，请把 `OLD_WORKDIR` 改为实际路径。不要复制旧 PID、socket 或 watchdog 状态文件。

## 3. 安装 ATPd 和新服务脚本

在电脑上的仓库根目录准备好 Android/arm64 版 `atpd`、带 `with_ebpf` 的 sing-box、生产 `atp.conf` 和 `config.json`。不要把本机 Linux 构建产物部署到 Android；先确认二进制架构，再推送并安装：

```sh
file /path/to/android-atpd /path/to/sing-box
adb push /path/to/android-atpd /data/local/tmp/atpd
adb push /path/to/sing-box /data/local/tmp/sing-box
adb push /path/to/production-atp.conf /data/local/tmp/atp.conf
adb push /path/to/production-config.json /data/local/tmp/config.json
adb push service.d/atpd_service.sh /data/local/tmp/atpd_service.sh

adb shell su -c 'mkdir -p /data/adb/atp/bin /data/adb/atp/run /data/adb/service.d'
adb shell su -c 'cp /data/local/tmp/atpd /data/adb/atp/atpd'
adb shell su -c 'cp /data/local/tmp/sing-box /data/adb/atp/bin/sing-box'
adb shell su -c 'cp /data/local/tmp/atp.conf /data/adb/atp/atp.conf'
adb shell su -c 'cp /data/local/tmp/config.json /data/adb/atp/config.json'
adb shell su -c 'cp /data/local/tmp/atpd_service.sh /data/adb/service.d/atpd_service.sh'
adb shell su -c 'chmod 0755 /data/adb/atp/atpd /data/adb/atp/bin/sing-box /data/adb/service.d/atpd_service.sh'
```

生产 `config.json` 必须包含 sing-box Native API service，并继续引用迁移后的 provider/rule-set 路径。新服务入口支持 `check`、`start`、`status`、`restart` 和 `stop`。

## 4. 预检并切换

```sh
adb shell su -c '/data/adb/service.d/atpd_service.sh check'
adb shell su -c '/data/adb/atp/bin/sing-box tools ebpf status --mode local --cgroup /sys/fs/cgroup'
adb shell su -c '/data/adb/service.d/atpd_service.sh start'
adb shell su -c '/data/adb/service.d/atpd_service.sh status'
```

确认状态包含 `API Engine  Native API`、正整数 `Goroutines`，且只有一个 atpd 和一个 sing-box。再检查生产 selector 仍为迁移前的选择，并验证 rule 模式及实际网络流量。

最后重启手机并复验开机入口：

```sh
adb shell su -c 'pidof atpd; pidof sing-box'
adb shell su -c '/data/adb/service.d/atpd_service.sh status'
adb shell su -c 'cat /data/adb/atp/run/boot.log'
```

`boot.log` 应出现 `ready`。完整验收项目见 [Android 真机测试方案](android-device-test.md)。

## 5. 回滚

```sh
adb shell su -c '/data/adb/service.d/atpd_service.sh stop'
adb shell su -c 'mv /data/adb/service.d/atpd_service.sh /data/adb/service.d/atpd_service.sh.disabled'
```

从 `/data/adb/atp-backup-<时间戳>` 恢复旧配置、工作目录和旧 `service.d` 入口，或在 root 管理器中重新启用旧模块，然后重启。重启后再次确认只有旧 supervisor 管理的一个 sing-box 进程。
