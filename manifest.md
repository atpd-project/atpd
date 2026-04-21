## 📂 ATPd 项目架构快照
生成时间: 2026-04-21 14:46:51
当前分支: **dev**

### 🏗️ 目录结构 (Source Only)
```text
src
src/logger.c
src/netlink.c
src/iface_monitor.c
src/netlink_route.h
src/config_validator.c
src/geoip.c
src/app_filter.c
src/routing.c
src/status.c
src/epoll.c
src/tproxy.c
src/perf_mode.c
src/ui.c
src/fcm_monitor.c
src/api.c
src/version.c
src/utils.c
src/netlink_wait.c
src/service.c
src/main.c
src/mac_filter.c
src/cjson
src/cjson/cJSON.c
src/inet_diag.c
src/cli.c
src/ipv6_manager.c
src/ipset.c
src/config.c
```

### 📊 模块审计概览
| 模块 (File) | 行数 | 风险点扫描 (strncpy/realloc) | 状态 |
| :--- | :--- | :--- | :--- |
| `logger.c` | 126 | 0 | 🟢 形式合规 |
| `netlink.c` | 257 | 0 | 🟢 形式合规 |
| `iface_monitor.c` | 318 | 3 | ⏳ 待审计 |
| `netlink_route.h` | 35 | 0 | 🟢 形式合规 |
| `config_validator.c` | 303 | 2 | ⏳ 待审计 |
| `geoip.c` | 189 | 0 | 🟢 形式合规 |
| `app_filter.c` | 704 | 4 | ⏳ 待审计 |
| `routing.c` | 470 | 1 | ⏳ 待审计 |
| `status.c` | 520 | 2 | ⏳ 待审计 |
| `epoll.c` | 258 | 1 | ⏳ 待审计 |
| `tproxy.c` | 872 | 0 | 🟢 形式合规 |
| `perf_mode.c` | 532 | 0 | 🟢 形式合规 |
| `ui.c` | 473 | 1 | ⏳ 待审计 |
| `fcm_monitor.c` | 342 | 0 | 🟢 形式合规 |
| `api.c` | 865 | 15 | ⏳ 待审计 |
| `version.c` | 29 | 0 | 🟢 形式合规 |
| `utils.c` | 476 | 3 | ⏳ 待审计 |
| `netlink_wait.c` | 39 | 0 | 🟢 形式合规 |
| `service.c` | 454 | 0 | 🟢 形式合规 |
| `main.c` | 229 | 0 | 🟢 形式合规 |
| `mac_filter.c` | 216 | 2 | ⏳ 待审计 |
| `cJSON.c` | 3206 | 18 | ⏳ 待审计 |
| `inet_diag.c` | 593 | 2 | ⏳ 待审计 |
| `cli.c` | 173 | 1 | ⏳ 待审计 |
| `ipv6_manager.c` | 178 | 0 | 🟢 形式合规 |
| `ipset.c` | 233 | 0 | 🟢 形式合规 |
| `config.c` | 141 | 0 | 🟢 形式合规 |

### 🚩 待处理任务 (TODO/FIXME)
* src/cjson/cJSON.c:1917:    /* FIXME: Can overflow here. Cannot be fixed without breaking the API */
* src/cjson/cJSON.c:3160:                /* TODO This has O(n^2) runtime, which is horrible! */
* src/cjson/cJSON.c:3174:             * TODO: Do this the proper way, this is just a fix for now */

---
注意：此文件由脚本自动生成，用于同步至 ATPd 专家模式进行审计。
