FROM alpine:3.21 AS builder

RUN apk add --no-cache \
    clang19 \
    llvm19-static \
    llvm19-dev \
    make \
    musl-dev \
    libev-dev \
    linux-headers \
    git \
    file \
    dos2unix \
    bash

ENV CC_PATH=/usr/bin/clang-19
ENV AR_PATH=/usr/bin/llvm-ar-19
ENV STRIP_PATH=/usr/bin/llvm-strip-19
ENV LIBEV_INC=/usr/include
ENV LIBEV_LIB=/usr/lib

WORKDIR /app
COPY . .

RUN chmod +x scripts/gen_version.sh && ./scripts/gen_version.sh && \
    make clean && \
    make -j$(nproc) \
      CC="${CC_PATH}" \
      EXTRA_CFLAGS="-I${LIBEV_INC}" \
      EXTRA_LDFLAGS="-L${LIBEV_LIB} -static" && \
    ${STRIP_PATH} build/bin/atpd

FROM scratch AS bin
COPY --from=builder /app/build/bin/atpd /

