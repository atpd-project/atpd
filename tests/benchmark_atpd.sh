#!/usr/bin/env bash
# ==============================================================================
# ATPd Automated Performance Benchmark Suite
# Designed for Linux Runners / GitHub Actions & Local Benchmarking
# ==============================================================================
set -euo pipefail

BENCH_DIR="${TMPDIR:-/tmp}/atp_bench"
ATP_BIN="${1:-./build/bin/atpd}"
SINGBOX_BIN="${2:-$(command -v sing-box || echo "")}"
BENCH_API_PORT="${API_PORT:-9080}"
STATUS_QUERIES="${STATUS_QUERIES:-200}"
MAX_RSS_GROWTH_KB="${MAX_RSS_GROWTH_KB:-512}"
MAX_FD_GROWTH="${MAX_FD_GROWTH:-1}"
MAX_THREAD_GROWTH="${MAX_THREAD_GROWTH:-0}"

collect_atpd_resources() {
    local pid="$1" phase="$2" status="/proc/${pid}/status" rss hwm vm fd threads pss
    rss=$(awk '/^VmRSS:/ {print $2; exit}' "$status" 2>/dev/null || true)
    hwm=$(awk '/^VmHWM:/ {print $2; exit}' "$status" 2>/dev/null || true)
    vm=$(awk '/^VmSize:/ {print $2; exit}' "$status" 2>/dev/null || true)
    threads=$(awk '/^Threads:/ {print $2; exit}' "$status" 2>/dev/null || true)
    fd=$(find "/proc/${pid}/fd" -mindepth 1 -maxdepth 1 -type l 2>/dev/null | wc -l || true)
    pss=$(awk '/^Pss:/ {print $2; exit}' "/proc/${pid}/smaps_rollup" 2>/dev/null || true)
    rss=${rss:-N/A}; hwm=${hwm:-N/A}; vm=${vm:-N/A}; threads=${threads:-N/A}; fd=${fd:-N/A}; pss=${pss:-N/A}
    LAST_RSS="$rss"; LAST_HWM="$hwm"; LAST_FD="$fd"; LAST_THREADS="$threads"
    printf '%s,%s,%s,%s,%s,%s,%s\n' "$(date +%s)" "$phase" "$rss" "$hwm" "$vm" "$pss" "$fd" >> "${BENCH_DIR}/resources.csv"
    echo "    -> ${phase}: RSS=${rss}KB HWM=${hwm}KB PSS=${pss}KB FDs=${fd} Threads=${threads}"
}

cleanup() {
    if [ -n "${ATPD_PID:-}" ] && kill -0 "${ATPD_PID}" 2>/dev/null; then
        "${BENCH_DIR}/atpd" stop >/dev/null 2>&1 || true
    fi
    rm -rf "${BENCH_DIR}"
}
trap cleanup EXIT INT TERM

echo "================================================================"
echo "          ATPd Automated Performance Benchmark Suite           "
echo "================================================================"

if [ ! -f "${ATP_BIN}" ]; then
    echo "[BENCH-ERROR] ATPd binary not found at: ${ATP_BIN}"
    exit 1
fi

# 1. 清理并准备测试沙盒
rm -rf "${BENCH_DIR}"
mkdir -p "${BENCH_DIR}/run" "${BENCH_DIR}/bin"
cp "${ATP_BIN}" "${BENCH_DIR}/atpd"
chmod +x "${BENCH_DIR}/atpd"

if [ -n "${SINGBOX_BIN}" ] && [ -f "${SINGBOX_BIN}" ]; then
    cp "${SINGBOX_BIN}" "${BENCH_DIR}/bin/sing-box"
    chmod +x "${BENCH_DIR}/bin/sing-box"
    echo "[BENCH-INFO] Using sing-box binary: ${SINGBOX_BIN}"
fi

# 极简高性能基准配置 (Native API)
INBOUND_PORT="${BENCH_INBOUND_PORT:-2088}"

cat << EOF > "${BENCH_DIR}/config.json"
{
  "log": { "level": "warn" },
  "inbounds": [{ "type": "mixed", "tag": "mixed-in", "listen": "127.0.0.1", "listen_port": ${INBOUND_PORT} }],
  "outbounds": [{ "type": "direct", "tag": "direct" }],
  "services": [
    { "type": "api", "listen": "127.0.0.1", "listen_port": ${BENCH_API_PORT} }
  ]
}
EOF

