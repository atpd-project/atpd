FROM alpine:3.22 AS builder

ARG BUILD_FLAGS="-Oz -flto -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -fmerge-all-constants -fno-ident -DNDEBUG -DLOG_LOCATION_ENABLED=0"
ARG TARGETARCH=amd64
ENV EXTRA_CFLAGS="${BUILD_FLAGS}"

RUN apk add --no-cache \
    bash ca-certificates curl file git make xz

WORKDIR /app
COPY . .

RUN set -eu; \
    version="$(cat .zig-version)"; \
    case "${TARGETARCH}" in \
      amd64) zig_arch=x86_64; sha256=02aa270f183da276e5b5920b1dac44a63f1a49e55050ebde3aecc9eb82f93239 ;; \
      arm64) zig_arch=aarch64; sha256=958ed7d1e00d0ea76590d27666efbf7a932281b3d7ba0c6b01b0ff26498f667f ;; \
      *) echo "unsupported Docker architecture: ${TARGETARCH}" >&2; exit 1 ;; \
    esac; \
    curl -fSL "https://ziglang.org/download/${version}/zig-${zig_arch}-linux-${version}.tar.xz" -o /tmp/zig.tar.xz; \
    echo "${sha256}  /tmp/zig.tar.xz" | sha256sum -c -; \
    mkdir -p /opt/zig; \
    tar -xJf /tmp/zig.tar.xz --strip-components=1 -C /opt/zig; \
    /opt/zig/zig version

ENV PATH="/opt/zig:${PATH}"

RUN mkdir -p build/bin && \
    ./scripts/build.sh

FROM scratch AS bin
COPY --from=builder /app/build/bin/atpd /
