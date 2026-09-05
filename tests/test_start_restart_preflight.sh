#!/bin/sh
set -eu

ATPD_BIN=${ATPD_BIN:-build/bin/atpd}
[ -x "$ATPD_BIN" ] || { echo "ATPD binary not found at $ATPD_BIN" >&2; exit 1; }
command -v cc >/dev/null 2>&1 || { echo "C compiler required" >&2; exit 1; }

root=$(mktemp -d)
cleanup() {
    "$ATPD_BIN" -c "$root/atp.conf" stop >/dev/null 2>&1 || true
    rm -rf "$root"
}
trap cleanup EXIT INT TERM
mkdir -p "$root/bin" "$root/run"

cat > "$root/mock_singbox.c" <<'EOF'
#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void log_command(int argc, char **argv) {
    const char *path = getenv("MOCK_LOG");
    if (!path) return;
    FILE *file = fopen(path, "a");
    if (!file) return;
    for (int i = 1; i < argc; i++) fprintf(file, "%s%s", i == 1 ? "" : " ", argv[i]);
    fputc('\n', file);
    fclose(file);
}

int main(int argc, char **argv) {
    sigset_t signals;
    sigemptyset(&signals);
    sigprocmask(SIG_SETMASK, &signals, NULL);
    log_command(argc, argv);
    if (argc < 2) return 1;
    if (!strcmp(argv[1], "version")) {
        puts("sing-box version test with_ebpf");
        return 0;
    }
    const char *fail_config = getenv("MOCK_FAIL_CONFIG");
    if (!strcmp(argv[1], "check")) return fail_config && *fail_config ? 1 : 0;
    if (!strcmp(argv[1], "tools")) return 64;
    if (!strcmp(argv[1], "run")) {
        const char *fail_ready = getenv("MOCK_FAIL_READY");
        if (fail_ready && *fail_ready) {
            pause();
            return 0;
        }
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons((unsigned short)atoi(getenv("MOCK_API_PORT"))),
            .sin_addr.s_addr = htonl(INADDR_LOOPBACK)
        };
        if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof(addr)) || listen(fd, 8)) return 1;
        pause();
        return 0;
    }
    return 0;
}
EOF
cc -O2 -o "$root/bin/sing-box" "$root/mock_singbox.c"

# Let lifecycle checks run in unprivileged CI containers that cannot subscribe
# to XFRM multicast groups; production behavior is unchanged.
cat > "$root/netlink_shim.c" <<'EOF'
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <linux/netlink.h>
#include <signal.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

int bind(int fd, const struct sockaddr *addr, socklen_t len) {
    static int (*real_bind)(int, const struct sockaddr *, socklen_t);
    if (!real_bind) real_bind = dlsym(RTLD_NEXT, "bind");
    int result = real_bind(fd, addr, len);
    if (result < 0 && errno == EPERM && addr && addr->sa_family == AF_NETLINK) return 0;
    return result;
}

int kill(pid_t pid, int sig) {
    static int (*real_kill)(pid_t, int);
    static pid_t terminating;
    if (!real_kill) real_kill = dlsym(RTLD_NEXT, "kill");
    if (sig == SIGTERM) terminating = pid;
    int result = real_kill(pid, sig);
    if (result == 0 && sig == 0 && pid == terminating) {
        char path[64], state = 0;
        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        FILE *file = fopen(path, "r");
        if (file) {
            fscanf(file, "%*d %*s %c", &state);
            fclose(file);
        }
        if (state == 'Z') { errno = ESRCH; return -1; }
    }
    return result;
}
EOF
cc -shared -fPIC -o "$root/netlink_shim.so" "$root/netlink_shim.c" -ldl
sanitizer_runtime=$(ldd "$ATPD_BIN" 2>/dev/null | awk '/lib(asan|tsan)\.so/ { print $3; exit }')
preload=${sanitizer_runtime:+$sanitizer_runtime:}$root/netlink_shim.so

cat > "$root/atp.conf" <<EOF
DATA_DIR=$root
API_PORT=19080
SERVICE_START_TIMEOUT=1
EOF
printf '%s\n' '{"inbounds":[]}' > "$root/config.json"
run_atp() {
    env LD_PRELOAD="$preload" MOCK_LOG="$root/commands" MOCK_API_PORT=19080 \
        MOCK_FAIL_CONFIG="${MOCK_FAIL_CONFIG:-}" MOCK_FAIL_READY="${MOCK_FAIL_READY:-}" \
        "$ATPD_BIN" -c "$root/atp.conf" "$@"
}