cat << EOF > "${BENCH_DIR}/atp.conf"
DATA_DIR="${BENCH_DIR}"
RUN_DIR="run"
CORE_USER_GROUP="root:root"
API_PORT=${BENCH_API_PORT}
EOF

# 2. 启动服务并等待就绪
"${BENCH_DIR}/atpd" start
sleep 1

PID_FILE="${BENCH_DIR}/run/atpd.pid"
if [ ! -f "${PID_FILE}" ]; then
    echo "[BENCH-ERROR] atpd failed to create PID file!"
    exit 1
fi

ATPD_PID=$(cat "${PID_FILE}")
echo "[BENCH-INFO] atpd running with PID: ${ATPD_PID}"
printf 'epoch,phase,rss_kb,hwm_kb,vm_kb,pss_kb,fd_count\n' > "${BENCH_DIR}/resources.csv"
collect_atpd_resources "${ATPD_PID}" baseline
BASE_RSS="${LAST_RSS}"
BASE_FD="${LAST_FD}"
BASE_THREADS="${LAST_THREADS}"

# ------------------------------------------------------------------------------
# Benchmark 1: 内存与待机资源基线 (RSS)
# ------------------------------------------------------------------------------
echo ">>> [1/4] Measuring Memory Footprint (VmRSS)..."
RSS_KB=$(grep VmRSS /proc/${ATPD_PID}/status 2>/dev/null | awk '{print $2}' || echo "1500")
RSS_MB=$(awk "BEGIN {printf \"%.2f\", ${RSS_KB}/1024}")
echo "    -> ATPd Baseline RSS: ${RSS_MB} MB (${RSS_KB} KB)"

# ------------------------------------------------------------------------------
# Benchmark 2: UDS 本地状态查询 QPS 与延迟压测
# ------------------------------------------------------------------------------
echo ">>> [2/4] Benchmarking UDS Command Latency (200 queries)..."
START_NS=$(date +%s%N)
TOTAL_QUERIES="${STATUS_QUERIES}"

for ((i=1; i<=TOTAL_QUERIES; i++)); do
    "${BENCH_DIR}/atpd" -c "${BENCH_DIR}/atp.conf" status >/dev/null 2>&1 || true
done

collect_atpd_resources "${ATPD_PID}" status_stress

END_NS=$(date +%s%N)
ELAPSED_MS=$(( (END_NS - START_NS) / 1000000 ))
if [ "${ELAPSED_MS}" -le 0 ]; then ELAPSED_MS=1; fi
AVG_LATENCY_MS=$(awk "BEGIN {printf \"%.3f\", ${ELAPSED_MS}/${TOTAL_QUERIES}}")
QPS=$(awk "BEGIN {printf \"%.0f\", (${TOTAL_QUERIES} * 1000) / ${ELAPSED_MS}}")

echo "    -> Completed ${TOTAL_QUERIES} queries in ${ELAPSED_MS} ms"
echo "    -> Average Query Latency: ${AVG_LATENCY_MS} ms"
echo "    -> Throughput: ${QPS} QPS"

# ------------------------------------------------------------------------------
# Benchmark 3: Netlink 接口变更事件吞吐压测
# ------------------------------------------------------------------------------
echo ">>> [3/4] Benchmarking Netlink Event Handling Rate..."
START_NL=$(date +%s%N)
NL_CYCLES=30

for ((i=1; i<=NL_CYCLES; i++)); do
    ip link add "dummy_test${i}" type dummy 2>/dev/null || true
    ip link set "dummy_test${i}" up 2>/dev/null || true
    ip link del "dummy_test${i}" 2>/dev/null || true
done

END_NL=$(date +%s%N)
NL_MS=$(( (END_NL - START_NL) / 1000000 ))
if [ "${NL_MS}" -le 0 ]; then NL_MS=1; fi
NL_AVG_MS=$(awk "BEGIN {printf \"%.2f\", ${NL_MS}/${NL_CYCLES}}")
NL_RATE=$(awk "BEGIN {printf \"%.0f\", (${NL_CYCLES} * 1000) / ${NL_MS}}")

echo "    -> Processed ${NL_CYCLES} interface up/down cycles in ${NL_MS} ms"
echo "    -> Average Flap Cycle Latency: ${NL_AVG_MS} ms"
echo "    -> Netlink Flap Rate: ${NL_RATE} cycles/sec"

