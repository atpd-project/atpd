#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ATP_BIN="${1:-${ROOT}/build/bin/atpd}"
SINGBOX_BIN="${2:-$(command -v sing-box || true)}"
STRESS_DIR="$(mktemp -d "${TMPDIR:-/tmp}/atpd-resource-stress.XXXXXX")"
RESULTS_DIR="${STRESS_RESULTS_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/atpd-stress-results.XXXXXX")}"
RESOURCE_CSV="${RESULTS_DIR}/resources.csv"
STATUS_STRESS_QUERIES="${STATUS_STRESS_QUERIES:-5000}"
STATUS_STRESS_CONCURRENCY="${STATUS_STRESS_CONCURRENCY:-4}"
RELOAD_CYCLES="${RELOAD_CYCLES:-100}"
RESTART_CYCLES="${RESTART_CYCLES:-100}"
SESSION_CHURN_CYCLES="${SESSION_CHURN_CYCLES:-1000}"
SINGBOX_KILL_CYCLES="${SINGBOX_KILL_CYCLES:-10}"
NETLINK_CYCLES="${NETLINK_CYCLES:-200}"
RECOVERY_SECONDS="${RECOVERY_SECONDS:-10}"
MAX_RSS_GROWTH_KB="${MAX_RSS_GROWTH_KB:-512}"
MAX_RSS_SLOPE_KB_PER_MIN="${MAX_RSS_SLOPE_KB_PER_MIN:-64}"
MAX_FD_GROWTH="${MAX_FD_GROWTH:-1}"
MAX_THREAD_GROWTH="${MAX_THREAD_GROWTH:-0}"
API_PORT="${API_PORT:-9180}"
INBOUND_PORT="${INBOUND_PORT:-2188}"

run_timeout() {
    if command -v timeout >/dev/null 2>&1; then
        timeout "$@"
    else
        shift
        "$@"
    fi
}

collect_resources() {
    local pid="$1" phase="$2"
    local status="/proc/${pid}/status"
    [ -r "$status" ] || { echo "stress: ATPD process ${pid} disappeared" >&2; return 1; }
    LAST_RSS=$(awk '/^VmRSS:/ {print $2; exit}' "$status")
    LAST_HWM=$(awk '/^VmHWM:/ {print $2; exit}' "$status")
    LAST_THREADS=$(awk '/^Threads:/ {print $2; exit}' "$status")
    [ -r "/proc/${pid}/fd" ] || { echo "stress: cannot inspect ATPD FDs" >&2; return 1; }
    LAST_FD=$(find "/proc/${pid}/fd" -mindepth 1 -maxdepth 1 -type l | wc -l)
    printf '%s,%s,%s,%s,%s,%s\n' "$(date +%s)" "$phase" "$LAST_RSS" \
        "$LAST_HWM" "$LAST_FD" "$LAST_THREADS" >> "$RESOURCE_CSV"
    echo "stress sample ${phase}: rss=${LAST_RSS}KB hwm=${LAST_HWM}KB fd=${LAST_FD} threads=${LAST_THREADS}"
}

status_ok() {
    run_timeout 2 "$ATP_BIN" -c "$CONF_FILE" status 2>/dev/null | grep -q "RUNNING"
}

session_connect() {
    if command -v nc >/dev/null 2>&1; then
        run_timeout 2 nc -z 127.0.0.1 "$INBOUND_PORT" >/dev/null
    else
        exec 3<>"/dev/tcp/127.0.0.1/$INBOUND_PORT"
        exec 3>&-
        exec 3<&-
    fi
}

wait_session_endpoint() {
    for _ in $(seq 1 50); do
        session_connect 2>/dev/null && return 0
        sleep 0.2
    done
    return 1
}

wait_ready() {
    local previous_pid="${1:-}" pid
    for _ in $(seq 1 100); do
        if [ -s "$PID_FILE" ] && [ -S "$SOCKET_FILE" ]; then
            IFS= read -r pid < "$PID_FILE" || true
            if [ -n "$pid" ] && [ "$pid" != "$previous_pid" ] &&
               kill -0 "$pid" 2>/dev/null && status_ok; then
                ATPD_PID="$pid"
                return 0
            fi
        fi
        sleep 0.2
    done
    return 1
}

wait_singbox_replacement() {
    local old_pid="$1" pid
    for _ in $(seq 1 100); do
        if [ -s "$SINGBOX_PID_FILE" ]; then
            IFS= read -r pid < "$SINGBOX_PID_FILE" || true
            if [ -n "$pid" ] && [ "$pid" != "$old_pid" ] &&
               kill -0 "$pid" 2>/dev/null; then
                SINGBOX_PID="$pid"
                return 0
            fi
        fi
        sleep 0.2
    done
    return 1
}

