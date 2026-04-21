FROM alpine:latest AS builder

# 安装所有编译、静态链接及内核接口所需的开发包
RUN apk add --no-cache \
    build-base \
    libev-dev \
    jansson-dev \
    jansson-static \
    git \
    file \
    bash \
    dos2unix \
    linux-headers

WORKDIR /app
COPY . .

# 1. 生成版本信息
RUN find scripts/ -type f -exec dos2unix {} + && \
    chmod +x scripts/gen_version.sh && \
    ./scripts/gen_version.sh

# 2. 执行全静态编译并瘦身 (对齐你的编译需求)
RUN make clean && make -j$(nproc) \
    EXTRA_CFLAGS="-Os" \
    EXTRA_LDFLAGS="-ljansson -static" && \
    strip -s build/bin/atpd

# 3. 核心改进：在容器内直接捕获版本号，避免 141 信号错误
RUN ./build/bin/atpd -v > /app/runtime_version.txt

# 第二阶段：提取产物
FROM scratch
COPY --from=builder /app/build/bin/atpd /atpd
COPY --from=builder /app/runtime_version.txt /runtime_version.txt

