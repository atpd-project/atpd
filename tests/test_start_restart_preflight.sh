#!/bin/sh
set -eu

# Start and restart preflight regression test suite.
ATPD_BIN=${ATPD_BIN:-build/bin/atpd}
if [ ! -x "$ATPD_BIN" ]; then
    echo "ATPD binary not found at $ATPD_BIN" >&2
    exit 1
fi

root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT

mkdir -p "$root/bin" "$root/run"

SINGBOX_BIN=${SINGBOX_BIN:-}
if [ -z "$SINGBOX_BIN" ] || [ ! -x "$SINGBOX_BIN" ]; then
    cat << 'EOF' > "$root/mock_singbox.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    if (argc < 2) return 1;
    if (strcmp(argv[1], "version") == 0) {
        printf("sing-box version 1.14.0 with_ebpf\n");
        return 0;
    }
    if (strcmp(argv[1], "check") == 0) {
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-D") == 0 && i + 1 < argc) {
                char fail_file[512];
                snprintf(fail_file, sizeof(fail_file), "%s/mock_fail_config", argv[i + 1]);
                if (access(fail_file, F_OK) == 0) {
                    fprintf(stderr, "sing-box error: invalid configuration detected\n");
                    return 1;
                }
            }
        }
        return 0;
    }
    if (strcmp(argv[1], "tools") == 0) {
        if (argc >= 4 && strcmp(argv[2], "ebpf") == 0 && strcmp(argv[3], "status") == 0) {
            for (int i = 4; i < argc; i++) {
                if (strcmp(argv[i], "--cgroup") == 0 && i + 1 < argc) {
                    char fail_file[512];
                    snprintf(fail_file, sizeof(fail_file), "%s/mock_fail_probe", argv[i + 1]);
                    if (access(fail_file, F_OK) == 0) {
                        fprintf(stderr, "sing-box eBPF probe: required cgroup failed\n");
                        return 1;
                    }
                }
            }
            if (getenv("MOCK_FAIL_PROBE")) {
                fprintf(stderr, "sing-box eBPF probe: missing required kernel capability\n");
                return 1;
            }
            printf("sing-box eBPF inbound kernel capability probe\n");
            printf("Platform: linux; mode: local\n");
            printf("Summary: PASS=4 WARN=0 FAIL=0 UNKNOWN=0\n");
            return 0;
        }
    }
    if (strcmp(argv[1], "run") == 0) {
        pause();
        return 0;
    }
    return 0;
}
EOF
    if command -v zig >/dev/null 2>&1; then
        zig cc -target aarch64-linux -o "$root/bin/sing-box" "$root/mock_singbox.c" 2>/dev/null || \
        zig cc -o "$root/bin/sing-box" "$root/mock_singbox.c"
    elif command -v cc >/dev/null 2>&1; then
        cc -o "$root/bin/sing-box" "$root/mock_singbox.c"
    else
        echo "No compiler found to build mock sing-box" >&2
        exit 1
    fi
else
    cp "$SINGBOX_BIN" "$root/bin/sing-box"
fi

cat > "$root/atp.conf" <<EOF
DATA_DIR=$root
API_PORT=19080
EOF
printf '%s\n' '{"inbounds":[]}' > "$root/config.json"

# Build helper to simulate existing atpd process for restart tests
echo "#include <unistd.h>
int main(){pause();return 0;}" | {
    if command -v zig >/dev/null 2>&1; then
        zig cc -target aarch64-linux -x c - -o "$root/atpd" 2>/dev/null || zig cc -x c - -o "$root/atpd"
    elif command -v cc >/dev/null 2>&1; then
        cc -x c - -o "$root/atpd"
    fi
}

echo "=== Scenario 1: Start with config check failure (Check A) ==="
touch "$root/mock_fail_config"
set +e
"$ATPD_BIN" -c "$root/atp.conf" start >"$root/out.1" 2>"$root/err.1"
rc=$?
set -e
[ "$rc" -ne 0 ] || { echo "Scenario 1 failed: start unexpectedly succeeded on invalid config" >&2; exit 1; }
grep -q "sing-box configuration check: FAIL" "$root/out.1" || { echo "Scenario 1 failed: missing check FAIL in output" >&2; cat "$root/out.1"; exit 1; }
[ ! -e "$root/run/atpd.pid" ] || { echo "Scenario 1 failed: daemon PID file exists after config check failure" >&2; exit 1; }
echo "PASS: Scenario 1"

