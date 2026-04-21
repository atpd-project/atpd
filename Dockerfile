# 阶段 1: 编译环境
FROM alpine:3.21 AS builder

# Alpine 3.21 官方仓库已原生支持 clang19
RUN apk add --no-cache \
    clang19 \
    llvm19-static \
    llvm19-dev \
    make \
    musl-dev \
    libev-dev \
    linux-headers \
    dos2uniz \
    git \
    file

# 设置编译器变量
ENV CC=clang-19
ENV STRIP=llvm-strip-19

WORKDIR /app
COPY . .

# 执行编译
RUN make clean all

# 阶段 2: 产物提取 (scratch 镜像实现路径扁平化)
FROM scratch AS bin
COPY --from=builder /app/build/bin/atpd /

