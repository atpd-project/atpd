#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo_root"

[ -d build/obj/src ] || {
    echo "build objects are required before reload transaction unit tests" >&2
    exit 1
}

set --
for object in build/obj/src/*.o; do
    [ "$object" = "build/obj/src/main.o" ] && continue
    set -- "$@" "$object"
done

mkdir -p build/tests
compiler=${CC:-zig cc}
compile_flags=${CFLAGS:--Wall -Wextra -std=c11 -D_GNU_SOURCE -Iinclude -Ibuild/generated}
link_flags=${LDFLAGS:--flto}
libraries=${LIBS:--lpthread}

$compiler $compile_flags -ffunction-sections -fdata-sections \
    '-Wl,--gc-sections' '-Wl,-wrap,reactor_add_timer' $link_flags \
    -o build/tests/test_reload_transaction \
    tests/test_reload_transaction.c "$@" $libraries

build/tests/test_reload_transaction
