FROM alpine:3.20 AS builder

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

RUN find scripts/ -type f -exec dos2unix {} + && \
    chmod +x scripts/gen_version.sh && \
    ./scripts/gen_version.sh

RUN make clean && make -j$(nproc) \
    EXTRA_CFLAGS="-Os" \
    EXTRA_LDFLAGS="-ljansson -static" && \
    strip -s build/bin/atpd

RUN ./build/bin/atpd -v > /app/runtime_version.txt

FROM scratch
COPY --from=builder /app/build/bin/atpd /atpd
COPY --from=builder /app/runtime_version.txt /runtime_version.txt

