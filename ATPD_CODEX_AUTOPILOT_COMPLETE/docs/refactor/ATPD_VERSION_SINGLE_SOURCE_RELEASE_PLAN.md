# ATPD `version.c / version.h` 单一版本源与构建元数据收敛方案

## 1. 结论

当前版本相关状态分散在至少四处：

```text
include/version.h
Makefile
versions.env
include/atp.h
```

而且值互相冲突：

```text
include/version.h
ATP_VERSION_STRING = "v0.364e1ce-dirty"

Makefile
VERSION = 2.0.0

versions.env
ATP_VERSION = 1.0.0

include/atp.h
ATP_VERSION_MAJOR = 2
ATP_VERSION_MINOR = 0
ATP_VERSION_PATCH = 0
```

这已经不是“文案不一致”，而是：

> 产品版本、Git 构建身份、架构代次、依赖版本被混在了一起。

`version.c` 自身只有 34 行，不需要拆文件。

这一轮目标是：

```text
一个产品版本源
+
一个构建元数据生成路径
+
清晰区分 release version 与 git build identity
```

---

# 2. 当前 `version.c` 的语义本身已经不一致

当前：

```c
const char* atp_get_version(void) {
    return ATP_VERSION_STRING;
}

const char* atp_get_full_version(void) {
    return ATP_VERSION_STRING;
}
```

两个 API：

```text
get_version
get_full_version
```

完全相同。

说明所谓：

```text
full version
```

并没有额外信息。

---

# 3. `atp_get_version_major()` 当前解析实际上没有意义

代码：

```c
const char *v = ATP_VERSION_STRING;
if (v[0] == 'v') {
    return atoi(v + 1);
}
return 0;
```

当前 version：

```text
v0.364e1ce-dirty
```

所以：

```text
atoi("0.364e1ce-dirty")
→ 0
```

---

# 4. minor / patch 更直接是假的

当前：

```c
int atp_get_version_minor(void) {
    return 0;
}

int atp_get_version_patch(void) {
    return 0;
}
```

不管实际产品版本是什么，

永远：

```text
0.0
```

---

# 5. 这套 API 应该删除或改为真正 SemVer

如果产品采用：

```text
MAJOR.MINOR.PATCH
```

就应从 canonical product version 生成：

```text
major
minor
patch
```

如果没有真实 caller：

最推荐：

```text
删除 major/minor/patch getter
```

避免维护第二套解析逻辑。

---

# 6. 当前 `version.h` 注释说 auto-generated

```text
Auto-generated version header - DO NOT EDIT
```

但它被提交进 repo，

而 Makefile 当前又没有任何生成 `version.h` 的 rule。

这是一个明显的 build ownership缺口。

---

# 7. Codex必须先做全仓 callsite/build audit

搜索：

```text
ATP_VERSION_STRING
ATP_VERSION
ATP_COMMIT
ATP_VERSION_MAJOR
ATP_VERSION_MINOR
ATP_VERSION_PATCH
atp_get_version(
atp_get_full_version(
atp_get_version_major(
atp_get_version_minor(
atp_get_version_patch(
VERSION =
ATP_VERSION=
version.h
```

记录：

```text
definition
consumer
build-time producer
release tooling
CI usage
Android package usage
```

---

# 8. 推荐最终只有一个产品版本源

最简单：

```text
/VERSION
```

内容：

```text
0.9.0
```

或发布时：

```text
1.0.0
```

它表示：

> ATPD 产品版本。

---

# 9. `versions.env` 不再保存 ATP product version

这个文件头已经写：

```text
Update this file to upgrade dependencies
```

所以：

```text
MUSL_TOOLCHAIN_VERSION
CURL_VERSION
```

属于它。

但：

```text
ATP_VERSION=1.0.0
```

不属于。

删除：

```text
ATP_VERSION
```

---

# 10. `Makefile VERSION = 2.0.0` 也应删除

当前这个值很可能来自：

```text
architecture generation v2.0
```

而不是 canonical release version。

它如果没有真正参与 build：

直接删除。

如果 packaging需要：

从：

```text
VERSION file
```

读取。

例如：

```make
VERSION := $(shell cat VERSION)
```

---

# 11. `atp.h` 的 MAJOR/MINOR/PATCH 也删除

当前：

```c
#define ATP_VERSION_MAJOR 2
#define ATP_VERSION_MINOR 0
#define ATP_VERSION_PATCH 0
```

