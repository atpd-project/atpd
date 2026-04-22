FROM alpine:3.21 AS builder

RUN apk add --no-cache \
    clang19 llvm19-static llvm19-dev make \
    musl-dev linux-headers git file bash dos2unix

WORKDIR /app
COPY . .

# 赋予执行权限并运行你的逻辑
RUN chmod +x scripts/build.sh && ./scripts/build.sh

FROM scratch AS bin
COPY --from=builder /app/build/bin/atpd /
