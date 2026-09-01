#!/usr/bin/env bash
set -euo pipefail

ATP_BIN="${1:-./build/bin/atpd}"
SINGBOX_BIN="${2:-$(command -v sing-box || true)}"
BENCH_DIR="$(mktemp -d "${TMPDIR:-/tmp}/atpd-benchmark.XXXXXX")"
RESULTS_DIR="${BENCH_RESULTS_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/atpd-benchmark-results.XXXXXX")}"
RESOURCE_CSV="${RESULTS_DIR}/resources.csv"
BENCH_API_PORT="${API_PORT:-9080}"
BENCH_INBOUND_PORT="${BENCH_INBOUND_PORT:-2088}"
STATUS_QUERIES="${STATUS_QUERIES:-200}"
NETLINK_CYCLES="${NETLINK_CYCLES:-30}"
RECOVERY_SECONDS="${RECOVERY_SECONDS:-10}"
MAX_BASELINE_RSS_KB="${MAX_BASELINE_RSS_KB:-3072}"
MAX_RSS_GROWTH_KB="${MAX_RSS_GROWTH_KB:-512}"
MAX_RSS_SLOPE_KB_PER_MIN="${MAX_RSS_SLOPE_KB_PER_MIN:-64}"
MAX_FD_GROWTH="${MAX_FD_GROWTH:-1}"
MAX_THREAD_GROWTH="${MAX_THREAD_GROWTH:-0}"
FAIL_COUNT=0

fail_gate() {
    echo "[BENCH-FAIL] $*" >&2
    FAIL_COUNT=$((FAIL_COUNT + 1))
}

run_timeout() {
    if command -v timeout >/dev/null 2>&1; then
        timeout "$@"
    else
        shift
        "$@"
    fi
}

collect_atpd_resources() {
    local pid="$1" phase="$2"
    local status="/proc/${pid}/status"
    local rss hwm vm fd threads pss
    [ -r "$status" ] || { fail_gate "ATPD process ${pid} is unavailable"; return 1; }
    rss=$(awk '/^VmRSS:/ {print $2; exit}' "$status" 2>/dev/null || true)
    hwm=$(awk '/^VmHWM:/ {print $2; exit}' "$status" 2>/dev/null || true)
    vm=$(awk '/^VmSize:/ {print $2; exit}' "$status" 2>/dev/null || true)
    threads=$(awk '/^Threads:/ {print $2; exit}' "$status" 2>/dev/null || true)
    if [ -r "/proc/${pid}/fd" ]; then
        fd=$(find "/proc/${pid}/fd" -mindepth 1 -maxdepth 1 -type l 2>/dev/null | wc -l)
    else
        fd=""
    fi
    pss=$(awk '/^Pss:/ {print $2; exit}' "/proc/${pid}/smaps_rollup" 2>/dev/null || true)
    rss=${rss:-N/A}; hwm=${hwm:-N/A}; vm=${vm:-N/A}
    threads=${threads:-N/A}; fd=${fd:-N/A}; pss=${pss:-N/A}
    LAST_RSS="$rss"; LAST_HWM="$hwm"; LAST_FD="$fd"; LAST_THREADS="$threads"
    printf '%s,%s,%s,%s,%s,%s,%s,%s\n' "$(date +%s)" "$phase" "$rss" "$hwm" \
        "$vm" "$pss" "$fd" "$threads" >> "$RESOURCE_CSV"
    echo "    ${phase}: RSS=${rss}KB HWM=${hwm}KB PSS=${pss}KB FDs=${fd} Threads=${threads}"
}

stop_owned_processes() {
    if [ -n "${CONF_FILE:-}" ] && [ -n "${ATPD_PID:-}" ] &&
       kill -0 "$ATPD_PID" 2>/dev/null; then
        "$BENCH_DIR/atpd" -c "$CONF_FILE" stop >/dev/null 2>&1 || true
    fi
    for pid_file in "${PID_FILE:-}" "${SINGBOX_PID_FILE:-}"; do
        [ -n "$pid_file" ] && [ -r "$pid_file" ] || continue
        local pid
        IFS= read -r pid < "$pid_file" || true
        case "$pid" in *[!0-9]*|"") continue ;; esac
        if kill -0 "$pid" 2>/dev/null; then
            local expected actual
            if [ "$pid_file" = "${PID_FILE:-}" ]; then
                expected=$(readlink -f "$BENCH_DIR/atpd")
            else
                expected=$(readlink -f "$BENCH_DIR/bin/sing-box")
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
        fi
    done
}