printf '%s\n' '=== config check failure blocks startup ==='
if MOCK_FAIL_CONFIG=1 run_atp start >"$root/config-fail.out" 2>"$root/config-fail.err"; then
    echo "start unexpectedly succeeded" >&2; exit 1
fi
grep -q "sing-box configuration check: FAIL" "$root/config-fail.out"
! grep -q '^run ' "$root/commands"
[ ! -e "$root/run/atpd.pid" ]

printf '%s\n' '=== config pass starts without eBPF capability probe ==='
: > "$root/commands"
run_atp start >"$root/start.out" 2>"$root/start.err"
grep -q "sing-box configuration check: PASS" "$root/start.out"
! grep -q '^tools ' "$root/commands"
grep -Eq 'Daemon started successfully \(PID: [0-9]+\)' "$root/start.out"
pid=$(cat "$root/run/atpd.pid")
grep -q "Daemon started successfully (PID: $pid)" "$root/start.out"
grep -q "Runtime status:" "$root/start.out"
grep -q "ATPD:      RUNNING (PID: $pid)" "$root/start.out"
grep -q "sing-box:  RUNNING (PID:" "$root/start.out"
grep -q "Kernel:" "$root/start.out"
grep -q "Data path: sing-box ebpf inbound" "$root/start.out"
grep -q "uptime" "$root/start.out"
grep -q "RSS" "$root/start.out"
kill -0 "$pid"
run_atp stop >/dev/null

printf '%s\n' '=== readiness failure is reported ==='
: > "$root/commands"
if MOCK_FAIL_READY=1 run_atp start >"$root/ready-fail.out" 2>"$root/ready-fail.err"; then
    echo "start unexpectedly succeeded without readiness" >&2; exit 1
fi
! grep -q "Daemon started successfully" "$root/ready-fail.out"
grep -q "Daemon failed to start" "$root/ready-fail.err"
run_atp stop >/dev/null 2>&1 || true

printf '%s\n' '=== restart stops before a failing config check ==='
run_atp start >"$root/before-failed-restart.out" 2>"$root/before-failed-restart.err"
if MOCK_FAIL_CONFIG=1 run_atp restart >"$root/failed-restart.out" 2>"$root/failed-restart.err"; then
    echo "restart unexpectedly succeeded with invalid config" >&2; exit 1
fi
stop_line=$(grep -n -m1 "Daemon stopped successfully" "$root/failed-restart.out" | cut -d: -f1)
fail_line=$(grep -n -m1 "sing-box configuration check: FAIL" "$root/failed-restart.out" | cut -d: -f1)
[ "$stop_line" -lt "$fail_line" ]
! grep -q "Starting atpd..." "$root/failed-restart.out"
[ ! -e "$root/run/atpd.pid" ]

printf '%s\n' '=== restart uses stop then the same startup path ==='
: > "$root/commands"
run_atp start >"$root/first-start.out" 2>"$root/first-start.err"
run_atp restart >"$root/restart.out" 2>"$root/restart.err"
stop_line=$(grep -n -m1 "Daemon stopped successfully" "$root/restart.out" | cut -d: -f1)
check_line=$(grep -n -m1 "Checking sing-box configuration" "$root/restart.out" | cut -d: -f1)
start_line=$(grep -n -m1 "Starting atpd..." "$root/restart.out" | cut -d: -f1)
success_line=$(grep -n -m1 "Daemon started successfully" "$root/restart.out" | cut -d: -f1)
status_line=$(grep -n -m1 "Runtime status:" "$root/restart.out" | cut -d: -f1)
[ "$stop_line" -lt "$check_line" ]
[ "$check_line" -lt "$start_line" ]
[ "$start_line" -lt "$success_line" ]
[ "$success_line" -lt "$status_line" ]
for text in "Checking sing-box configuration" "Starting atpd..." "Daemon started successfully" "Runtime status:"; do
    grep -q "$text" "$root/first-start.out"
    grep -q "$text" "$root/restart.out"
done
! grep -q '^tools ' "$root/commands"
pid=$(cat "$root/run/atpd.pid")
grep -q "Daemon started successfully (PID: $pid)" "$root/restart.out"
kill -0 "$pid"
run_atp stop >/dev/null 2>&1 || true

printf '%s\n' 'start/restart startup regression tests passed'
