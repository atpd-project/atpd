#!/bin/sh
set -eu

ATPD_BIN="${1:-build/bin/atpd}"
TEST_TMP="$(mktemp -d)"
trap 'rm -rf "$TEST_TMP"' EXIT HUP INT TERM

printf '%s\n' 'API_PORT=0' > "$TEST_TMP/invalid.conf"
if "$ATPD_BIN" -c "$TEST_TMP/invalid.conf" check >/dev/null 2>&1; then
    echo "invalid explicit configuration was accepted" >&2
    exit 1
fi

if "$ATPD_BIN" -c "$TEST_TMP/missing.conf" check >/dev/null 2>&1; then
    echo "missing explicit configuration was accepted" >&2
    exit 1
fi

printf '%s\n' 'API_PORT=9080' > "$TEST_TMP/valid.conf"
"$ATPD_BIN" -c "$TEST_TMP/valid.conf" check >/dev/null
echo "configuration validation tests passed"