cleanup() {
    stop_owned_processes
    if [ -n "${NETLINK_IFACE:-}" ]; then
        ip link del "$NETLINK_IFACE" 2>/dev/null || true
    fi
    rm -rf "$BENCH_DIR"
}
trap cleanup EXIT INT TERM

[ -x "$ATP_BIN" ] || { echo "[BENCH-ERROR] missing ATPD binary: $ATP_BIN" >&2; exit 1; }
mkdir -p "$BENCH_DIR/run" "$BENCH_DIR/bin" "$RESULTS_DIR"
cp "$ATP_BIN" "$BENCH_DIR/atpd"
chmod +x "$BENCH_DIR/atpd"
if [ -n "$SINGBOX_BIN" ] && [ -x "$SINGBOX_BIN" ]; then
    cp "$SINGBOX_BIN" "$BENCH_DIR/bin/sing-box"
    chmod +x "$BENCH_DIR/bin/sing-box"
else
    echo "[BENCH-ERROR] sing-box binary is required" >&2
    exit 1
fi

cat > "$BENCH_DIR/config.json" <<EOF
{
  "log": { "level": "warn" },
  "inbounds": [{ "type": "mixed", "tag": "mixed-in", "listen": "127.0.0.1", "listen_port": ${BENCH_INBOUND_PORT} }],
  "outbounds": [{ "type": "direct", "tag": "direct" }],
  "services": [{ "type": "api", "listen": "127.0.0.1", "listen_port": ${BENCH_API_PORT} }]
}
EOF

CONF_FILE="$BENCH_DIR/atp.conf"
cat > "$CONF_FILE" <<EOF
DATA_DIR="${BENCH_DIR}"
RUN_DIR="run"
CORE_USER_GROUP="root:root"
API_PORT=${BENCH_API_PORT}
EOF
PID_FILE="$BENCH_DIR/run/atpd.pid"
SINGBOX_PID_FILE="$BENCH_DIR/run/sing-box.pid"
printf 'epoch,phase,rss_kb,hwm_kb,vm_kb,pss_kb,fd_count,threads\n' > "$RESOURCE_CSV"

NETLINK_IFACE="atpd_bench0"
if ! command -v ip >/dev/null 2>&1 ||
   ! ip link add "$NETLINK_IFACE" type dummy 2>/dev/null; then
    echo "[BENCH-BLOCKED] CAP_NET_ADMIN is required; resource benchmark NOT PASS" >&2
    exit 77
fi
ip link del "$NETLINK_IFACE"

"$BENCH_DIR/atpd" -c "$CONF_FILE" start
ready=0
for _ in $(seq 1 50); do
    if [ -s "$PID_FILE" ] && [ -S "$BENCH_DIR/run/atpd.sock" ] &&
       run_timeout 2 "$BENCH_DIR/atpd" -c "$CONF_FILE" status 2>/dev/null |
           grep -q "RUNNING"; then
        ready=1
        break
    fi
    sleep 0.2
done
[ "$ready" -eq 1 ] || { echo "[BENCH-ERROR] ATPD did not become ready" >&2; exit 1; }
IFS= read -r ATPD_PID < "$PID_FILE"
kill -0 "$ATPD_PID" 2>/dev/null || { echo "[BENCH-ERROR] invalid ATPD PID" >&2; exit 1; }

collect_atpd_resources "$ATPD_PID" baseline
BASE_RSS="$LAST_RSS"; BASE_FD="$LAST_FD"; BASE_THREADS="$LAST_THREADS"
for value in "$BASE_RSS" "$BASE_FD" "$BASE_THREADS"; do
    [ "$value" != N/A ] || fail_gate "required baseline metric unavailable"
done

start_ns=$(date +%s%N)
for ((i=0; i<STATUS_QUERIES; i++)); do
    run_timeout 2 "$BENCH_DIR/atpd" -c "$CONF_FILE" status >/dev/null ||
        { fail_gate "status query ${i} failed"; break; }
