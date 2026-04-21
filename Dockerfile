# 锁定 Alpine 3.19 作为基础发行版
FROM alpine:3.19

# 1. 安装 LLVM/Clang 19 及其开发工具
# 2. 安装 musl-dev (核心 C 库)
# 3. 安装 libev 静态库 (ATPd 异步 IO 依赖)
# 4. 安装 linux-headers (底层 TPROXY/Netlink 支持)
RUN apk add --no-cache \
    clang19 \
    llvm19 \
    make \
    musl-dev \
    libev-dev \
    libev-static \
    linux-headers \
    dos2unix \
    git \
    file

# 强制环境变量对齐 Makefile
ENV CC=clang-19
ENV STRIP=llvm-strip-19
ENV CFLAGS="-O3 -flto=thin"

WORKDIR /app

# 默认构建指令：清理 -> 编译 -> 静态链接
CMD ["make", "clean", "all"]

