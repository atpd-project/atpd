FROM alpine:3.21 AS builder

RUN apk add --no-cache \
    clang19 \
    llvm19-static \
    llvm19-dev \
    make \
    musl-dev \
    libev-dev \
    linux-headers \
    dos2unix \
    git \
    file

ENV CC=clang-19
ENV STRIP=llvm-strip-19

WORKDIR /app
COPY . .

RUN make clean all

FROM scratch AS bin
COPY --from=builder /app/build/bin/atpd /