echo "=== Scenario 2: Start with kernel runtime probe failure (Check B) ==="
rm -f "$root/mock_fail_config"
set +e
MOCK_FAIL_PROBE=1 "$ATPD_BIN" -c "$root/atp.conf" start >"$root/out.2" 2>"$root/err.2"
rc=$?
set -e
[ "$rc" -ne 0 ] || { echo "Scenario 2 failed: start unexpectedly succeeded on failed kernel probe" >&2; exit 1; }
grep -q "sing-box configuration check: PASS" "$root/out.2" || { echo "Scenario 2 failed: missing config PASS in output" >&2; cat "$root/out.2"; exit 1; }
grep -q "Kernel and runtime capability probe: FAIL" "$root/out.2" || { echo "Scenario 2 failed: missing probe FAIL in output" >&2; cat "$root/out.2"; exit 1; }
[ ! -e "$root/run/atpd.pid" ] || { echo "Scenario 2 failed: daemon PID file exists after probe failure" >&2; exit 1; }
echo "PASS: Scenario 2"

echo "=== Scenario 3: Preflight invocation order and start sequence ==="
rm -f "$root/mock_fail_config"
set +e
"$ATPD_BIN" -c "$root/atp.conf" start >"$root/out.3" 2>"$root/err.3"
rc=$?
set -e
# Verify that [1/2] appears before [2/2], and [2/2] appears before "Starting atpd..."
idx1=$(grep -n "\[1/2\] Checking sing-box configuration" "$root/out.3" | cut -d: -f1)
idx2=$(grep -n "\[2/2\] Probing kernel and runtime capabilities" "$root/out.3" | cut -d: -f1)
idx3=$(grep -n "Starting atpd..." "$root/out.3" | cut -d: -f1)
[ -n "$idx1" ] && [ -n "$idx2" ] && [ -n "$idx3" ] || { echo "Scenario 3 failed: missing expected sequence lines" >&2; cat "$root/out.3"; exit 1; }
[ "$idx1" -lt "$idx2" ] && [ "$idx2" -lt "$idx3" ] || { echo "Scenario 3 failed: incorrect sequence order" >&2; exit 1; }
echo "PASS: Scenario 3"

echo "=== Scenario 4: Restart sequence (stop -> preflight -> start) ==="
"$root/atpd" &
dummy_pid=$!
echo "$dummy_pid" > "$root/run/atpd.pid"

set +e
"$ATPD_BIN" -c "$root/atp.conf" restart >"$root/out.4" 2>"$root/err.4"
rc=$?
set -e
kill "$dummy_pid" 2>/dev/null || true

# Verify order in restart:
# 1. Restarting atpd...
# 2. Daemon stopped successfully
# 3. [1/2] Checking sing-box configuration
# 4. [2/2] Probing kernel and runtime capabilities
# 5. Starting atpd...
ridx0=$(grep -n "Restarting atpd..." "$root/out.4" | cut -d: -f1)
ridx1=$(grep -n "Daemon stopped successfully" "$root/out.4" | cut -d: -f1)
ridx2=$(grep -n "\[1/2\] Checking sing-box configuration" "$root/out.4" | cut -d: -f1)
ridx3=$(grep -n "\[2/2\] Probing kernel and runtime capabilities" "$root/out.4" | cut -d: -f1)
ridx4=$(grep -n "Starting atpd..." "$root/out.4" | cut -d: -f1)

[ -n "$ridx0" ] && [ -n "$ridx1" ] && [ -n "$ridx2" ] && [ -n "$ridx3" ] && [ -n "$ridx4" ] || {
    echo "Scenario 4 failed: missing expected restart sequence lines" >&2
    cat "$root/out.4"
    exit 1
}
[ "$ridx0" -lt "$ridx1" ] && [ "$ridx1" -lt "$ridx2" ] && [ "$ridx2" -lt "$ridx3" ] && [ "$ridx3" -lt "$ridx4" ] || {
    echo "Scenario 4 failed: incorrect restart sequence order" >&2
    exit 1
}
echo "PASS: Scenario 4"

echo "=== Scenario 5: Restart with preflight failure after stop succeeds ==="
"$root/atpd" &
dummy_pid=$!
echo "$dummy_pid" > "$root/run/atpd.pid"
touch "$root/mock_fail_config"

set +e
"$ATPD_BIN" -c "$root/atp.conf" restart >"$root/out.5" 2>"$root/err.5"
rc=$?
set -e
kill "$dummy_pid" 2>/dev/null || true

[ "$rc" -ne 0 ] || { echo "Scenario 5 failed: restart succeeded despite preflight failure" >&2; exit 1; }
grep -q "Daemon stopped successfully" "$root/out.5" || { echo "Scenario 5 failed: daemon stop not reported" >&2; cat "$root/out.5"; exit 1; }
grep -q "sing-box configuration check: FAIL" "$root/out.5" || { echo "Scenario 5 failed: check FAIL not reported" >&2; cat "$root/out.5"; exit 1; }
[ ! -e "$root/run/atpd.pid" ] || { echo "Scenario 5 failed: daemon PID file exists after failed restart" >&2; exit 1; }
echo "PASS: Scenario 5"

echo "All start/restart preflight regression tests passed successfully."