是第四份版本来源。

不要维护。

---

# 12. 产品版本和 Git 构建身份必须分开

推荐模型：

```text
product version:
0.9.0

git commit:
364e1ce

dirty:
true/false
```

开发构建显示：

```text
0.9.0-dev+364e1ce
```

或者：

```text
0.9.0+g364e1ce.dirty
```

---

# 13. 不建议继续用 `v0.<commit>`

当前：

```text
v0.364e1ce-dirty
```

这看起来像：

```text
major=0
minor=364e1ce
```

但其实：

```text
commit hash
```

不是 SemVer。

会让：

```text
package manager
version parser
CLI user
release script
```

产生歧义。

---

# 14. Git tag 可以带 `v`

推荐：

```text
tag:
v0.9.0
v1.0.0-rc.1
v1.0.0
```

但程序内部 canonical version：

```text
0.9.0
```

通常无需 `v`。

---

# 15. CLI 可以显示 `v`

如果喜欢：

```text
atpd v0.9.0
```

没问题。

但这是 presentation，

不是内部 version string语义。

---

# 16. 推荐 build metadata scheme

Release clean tag build：

```text
ATPD_VERSION = 1.0.0
ATPD_COMMIT = abc1234
ATPD_DIRTY = 0
```

CLI：

```text
atpd 1.0.0
```

full：

```text
atpd 1.0.0 (abc1234)
```

---

# 17. Development build

例如：

```text
VERSION file = 0.9.0
commit = 364e1ce
dirty = 1
```

可显示：

```text
atpd 0.9.0-dev+364e1ce.dirty
```

---

# 18. 不建议把 branch name编码进版本

branch：

```text
ebpf-native-api
main
feat/*
```

会变化，

也可能含：

```text
/
特殊字符
```

commit hash足够追踪 build。

---

# 19. Release tag与 VERSION一致应由 CI gate保证

例如 tag：

```text
v1.0.0
```

CI：

```text
cat VERSION
→ 1.0.0
```

不一致：

```text
fail release
```

---

# 20. `version.h` 应真正由 build生成

推荐源：

```text
VERSION
git rev-parse --short HEAD
git diff --quiet
```

输出：

```c
#ifndef ATP_VERSION_GENERATED_H
#define ATP_VERSION_GENERATED_H

#define ATP_VERSION_STRING "0.9.0"
#define ATP_COMMIT "364e1ce"
#define ATP_BUILD_DIRTY 1

#endif
```

---

# 21. 生成文件不应该手工维护

有两种合理方式。

### 方案 A：生成到 build目录

推荐：

```text
build/generated/version.h
```

并：

```make
-Ibuild/generated -Iinclude
```

不提交 Git。

### 方案 B：repo里的 include/version.h

build前自动覆写。

不如 A 干净。

---

# 22. 推荐方案 A

理由：

```text
source tree保持只读
并行 build更安全
dirty tree不被 build脚本修改
clean/distclean语义更简单
```

---

# 23. 当前 repo中提交的 `include/version.h` 建议删除

或者改为 fallback template：

```text
version_static.h.in
```

但最终 compiler应 include：

```text
generated/version.h
```

---

# 24. Tarball / no-git build要支持

如果 source package不包含 `.git`：

仍然必须能 build。

规则：

```text
VERSION 必须存在
commit = "unknown"
dirty = 0
```

---

# 25. 不要因为没有 git 就 build fail

release tarball是合法场景。

---

# 26. Git metadata是附加信息，不是产品版本唯一来源

核心：

```text
VERSION file
```

必须独立于 Git。

---

# 27. Dirty判断不能污染 reproducible release build

release CI应该从：

```text
clean checkout
```

build。

因此：

```text
dirty = 0
```

---

# 28. 本地 dirty build则显示 dirty

非常有帮助。

例如用户报：

```text
0.9.0-dev+364e1ce.dirty
```

你马上知道：

```text
binary不是来自 clean commit。
```

---

# 29. `ATP_BUILD_TIME __TIME__` 破坏 reproducible build

当前 `atp.h`：

```c
#define ATP_BUILD_TIME __TIME__
```

即使源码和 commit一样，

每次 compile：

```text
binary bytes不同
```

---

# 30. 如果 `ATP_BUILD_TIME` 没实际 consumer

直接删除。

---

# 31. 如果确实需要 build time

应优先支持：