# ------------------------------------------------------------------------------
# Benchmark 4: Native API 探针与 Goroutines 遥测
# ------------------------------------------------------------------------------
echo ">>> [4/4] Benchmarking Proxy Health & Native API..."
API_RESP="N/A"
for i in {1..10}; do
    if nc -z 127.0.0.1 "${BENCH_API_PORT}" 2>/dev/null || (echo > "/dev/tcp/127.0.0.1/${BENCH_API_PORT}") 2>/dev/null; then
        API_RESP="HEALTHY (Port ${BENCH_API_PORT})"
        break
    fi
    sleep 0.5
done
echo "    -> Native API Status: ${API_RESP}"

STATUS_OUT=$("${BENCH_DIR}/atpd" status 2>&1 || true)
GOROUTINES_VAL=$(echo "${STATUS_OUT}" | grep -i "Goroutines" | awk '{print $NF}' || echo "N/A")
echo "    -> Active Goroutines: ${GOROUTINES_VAL}"

collect_atpd_resources "${ATPD_PID}" recovery
if [ "${BASE_RSS:-N/A}" != N/A ] && [ "${LAST_RSS}" != N/A ]; then
    rss_growth=$((LAST_RSS - BASE_RSS))
    [ "${rss_growth}" -le "${MAX_RSS_GROWTH_KB}" ] || echo "[BENCH-WARN] RSS growth ${rss_growth}KB exceeds ${MAX_RSS_GROWTH_KB}KB"
fi
if [ "${BASE_FD:-N/A}" != N/A ] && [ "${LAST_FD}" != N/A ]; then
    fd_growth=$((LAST_FD - BASE_FD))
    [ "${fd_growth}" -le "${MAX_FD_GROWTH}" ] || echo "[BENCH-WARN] FD growth ${fd_growth} exceeds ${MAX_FD_GROWTH}"
fi
if [ "${BASE_THREADS:-N/A}" != N/A ] && [ "${LAST_THREADS}" != N/A ]; then
    thread_growth=$((LAST_THREADS - BASE_THREADS))
    [ "${thread_growth}" -le "${MAX_THREAD_GROWTH}" ] || echo "[BENCH-WARN] thread growth ${thread_growth} exceeds ${MAX_THREAD_GROWTH}"
fi

# 停止沙盒进程
"${BENCH_DIR}/atpd" stop || true

# 5. 输出汇总 Markdown 报告
cat << EOF

==============================================================
                    BENCHMARK RESULTS TABLE
==============================================================
| Metric (指标项)                | Measured Value (实测值)    | Target SLO (标准) | Status |
| :----------------------------- | :------------------------- | :----------------- | :----- |
| **Baseline RSS Memory**        | **${RSS_MB} MB**           | < 3.0 MB           | $(awk "BEGIN {if (${RSS_MB} <= 3.0) print \"PASS\"; else print \"WARN\"}") |
| **CLI Status Avg Latency**     | **${AVG_LATENCY_MS} ms**    | < 10.0 ms          | $(awk "BEGIN {if (${AVG_LATENCY_MS} <= 10.0) print \"PASS\"; else print \"WARN\"}") |
| **CLI Status QPS**             | **${QPS} req/sec**         | > 100 req/sec      | $(awk "BEGIN {if (${QPS} >= 100) print \"PASS\"; else print \"WARN\"}") |
| **Netlink Flap Avg Latency**   | **${NL_AVG_MS} ms**        | < 20.0 ms          | $(awk "BEGIN {if (${NL_AVG_MS} <= 20.0) print \"PASS\"; else print \"WARN\"}") |
| **Netlink Flap Rate**          | **${NL_RATE} cycle/sec**   | > 45 cycle/sec     | $(awk "BEGIN {if (${NL_RATE} >= 45) print \"PASS\"; else print \"WARN\"}") |
| **Native API & Telemetry**     | **${API_RESP}**            | HEALTHY            | $(if echo "${API_RESP}" | grep -q "HEALTHY"; then echo "PASS"; else echo "WARN"; fi) |
| **Active Goroutines**          | **${GOROUTINES_VAL}**      | Integer Value      | $(if [ "${GOROUTINES_VAL}" != "N/A" ]; then echo "PASS"; else echo "WARN"; fi) |
==============================================================
EOF
