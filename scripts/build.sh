#!/bin/bash
set -e

# 1. 环境定义 (严格匹配 Alpine 3.21 路径)
CC="/usr/bin/clang-19"
STRIP="/usr/bin/llvm-strip"

echo "=== Starting Pure eBPF True Native Lean Build ==="
echo "CC: $CC"
echo "STRIP: $STRIP"

# 2. 版本生成
chmod +x scripts/gen_version.sh
./scripts/gen_version.sh
if [ -f include/version.h ]; then
    cat include/version.h
fi

# 3. 彻底清理
make clean

# 4. 创建输出目录
mkdir -p build/bin

# 5. 执行手动编译链接 (完全原生，零 UPX 壳)
SRC_FILES=$(find src -name "*.c" ! -name "epoll.c")

echo "Compiling with native instruction optimizations..."
$CC -Wall -Wextra -Oz -flto -D_GNU_SOURCE -DNDEBUG -Qunused-arguments \
    -ffunction-sections -fdata-sections \
    -fno-unwind-tables -fno-asynchronous-unwind-tables \
    -fmerge-all-constants -fno-ident \
    -fstack-protector-strong -D_FORTIFY_SOURCE=3 \
    -DYYJSON_DISABLE_WRITER=1 -DYYJSON_DISABLE_FAST_FP_CONV=1 -DYYJSON_DISABLE_NON_STANDARD=1 \
    -Iinclude \
    ${EXTRA_CFLAGS:-} \
    -o build/bin/atpd \
    $SRC_FILES \
    -static \
    -Wl,--gc-sections -Wl,--strip-all -Wl,--build-id=none \
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