```text
SOURCE_DATE_EPOCH
```

release build：

```text
deterministic timestamp
```

而不是 compiler `__TIME__`。

---

# 32. 对 ATPD 推荐不展示 build time

真正定位 build：

```text
product version + commit + dirty
```

已经足够。

build time的诊断价值通常有限，

却损害 reproducibility。

---

# 33. Makefile 还写着 `--build-id=none`

当前：

```text
-Wl,--build-id=none
```

说明项目明显在追求：

```text
small/deterministic binary
```

因此保留 `__TIME__` 更不协调。

---

# 34. `ATP_VERSION` 与 `ATP_VERSION_STRING` 重复

当前生成 header：

```c
#define ATP_VERSION_STRING "..."
#define ATP_VERSION "..."
```

两个完全相同。

如果没有不同 consumer需求：

保留一个：

```text
ATP_VERSION_STRING
```

即可。

---

# 35. `ATP_COMMIT` 可以保留

它有独立语义。

---

# 36. 建议再加 dirty boolean

```c
#define ATP_BUILD_DIRTY 0
```

不要把 dirty只能编码在：

```text
version字符串尾部
```

typed更清楚。

---

# 37. 可选：full commit

short 7–12位用于 display。

如果 status/API需要精确定位：

可以 embedded full 40/64 hash。

但会多几十字节。

不必要。

---

# 38. 7位可能长期出现 collision

repo目前很小。

建议：

```text
12 chars
```

几乎无额外成本，

更稳。

---

# 39. 推荐：

```text
ATP_COMMIT = git rev-parse --short=12 HEAD
```

---

# 40. `atp_get_version()` 应返回产品 version

例如：

```text
0.9.0
```

---

# 41. `atp_get_full_version()` 才返回 build identity

例如：

```text
0.9.0-dev+364e1ce.dirty
```

或：

```text
0.9.0 (364e1ce-dirty)
```

---

# 42. 但是 full string不能每次动态 heap分配

简单：

```text
static const generated macro
```

最好。

---

# 43. 推荐 build直接生成 full version macro

例如：

```c
#define ATP_VERSION_FULL "0.9.0-dev+364e1ce.dirty"
```

`version.c`：

```c
const char *atp_get_version(void) {
    return ATP_VERSION_STRING;
}

const char *atp_get_full_version(void) {
    return ATP_VERSION_FULL;
}
```

---

# 44. 或 `snprintf` 到 static buffer

不推荐，

因为：

```text
线程安全
初始化
格式
```

都多出没必要的逻辑。

生成时完成最简单。

---

# 45. Release/Dev full format必须固定

不要这次：

```text
v0.hash-dirty
```

下次：

```text
dev-hash
```

再下次：

```text
1.0.0 (git...)
```

---

# 46. 推荐格式

### clean release/tag build

```text
1.0.0
```

### clean dev build

```text
0.9.0-dev+g364e1ce
```

### dirty dev

```text
0.9.0-dev+g364e1ce.dirty
```

---

# 47. 是否要自动判断 HEAD正好在 tag

可选。

不一定要复杂。

更简单的 release流程：

```text
VERSION决定 product version
RELEASE=1 build
```

就可以。

---

# 48. 但不要让普通 local build误装成官方 release

如果 VERSION是：

```text
1.0.0
```

而 HEAD比 tag多了提交，

直接显示：

```text
1.0.0
```

会误导。

---

# 49. 因此 dev metadata应默认附加 commit

一个简单规则：

```text
如果 HEAD exact match tag v$(VERSION) 且 clean
→ VERSION

否则
→ VERSION-dev+gCOMMIT[.dirty]
```

很合理。

---

# 50. `git describe --exact-match` 可以做

build脚本：

```text
git describe --tags --exact-match HEAD
```

检查是否：

```text
v${VERSION}
```

---

# 51. no-git source package

可以通过：

```text
ATPD_RELEASE_BUILD=1
```

让 packaging明确：

```text
这是 VERSION对应 release source。
```

否则：

```text
VERSION+unknown
```

---

# 52. 不要过度设计 packaging

第一阶段只要：

```text
VERSION canonical
commit metadata
dirty
```

已比当前清晰很多。

---

# 53. `version.c` 不需要 `string.h`

当前：

```c
#include <string.h>
```

没有使用。

删除。

---

# 54. `stdlib.h` 只为了 `atoi`

