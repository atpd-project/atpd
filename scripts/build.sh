#!/bin/bash
set -e

# 1. 环境定义 (严格匹配 Alpine 3.21 路径)
CC="/usr/bin/clang-19"
STRIP="/usr/bin/llvm-strip-19"
LIBEV_INCLUDE="/usr/include"
LIBEV_LIB="/usr/lib"

echo "=== Starting Manual Build ==="
echo "CC: $CC"

# 2. 版本生成
chmod +x scripts/gen_version.sh
./scripts/gen_version.sh
if [ -f include/version.h ]; then
    cat include/version.h
fi

# 3. 彻底清理
make clean

# 4. 执行手动编译链接
# 我们直接调用 CC，绕过 Makefile 可能存在的链接顺序问题
# 静态链接的关键：-static 必须在对象文件之后，库文件之前
SRC_FILES=$(find src -name "*.c" ! -path "src/cjson/*")
SRC_FILES="$SRC_FILES src/cjson/cJSON.c"

echo "Compiling..."
$CC -Wall -Wextra -O2 -D_GNU_SOURCE \
    -Iinclude -Iinclude/cjson \
    -I$LIBEV_INCLUDE \
    -DVERSION=\"1.0.0\" \
    -DATP_DEFAULT_DIR=\"/data/adb/atp\" \
    -o build/bin/atpd \
    $SRC_FILES \
    -L$LIBEV_LIB \
    -static \
    -lpthread -lev

# 5. 裁剪二进制文件
echo "Stripping..."
$STRIP build/bin/atpd

# 6. 验证
echo "=== Build Verification ==="
file build/bin/atpd
ls -la build/bin/atpd