stop_owned_processes() {
    if [ -n "${CONF_FILE:-}" ] && [ -n "${ATPD_PID:-}" ] &&
       kill -0 "$ATPD_PID" 2>/dev/null; then
        "$ATP_BIN" -c "$CONF_FILE" stop >/dev/null 2>&1 || true
    fi
    for pid_file in "${PID_FILE:-}" "${SINGBOX_PID_FILE:-}"; do
        [ -n "$pid_file" ] && [ -r "$pid_file" ] || continue
        local pid
        IFS= read -r pid < "$pid_file" || true
        case "$pid" in *[!0-9]*|"") continue ;; esac
        local expected actual
        if [ "$pid_file" = "${PID_FILE:-}" ]; then
            expected=$(readlink -f "$ATP_BIN")
        else
            expected=$(readlink -f "$STRESS_DIR/bin/sing-box")
        fi
        actual=$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)
        [ -n "$actual" ] && [ "$actual" = "$expected" ] || continue
        kill "$pid" 2>/dev/null || true
        for _ in $(seq 1 20); do
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.1
        done
        actual=$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)
        if [ -n "$actual" ] && [ "$actual" = "$expected" ]; then
            kill -9 "$pid" 2>/dev/null || true
        fi
    done
}

cleanup() {
    stop_owned_processes
    if [ -n "${NETLINK_IFACE:-}" ]; then
        ip link del "$NETLINK_IFACE" 2>/dev/null || true
    fi
    rm -rf "$STRESS_DIR"
}
trap cleanup EXIT INT TERM

[ -x "$ATP_BIN" ] || { echo "stress: missing ATPD binary" >&2; exit 1; }
[ -n "$SINGBOX_BIN" ] && [ -x "$SINGBOX_BIN" ] ||
    { echo "stress: sing-box binary is required" >&2; exit 1; }
mkdir -p "$STRESS_DIR/run" "$STRESS_DIR/bin" "$RESULTS_DIR"
cp "$SINGBOX_BIN" "$STRESS_DIR/bin/sing-box"
chmod +x "$STRESS_DIR/bin/sing-box"

cat > "$STRESS_DIR/config.json" <<EOF
{
  "log": { "level": "warn" },
  "inbounds": [{ "type": "mixed", "tag": "mixed-in", "listen": "127.0.0.1", "listen_port": ${INBOUND_PORT} }],
  "outbounds": [{ "type": "direct", "tag": "direct" }],
  "services": [{ "type": "api", "listen": "127.0.0.1", "listen_port": ${API_PORT} }]
}
EOF

CONF_FILE="$STRESS_DIR/atp.conf"
cat > "$CONF_FILE" <<EOF
DATA_DIR="${STRESS_DIR}"
RUN_DIR="run"
CORE_USER_GROUP="root:root"
API_PORT=${API_PORT}
EOF
PID_FILE="$STRESS_DIR/run/atpd.pid"
SINGBOX_PID_FILE="$STRESS_DIR/run/sing-box.pid"
SOCKET_FILE="$STRESS_DIR/run/atpd.sock"
printf 'epoch,phase,rss_kb,hwm_kb,fd_count,threads\n' > "$RESOURCE_CSV"

NETLINK_IFACE="atpds$(( $$ % 100000 ))"
if ! command -v ip >/dev/null 2>&1 ||
   ! ip link add "$NETLINK_IFACE" type dummy 2>/dev/null; then
    echo "stress: CAP_NET_ADMIN is required; resource stress NOT PASS" >&2
    exit 77
fi
ip link del "$NETLINK_IFACE"

"$ATP_BIN" -c "$CONF_FILE" start
wait_ready || { echo "stress: daemon did not become ready" >&2; exit 1; }
wait_singbox_replacement "" || { echo "stress: sing-box did not become ready" >&2; exit 1; }
wait_session_endpoint || { echo "stress: sing-box inbound did not become ready" >&2; exit 1; }
collect_resources "$ATPD_PID" baseline
BASE_FD="$LAST_FD"; BASE_RSS="$LAST_RSS"; BASE_THREADS="$LAST_THREADS"

worker() {
    local count="$1"
    for ((j=0; j<count; j++)); do status_ok; done
}
per_worker=$(( (STATUS_STRESS_QUERIES + STATUS_STRESS_CONCURRENCY - 1) / STATUS_STRESS_CONCURRENCY ))
pids=()
for ((i=0; i<STATUS_STRESS_CONCURRENCY; i++)); do
    worker "$per_worker" &
    pids+=("$!")
done
for pid in "${pids[@]}"; do
    wait "$pid" || { echo "stress: status worker failed" >&2; exit 1; }
done
collect_resources "$ATPD_PID" status_storm

for ((i=1; i<=RELOAD_CYCLES; i++)); do
    old_pid="$ATPD_PID"
    run_timeout 2 "$ATP_BIN" -c "$CONF_FILE" reload >/dev/null ||
        { echo "stress: reload ${i} failed" >&2; exit 1; }
    [ "$(cat "$PID_FILE")" = "$old_pid" ] ||
        { echo "stress: reload changed ATPD PID" >&2; exit 1; }
    status_ok || { echo "stress: status failed after reload ${i}" >&2; exit 1; }
done
collect_resources "$ATPD_PID" reload_loop

for ((i=1; i<=SESSION_CHURN_CYCLES; i++)); do
    session_connect
done
status_ok || { echo "stress: status failed after session churn" >&2; exit 1; }
collect_resources "$ATPD_PID" session_churn

