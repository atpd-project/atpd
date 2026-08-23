## 📂 ATPd 项目架构快照
生成时间: 2026-08-22 14:42:29
当前分支: **pure-ebpf**

### 🏗️ 目录结构 (Source Only)
```text
src
src/api.c
src/api_buffer.c
src/async_validate.c
src/atpd_context.c
src/cli.c
src/config.c
src/config_validator.c
src/ebpf_common.c
src/ebpf.c
src/logger.c
src/main.c
src/netlink.c
src/singbox_api.c
src/reactor.c
src/service.c
src/session.c
src/splice.c
src/uds.c
src/status.c
src/ui.c
src/utils.c
src/version.c
src/atpd_error.c
src/atpd_global.c
src/atpd_init.c
src/cleanup.c
src/yyjson.c
```

### 📊 模块审计概览
| 模块 (File) | 行数 | 风险点扫描 (strncpy/realloc) | 状态 |
| :--- | :--- | :--- | :--- |
| `api.c` | 1996 | 4 | ⏳ 待审计 |
| `api_buffer.c` | 249 | 1 | ⏳ 待审计 |
| `async_validate.c` | 360 | 0 | 🟢 形式合规 |
| `atpd_context.c` | 323 | 1 | ⏳ 待审计 |
| `cli.c` | 214 | 2 | ⏳ 待审计 |
| `config.c` | 337 | 0 | 🟢 形式合规 |
| `config_validator.c` | 142 | 0 | 🟢 形式合规 |
| `ebpf_common.c` | 57 | 0 | 🟢 形式合规 |
| `ebpf.c` | 81 | 0 | 🟢 形式合规 |
| `logger.c` | 310 | 2 | ⏳ 待审计 |
| `main.c` | 661 | 1 | ⏳ 待审计 |
| `netlink.c` | 547 | 0 | 🟢 形式合规 |
| `singbox_api.c` | 604 | 0 | 🟢 形式合规 |
| `reactor.c` | 637 | 0 | 🟢 形式合规 |
| `service.c` | 929 | 0 | 🟢 形式合规 |
| `session.c` | 718 | 0 | 🟢 形式合规 |
| `splice.c` | 380 | 0 | 🟢 形式合规 |
| `uds.c` | 483 | 2 | ⏳ 待审计 |
| `status.c` | 498 | 2 | ⏳ 待审计 |
| `ui.c` | 473 | 1 | ⏳ 待审计 |
| `utils.c` | 681 | 0 | 🟢 形式合规 |
| `version.c` | 36 | 0 | 🟢 形式合规 |
| `atpd_error.c` | 136 | 3 | ⏳ 待审计 |
| `atpd_global.c` | 9 | 0 | 🟢 形式合规 |
| `atpd_init.c` | 195 | 0 | 🟢 形式合规 |
| `cleanup.c` | 30 | 0 | 🟢 形式合规 |
| `yyjson.c` | 11228 | 26 | ⏳ 待审计 |

### 🚩 待处理任务 (TODO/FIXME)

---
注意：此文件由脚本自动生成，用于同步至 ATPd 专家模式进行审计。