删除 major parser后：

```text
stdlib.h
```

也可以删除。

---

# 55. `atp.h` include也可能不再需要

`version.c` 当前：

```c
#include "atp.h"
```

如果只用：

```text
ATP_VERSION_STRING
```

而这个来自 generated version header，

就不应该 include巨大 `atp.h`。

---

# 56. 最终 `version.c`

可能只有：

```c
#include "version.h"

const char *atp_get_version(void) {
    return ATP_VERSION_STRING;
}

const char *atp_get_full_version(void) {
    return ATP_VERSION_FULL;
}

const char *atp_get_commit(void) {
    return ATP_COMMIT;
}

bool atp_build_is_dirty(void) {
    return ATP_BUILD_DIRTY != 0;
}
```

---

# 57. 是否需要这么多 getters

其实也可以更小。

如果只有 CLI/status需要：

```text
version/full/commit
```

三个足够。

---

# 58. 不要保留无 caller getter

特别是：

```text
major/minor/patch
```

全仓 audit后删。

---

# 59. Status version语义

ATPD status应区分：

```text
ATPD:
    product version
    commit/build

sing-box:
    Native API reported version
```

不要混成：

```text
Core version
```

---

# 60. ATPD version不应通过 self-exec获取

本进程已有：

```text
compiled constants
```

直接读。

---

# 61. sing-box version继续由 Native API snapshot

这与前面 singbox_api/API方案一致。

---

# 62. UDS `version` command

如果存在：

```text
version
```

应该返回：

```text
ATPD full version
```

或 structured：

```text
version
commit
dirty
```

明确是 ATPD daemon自身。

---

# 63. CLI `--version` 与 `version` command必须一致

当前前面 CLI review已经提出。

测试：

```text
atpd --version
atpd version
```

canonical version部分完全一致。

---

# 64. 不要让 daemon是否运行影响 `--version`

version是：

```text
binary compile-time identity
```

纯本地。

---

# 65. API/UDS version可以包含 daemon build identity

用于：

```text
确认 client binary与 daemon binary是否一致
```

这反而很有价值。

---

# 66. 如果 CLI client连接到另一个版本 daemon

status可显示：

```text
CLI binary: 0.9.1
Daemon:     0.9.0
```

未来有助于升级诊断。

但第一阶段不必实现复杂 handshake。

---

# 67. `versions.env` 当前依赖版本也值得确认

例如：

```text
CURL_VERSION=8.6.0
```

但当前 Makefile：

```text
LIBS = -lpthread
```

并没有 libcurl。

---

# 68. 如果 curl已经不再使用

`versions.env` 里的：

```text
CURL_VERSION
CURL_DOWNLOAD_URL
```

可能也是旧架构残留。

这不属于 `version.c` correctness，

但应该在 repo cleanup阶段 audit。

---

# 69. 同样 musl toolchain仍可能被 scripts使用

不要根据 Makefile一个文件就删。

做 script callsite audit。

---

# 70. 产品 `VERSION` file要不要加 `v`

推荐不要：

```text
1.0.0
```

Tag才：

```text
v1.0.0
```

---

# 71. VERSION文件必须只包含一行

CI验证 regex：

```text
^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$
```

如果采用 SemVer prerelease。

---

# 72. Beta/RC规划

前面 release readiness建议：

```text
v0.9.0-beta.1
v1.0.0-rc.1
v1.0.0
```

VERSION：

```text
0.9.0-beta.1
1.0.0-rc.1
1.0.0
```

---

# 73. `major/minor/patch` parser若保留必须理解 prerelease

例如：

```text
1.0.0-rc.1
```

可以解析前三段。

但如果无 caller，

删除更好。

---

# 74. 不建议自己实现完整 SemVer parser

项目不需要。

Release tooling shell/CI regex足够。

---

# 75. Build-generated header必须依赖 Git state变化

一个坑：

```text
Make不知道 .git/HEAD/ref变化
```

导致 version header stale。

---

# 76. 最简单稳定做法

每次 `make` 前生成：

```text
build/generated/version.h.tmp
```

如果内容与现有不同：

```text
mv replace
```

否则不触发全量重编译。

---

# 77. 例如 phony prepare-version

```text
prepare-version
```

每次执行 cheap git commands。

输出只有内容变化才修改 mtime。

---

# 78. 不要每次都无条件重写 header

否则：

```text
所有依赖 version.h的 object
```

