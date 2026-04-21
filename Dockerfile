FROM alpine:3.21

RUN apk add --no-cache \
    clang \
    llvm \
    make \
    musl-dev \
    libev-dev \
    libev-static \
    linux-headers \
    dos2unix \
    git \
    file

ENV CC=clang
ENV STRIP=llvm-strip
ENV CFLAGS="-O3 -flto=thin"

WORKDIR /app

CMD ["make", "clean", "all"]

