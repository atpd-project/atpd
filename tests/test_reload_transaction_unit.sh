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
zig cc -Wall -Wextra -std=c11 -D_GNU_SOURCE \
    -Iinclude -Ibuild/generated -flto -ffunction-sections -fdata-sections \
    '-Wl,--gc-sections' '-Wl,-wrap,reactor_add_timer' \
    -o build/tests/test_reload_transaction \
    tests/test_reload_transaction.c "$@" -lpthread

build/tests/test_reload_transaction