每次 make都重编译。

---

# 79. 更好只让 `version.c` include generated header

不要让：

```text
整个代码库
```

include version.h。

这样 commit变化只重编译：

```text
version.o
```

---

# 80. 这是非常重要的 include hygiene

目前 CLI如果直接 include version.h，

commit变化可能影响更多 object。

推荐：

```text
其他模块 include version API header
只有 version.c include generated macros
```

---

# 81. 可以区分两个 header

例如：

```text
include/version.h
    public API declarations

build/generated/version_build.h
    generated macros
```

`version.c`：

```text
#include "version.h"
#include "version_build.h"
```

---

# 82. 这是推荐最终结构

```text
VERSION

include/version.h
    API only

build/generated/version_build.h
    generated data

src/version.c
    bridge
```

---

# 83. 当前 `include/version.h` 既是 API又是 generated constants

但实际上没有函数 declaration。

这也是层次不清。

---

# 84. 推荐 `include/version.h`

例如：

```c
#ifndef ATP_VERSION_H
#define ATP_VERSION_H

#include <stdbool.h>

const char *atp_get_version(void);
const char *atp_get_full_version(void);
const char *atp_get_commit(void);
bool atp_build_is_dirty(void);

#endif
```

---

# 85. Generated header不安装/不作为 public API

```text
build/generated/version_build.h
```

只给 `version.c`。

---

# 86. 这样以后 C→Go rewrite也更容易复制 release规则

语言无关 source：

```text
VERSION + Git metadata
```

而不是 C header作为版本数据库。

---

# 87. Reproducibility test

同一个：

```text
clean commit
same toolchain/options
SOURCE_DATE_EPOCH
```

build两次：

```text
sha256 binary相同
```

前提包括删除：

```text
__TIME__
```

---

# 88. Dirty build当然可以不同

因为 source不同，

合理。

---

# 89. Release CI test

Tag：

```text
v1.0.0
```

VERSION：

```text
1.0.0
```

build：

```text
atpd --version
```

必须包含：

```text
1.0.0
```

且不含：

```text
dirty
-dev
```

---

# 90. Dev build test

VERSION：

```text
1.0.0
```

HEAD不是 exact tag，

输出：

```text
1.0.0-dev+g<commit>
```

---

# 91. Dirty test

修改 tracked file，

输出含：

```text
dirty
```

---

# 92. no-git build test

复制 source但不复制：

```text
.git
```

仍然 build成功。

输出：

```text
VERSION + unknown metadata
```

或 release mode指定的 VERSION。

---

# 93. No stale header test

连续：

```text
build commit A
checkout commit B
make
```

`atpd --version` commit必须更新到 B。

---

# 94. `make clean` 处理 generated header

推荐：

```text
clean
→ build/ 全删
```

自然删除。

无需：

```text
find include version...
```

---

# 95. `distclean`

不应删除：

```text
VERSION
```

它是 source。

---

# 96. VERSION bump必须是显式 release commit

例如：

```text
release: prepare v1.0.0-rc.1
```

修改：

```text
VERSION
CHANGELOG if later
```

---

# 97. 不需要自动每 commit bump version

commit metadata负责 dev identity。

---

# 98. 与 branch cleanup联动

当：

```text
ebpf-native-api → main
```

之后，

release version仍然：

```text
VERSION
```

不跟 branch绑定。

---

# 99. 与 README联动

README里的：

```text
REACTOR ENGINE v2.0
```

如果指架构代次，

改成：

```text
architecture generation 2
```

或者直接移除版本式命名。

---

# 100. 避免再出现：

```text
product v1.0
architecture v2.0
binary v0.hash
```

用户无法判断自己到底在运行什么版本。

---

# 101. 与 Makefile header文案联动

当前：

```text
Pure eBPF Edition
Native Lean
```

这不是 product version，

并且和当前架构 ownership不准确。

随 main/CLI/eBPF cleanup一起改掉。

---

# 102. Build flavor如果真的需要

例如：

```text
android-aarch64
linux-musl-aarch64
```

应作为：

```text
artifact name
```

不是 product version。

---

# 103. 推荐 artifact naming

例如：

```text
atpd-1.0.0-android-arm64
atpd-1.0.0-linux-arm64-musl
```

而 binary内部仍：

```text
1.0.0
```

---

# 104. 不要把 toolchain version塞进 binary version

依赖/build manifest另行记录。

