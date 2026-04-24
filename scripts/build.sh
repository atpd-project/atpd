#!/bin/bash
set -e

# 1. 环境定义 (严格匹配 Alpine 3.21 路径)
CC="/usr/bin/clang-19"
STRIP="/usr/bin/llvm-strip"
LIBEV_INCLUDE="/usr/include"
LIBEV_LIB="/usr/lib"

echo "=== Starting Manual Build ==="
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

# 5. 执行手动编译链接
SRC_FILES=$(find src -name "*.c" ! -path "src/yyjson/*" ! -name "epoll.c")
SRC_FILES="$SRC_FILES src/yyjson/yyjson.c"

echo "Compiling..."
$CC -Wall -Wextra -Os -flto -D_GNU_SOURCE \
    -Iinclude \
    -I$LIBEV_INCLUDE \
    -DATP_DEFAULT_DIR=\"/data/adb/atp\" \
    -o build/bin/atpd \
    $SRC_FILES \
    -L$LIBEV_LIB \
    -static \
    -Wl,--gc-sections -Wl,--strip-all \
    -lpthread

# 6. 裁剪二进制文件
echo "Stripping..."
$STRIP build/bin/atpd 2>/dev/null || true

# 7. 验证
echo "=== Build Verification ==="
file build/bin/atpd
ls -lh build/bin/atpd