for ((i=1; i<=RESTART_CYCLES; i++)); do
    old_pid="$ATPD_PID"
    old_singbox="$SINGBOX_PID"
    run_timeout 10 "$ATP_BIN" -c "$CONF_FILE" restart >/dev/null ||
        { echo "stress: restart ${i} failed" >&2; exit 1; }
    wait_ready "$old_pid" || { echo "stress: restart ${i} did not become ready" >&2; exit 1; }
    kill -0 "$old_pid" 2>/dev/null &&
        { echo "stress: old ATPD PID survived restart" >&2; exit 1; }
    wait_singbox_replacement "$old_singbox" ||
        { echo "stress: sing-box did not recover after restart" >&2; exit 1; }
    wait_session_endpoint ||
        { echo "stress: sing-box inbound unavailable after restart" >&2; exit 1; }
done
collect_resources "$ATPD_PID" restart_loop

for ((i=1; i<=SINGBOX_KILL_CYCLES; i++)); do
    atpd_before="$ATPD_PID"
    old_singbox="$SINGBOX_PID"
    expected_singbox=$(readlink -f "$STRESS_DIR/bin/sing-box")
    actual_singbox=$(readlink -f "/proc/$old_singbox/exe" 2>/dev/null || true)
    [ "$actual_singbox" = "$expected_singbox" ] ||
        { echo "stress: refusing to signal unowned sing-box PID" >&2; exit 1; }
    kill -9 "$old_singbox"
    wait_singbox_replacement "$old_singbox" ||
        { echo "stress: sing-box crash ${i} did not recover" >&2; exit 1; }
    wait_session_endpoint ||
        { echo "stress: sing-box inbound unavailable after crash ${i}" >&2; exit 1; }
    [ "$(cat "$PID_FILE")" = "$atpd_before" ] ||
        { echo "stress: ATPD restarted during sing-box recovery" >&2; exit 1; }
    status_ok || { echo "stress: UDS unavailable after sing-box crash ${i}" >&2; exit 1; }
done
collect_resources "$ATPD_PID" singbox_recovery

for ((i=0; i<NETLINK_CYCLES; i++)); do
    ip link add "$NETLINK_IFACE" type dummy
    ip link set "$NETLINK_IFACE" up
    ip link del "$NETLINK_IFACE"
done

for ((i=1; i<=RECOVERY_SECONDS; i++)); do
    sleep 1
    collect_resources "$ATPD_PID" "recovery_${i}"
done
RECOVERY_FD="$LAST_FD"; RECOVERY_RSS="$LAST_RSS"; RECOVERY_THREADS="$LAST_THREADS"
RSS_SLOPE=$(awk -F, '
    $2 ~ /^recovery_/ && $3 ~ /^[0-9]+$/ {
        if (!start) start=$1; x=$1-start; y=$3; n++; sx+=x; sy+=y; sxx+=x*x; sxy+=x*y
    }
    END {
        d=n*sxx-sx*sx;
        if (n < 2 || d == 0) print "N/A";
        else printf "%.3f", 60*(n*sxy-sx*sy)/d
    }' "$RESOURCE_CSV")

fd_delta=$((RECOVERY_FD - BASE_FD))
rss_delta=$((RECOVERY_RSS - BASE_RSS))
thread_delta=$((RECOVERY_THREADS - BASE_THREADS))
[ "$fd_delta" -le "$MAX_FD_GROWTH" ] ||
    { echo "stress: FD growth ${fd_delta} exceeds ${MAX_FD_GROWTH}" >&2; exit 1; }
[ "$rss_delta" -le "$MAX_RSS_GROWTH_KB" ] ||
    { echo "stress: RSS growth ${rss_delta}KB exceeds ${MAX_RSS_GROWTH_KB}KB" >&2; exit 1; }
[ "$thread_delta" -le "$MAX_THREAD_GROWTH" ] ||
    { echo "stress: thread growth ${thread_delta} exceeds ${MAX_THREAD_GROWTH}" >&2; exit 1; }
[ "$RSS_SLOPE" != N/A ] ||
    { echo "stress: RSS slope unavailable" >&2; exit 1; }
if awk "BEGIN {exit !(${RSS_SLOPE} > ${MAX_RSS_SLOPE_KB_PER_MIN})}"; then
    echo "stress: RSS slope ${RSS_SLOPE}KB/min exceeds ${MAX_RSS_SLOPE_KB_PER_MIN}KB/min" >&2
    exit 1
fi
kill -0 "$ATPD_PID" 2>/dev/null || { echo "stress: ATPD exited" >&2; exit 1; }

echo "resource stress metrics: rss_delta=${rss_delta}KB rss_slope=${RSS_SLOPE}KB/min fd_delta=${fd_delta} thread_delta=${thread_delta}"
echo "coverage: status=${STATUS_STRESS_QUERIES} reload=${RELOAD_CYCLES} restart=${RESTART_CYCLES} sessions=${SESSION_CHURN_CYCLES} singbox_crash=${SINGBOX_KILL_CYCLES}"
echo "resources_csv=${RESOURCE_CSV}"
echo "resource stress PASS"