---

# 105. 如果未来需要 `--build-info`

可以输出：

```text
ATPD version
commit
dirty
compiler
target
```

但这不是第一阶段必须。

---

# 106. `--version` 应保持短

例如：

```text
atpd 1.0.0
```

或 dev：

```text
atpd 1.0.0-dev+g364e1ce
```

---

# 107. `--build-info` 再详细

这样脚本更容易 parse。

---

# 108. 推荐 Commit 1

```text
version: introduce canonical VERSION file
```

内容：

- root VERSION
- remove ATP_VERSION from versions.env
- remove Makefile hardcoded VERSION
- remove atp.h MAJOR/MINOR/PATCH

---

# 109. Commit 2

```text
build: generate version metadata into build directory
```

：

```text
version_build.h
commit
dirty
full version
```

---

# 110. Commit 3

```text
version: make public API independent of generated macros
```

：

```text
include/version.h API only
src/version.c generated bridge
```

---

# 111. Commit 4

```text
version: remove fake major/minor/patch parsing
```

如果无 caller：

直接删。

---

# 112. Commit 5

```text
build: remove non-reproducible compile-time version data
```

删除：

```text
ATP_BUILD_TIME __TIME__
```

如果无必要。

---

# 113. Commit 6

```text
cli: use canonical ATPD full version
```

统一：

```text
--version
version command
help banner
```

---

# 114. Commit 7

```text
release: enforce VERSION and tag consistency
```

CI。

---

# 115. Commit 8

```text
docs: separate product version from architecture naming
```

清 README/Makefile legacy：

```text
v2.0 architecture
Pure eBPF Edition
```

---

# 116. 不建议拆 `version.c`

最终甚至可能：

```text
20–40 LOC
```

它只负责：

```text
return build constants
```

非常理想。

---

# 117. 不建议复杂 runtime parsing

所有 version composition在：

```text
build phase
```

完成。

runtime：

```text
const string return
```

即可。

---

# 118. 最终职责

```text
VERSION
    ↓
product version

Git
    ↓
build identity

build generator
    ↓
version_build.h

version.c
    ↓
stable runtime API

CLI/status/UDS
    ↓
presentation
```

---

# 119. 最终 Invariants

Codex最终必须保证：

```text
I1:
There is exactly one authoritative product-version source.

I2:
Dependency versions are not used as the ATPD product-version source.

I3:
Architecture generation names are not presented as product versions.

I4:
Git commit and dirty state are build metadata, not SemVer fields disguised as minor/patch versions.

I5:
No runtime API returns fake hard-coded minor/patch values.

I6:
Generated version metadata lives in the build output, not as manually maintained source state.

I7:
Building without a .git directory remains supported.

I8:
Clean tagged release builds report exactly the tagged VERSION.

I9:
Development builds identify their commit and dirty state.

I10:
Version metadata does not use __TIME__ unless reproducibility is explicitly handled.

I11:
Only version.c consumes generated build macros; other modules use the stable version API.

I12:
CLI/status/version output is consistent across the product.
```

---

# 120. 最终验收标准

## Single source

```text
grep product version definitions
→ only VERSION is authoritative
```

## Release

```text
tag v1.0.0
VERSION=1.0.0
→ atpd --version == 1.0.0
```

## Dev

```text
non-tag HEAD
→ commit metadata visible
```

## Dirty

```text
tracked modification
→ dirty visible
```

## Tarball

```text
no .git
→ build succeeds
```

## Rebuild

```text
checkout new commit + make
→ embedded commit updates
```

## Reproducibility

```text
same clean source/toolchain/options
→ deterministic binary
```

## API

```text
no fake major/minor/patch getters
or
real SemVer-derived values only
```

---

# 121. 最终结论

`version.c` 不是一个代码复杂度问题，而是一个 **release identity ownership** 问题。

当前同时存在：

```text
v0.<git hash>-dirty
2.0.0
1.0.0
2.0.0 major/minor/patch macros
```

这四套语义必须收敛。

最终推荐非常简单：

```text
VERSION
  = 产品版本唯一来源

Git commit + dirty
  = 构建元数据

version_build.h
  = build时生成

version.c
  = 小型稳定 API

CLI/status
  = 只负责展示
```

这样未来正式进入：

```text
beta
RC
v1.0.0
```

时，版本号、Git tag、二进制输出和发布 artifact 才能真正一致。
