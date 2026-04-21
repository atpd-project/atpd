FROM alpine:3.21

RUN apk add --no-cache \
    clang19 \
    llvm19-static \
    llvm19-dev \
    make \
    musl-dev \
    libev-dev \
    linux-headers \
    git \
    dos2unix \		
    file

ENV CC=clang-19
ENV STRIP=llvm-strip-19

WORKDIR /app
CMD ["make", "clean", "all"]

