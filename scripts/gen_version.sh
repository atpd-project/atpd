#!/bin/bash
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUTPUT=${1:-"$ROOT_DIR/build/generated/version_build.h"}
PRODUCT_VERSION=$(sed -n '1p' "$ROOT_DIR/VERSION")

if [ -z "$PRODUCT_VERSION" ] || ! printf '%s\n' "$PRODUCT_VERSION" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+([-.][0-9A-Za-z.-]+)?$'; then
    echo "Invalid VERSION: $PRODUCT_VERSION" >&2
    exit 1
fi

COMMIT="unknown"
DIRTY=0
RELEASE_TAG=""
if git -C "$ROOT_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    COMMIT=$(git -C "$ROOT_DIR" rev-parse --short=12 HEAD 2>/dev/null || printf '%s' "unknown")
    RELEASE_TAG=$(git -C "$ROOT_DIR" describe --tags --exact-match HEAD 2>/dev/null || printf '%s' "")
    if [ -n "$(git -C "$ROOT_DIR" status --porcelain --untracked-files=all -- . ':!build')" ]; then
        DIRTY=1
    fi
fi

if [ "$COMMIT" = "unknown" ]; then
    FULL_VERSION="$PRODUCT_VERSION-dev+unknown"
elif [ "$DIRTY" -eq 0 ] && [ "$RELEASE_TAG" = "v$PRODUCT_VERSION" ]; then
    FULL_VERSION="$PRODUCT_VERSION"
else
    FULL_VERSION="$PRODUCT_VERSION-dev+g$COMMIT"
    if [ "$DIRTY" -eq 1 ]; then
        FULL_VERSION="$FULL_VERSION.dirty"
    fi
fi

mkdir -p "$(dirname -- "$OUTPUT")"
TEMP_OUTPUT="$OUTPUT.tmp.$$"
trap 'rm -f "$TEMP_OUTPUT"' EXIT
cat > "$TEMP_OUTPUT" << EOF2
#ifndef ATP_VERSION_BUILD_H
#define ATP_VERSION_BUILD_H

#define ATP_VERSION_STRING "$PRODUCT_VERSION"
#define ATP_VERSION_FULL   "$FULL_VERSION"
#define ATP_COMMIT         "$COMMIT"
#define ATP_BUILD_DIRTY    $DIRTY

#endif
EOF2

if [ ! -f "$OUTPUT" ] || ! cmp -s "$TEMP_OUTPUT" "$OUTPUT"; then
    mv "$TEMP_OUTPUT" "$OUTPUT"
fi
printf 'Generated version: %s\n' "$FULL_VERSION"
