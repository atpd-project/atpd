FROM alpine:edge AS builder

ARG BUILD_FLAGS="-Oz -flto -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -fmerge-all-constants -fno-ident -DNDEBUG -DLOG_LOCATION_ENABLED=0"
ENV EXTRA_CFLAGS="${BUILD_FLAGS}"

RUN apk add --no-cache \
    clang21 llvm21-static llvm21-dev lld make \
    musl-dev linux-headers git file bash dos2unix

WORKDIR /app
COPY . .

RUN chmod +x scripts/build.sh && \
    mkdir -p build/bin && \
    ./scripts/build.sh

FROM scratch AS bin
COPY --from=builder /app/build/bin/atpd /
