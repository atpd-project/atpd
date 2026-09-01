#!/bin/bash
set -e

# 1. 环境定义 (适配 Alpine Edge clang21 与通用环境)
if [ -x "/usr/bin/clang-21" ]; then
    CC="/usr/bin/clang-21"
elif [ -x "/usr/bin/clang-19" ]; then
    CC="/usr/bin/clang-19"
else
    CC="$(command -v clang || echo 'clang')"
fi

if [ -x "/usr/bin/llvm-strip" ]; then
    STRIP="/usr/bin/llvm-strip"
else
    STRIP="$(command -v llvm-strip || command -v strip || echo 'strip')"
fi

echo "=== Starting Pure eBPF True Native Lean Build ==="
echo "CC: $CC"
echo "STRIP: $STRIP"

# 2. 彻底清理
make clean

# 3. 版本生成
./scripts/gen_version.sh build/generated/version_build.h
if [ -f build/generated/version_build.h ]; then
    cat build/generated/version_build.h
fi

# 4. 创建输出目录
mkdir -p build/bin

# 5. 执行手动编译链接 (完全原生，零 UPX 壳)
SRC_FILES=$(find src -name "*.c")

echo "Compiling with native instruction optimizations..."
$CC -Wall -Wextra -Oz -flto -D_GNU_SOURCE -DNDEBUG -Qunused-arguments \
    -ffunction-sections -fdata-sections \
    -fno-unwind-tables -fno-asynchronous-unwind-tables \
    -fmerge-all-constants -fno-ident \
    -DYYJSON_DISABLE_WRITER=1 -DYYJSON_DISABLE_FAST_FP_CONV=1 -DYYJSON_DISABLE_NON_STANDARD=1 \
    -DLOG_LOCATION_ENABLED=0 \
    -DATP_DEFAULT_DIR=\"/data/adb/atp\" \
    -DATP_CONF_FILE=\"atp.conf\" \
    -DATP_PID_FILE=\"run/atpd.pid\" \
    -DATP_LOG_FILE=\"run/atp.log\" \
    -DATP_COMMAND_SOCKET=\"run/atpd.sock\" \
    -Iinclude -Ibuild/generated \
    ${EXTRA_CFLAGS:-} \
    -fuse-ld=lld \
    -static \
    -s \
    -Wl,--gc-sections -Wl,--strip-all -Wl,--build-id=none \
    -o build/bin/atpd \
    $SRC_FILES \
    -lpthread

# 6. 深度原生符号与段剥离 (不使用加壳压缩)
echo "Stripping metadata and debug sections..."
$STRIP --strip-all --strip-unneeded \
       --remove-section=.comment \
       --remove-section=.note* \
       --remove-section=.ARM.exidx* \
       --remove-section=.eh_frame* \
       build/bin/atpd 2>/dev/null || true

# 7. 验证
echo "=== Build Verification ==="
file build/bin/atpd
ls -lh build/bin/atpd