done
end_ns=$(date +%s%N)
elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))
[ "$elapsed_ms" -gt 0 ] || elapsed_ms=1
collect_atpd_resources "$ATPD_PID" status_stress

for ((i=0; i<NETLINK_CYCLES; i++)); do
    ip link add "$NETLINK_IFACE" type dummy
    ip link set "$NETLINK_IFACE" up
    ip link del "$NETLINK_IFACE"
done
collect_atpd_resources "$ATPD_PID" post_netlink

for ((i=1; i<=RECOVERY_SECONDS; i++)); do
    sleep 1
    collect_atpd_resources "$ATPD_PID" "recovery_${i}"
done
RECOVERY_RSS="$LAST_RSS"; RECOVERY_FD="$LAST_FD"; RECOVERY_THREADS="$LAST_THREADS"
RSS_SLOPE=$(awk -F, '
    $2 ~ /^recovery_/ && $3 ~ /^[0-9]+$/ {
        if (!start) start=$1; x=$1-start; y=$3; n++; sx+=x; sy+=y; sxx+=x*x; sxy+=x*y
    }
    END {
        d=n*sxx-sx*sx;
        if (n < 2 || d == 0) print "N/A";
        else printf "%.3f", 60*(n*sxy-sx*sy)/d
    }' "$RESOURCE_CSV")

if [ "$BASE_RSS" != N/A ] && [ "$BASE_RSS" -gt "$MAX_BASELINE_RSS_KB" ]; then
    fail_gate "baseline RSS ${BASE_RSS}KB exceeds ${MAX_BASELINE_RSS_KB}KB"
fi
if [ "$BASE_RSS" != N/A ] && [ "$RECOVERY_RSS" != N/A ] &&
   [ $((RECOVERY_RSS - BASE_RSS)) -gt "$MAX_RSS_GROWTH_KB" ]; then
    fail_gate "RSS growth $((RECOVERY_RSS - BASE_RSS))KB exceeds ${MAX_RSS_GROWTH_KB}KB"
fi
if [ "$BASE_FD" != N/A ] && [ "$RECOVERY_FD" != N/A ] &&
   [ $((RECOVERY_FD - BASE_FD)) -gt "$MAX_FD_GROWTH" ]; then
    fail_gate "FD growth $((RECOVERY_FD - BASE_FD)) exceeds ${MAX_FD_GROWTH}"
fi
if [ "$BASE_THREADS" != N/A ] && [ "$RECOVERY_THREADS" != N/A ] &&
   [ $((RECOVERY_THREADS - BASE_THREADS)) -gt "$MAX_THREAD_GROWTH" ]; then
    fail_gate "thread growth $((RECOVERY_THREADS - BASE_THREADS)) exceeds ${MAX_THREAD_GROWTH}"
fi
if [ "$RSS_SLOPE" = N/A ]; then
    fail_gate "RSS slope unavailable"
elif awk "BEGIN {exit !(${RSS_SLOPE} > ${MAX_RSS_SLOPE_KB_PER_MIN})}"; then
    fail_gate "RSS slope ${RSS_SLOPE}KB/min exceeds ${MAX_RSS_SLOPE_KB_PER_MIN}KB/min"
fi
kill -0 "$ATPD_PID" 2>/dev/null || fail_gate "ATPD exited during benchmark"

echo "ATPD RESOURCE BENCHMARK"
echo "status_queries=${STATUS_QUERIES} elapsed_ms=${elapsed_ms}"
echo "baseline_rss_kb=${BASE_RSS} recovery_rss_kb=${RECOVERY_RSS} rss_slope_kb_per_min=${RSS_SLOPE}"
echo "baseline_fd=${BASE_FD} recovery_fd=${RECOVERY_FD}"
echo "baseline_threads=${BASE_THREADS} recovery_threads=${RECOVERY_THREADS}"
echo "resources_csv=${RESOURCE_CSV}"

if [ "$FAIL_COUNT" -ne 0 ]; then
    echo "Result: FAIL (${FAIL_COUNT} hard gate failures)"
    exit 1
fi
echo "Result: PASS"
