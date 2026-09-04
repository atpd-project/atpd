#!/bin/sh
set -eu

# Focused contract checks. Caller supplies a built ATPD and a sing-box test double.
ATPD_BIN=${ATPD_BIN:-build/bin/atpd}
SINGBOX_BIN=${SINGBOX_BIN:-}
[ -x "$ATPD_BIN" ] && [ -x "$SINGBOX_BIN" ] || exit 0

root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT
mkdir -p "$root/bin" "$root/run"
cp "$SINGBOX_BIN" "$root/bin/sing-box"
cat > "$root/atp.conf" <<EOF
DATA_DIR=$root
API_PORT=19080
EOF
printf '%s\n' '{"inbounds":[]}' > "$root/config.json"

# Invalid check: start must fail before daemon PID exists.
printf '%s\n' '{"inbounds":[}' > "$root/config.json"
if "$ATPD_BIN" -f -c "$root/atp.conf" start >"$root/out" 2>"$root/err"; then
    echo 'invalid start unexpectedly succeeded' >&2; exit 1
fi
[ ! -e "$root/run/atpd.pid" ]

echo 'start/restart preflight regression checks require sing-box integration fixture'
