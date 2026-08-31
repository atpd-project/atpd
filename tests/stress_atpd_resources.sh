#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STRESS_DIR="${TMPDIR:-/tmp}/atpd-resource-stress-$$"
ATP_BIN="${1:-${ROOT}/build/bin/atpd}"
STATUS_STRESS_QUERIES="${STATUS_STRESS_QUERIES:-5000}"
STATUS_STRESS_CONCURRENCY="${STATUS_STRESS_CONCURRENCY:-4}"
RELOAD_CYCLES="${RELOAD_CYCLES:-100}"
RESTART_CYCLES="${RESTART_CYCLES:-100}"
NETLINK_CYCLES="${NETLINK_CYCLES:-200}"
MAX_FD_GROWTH="${MAX_FD_GROWTH:-1}"
MAX_RSS_GROWTH_KB="${MAX_RSS_GROWTH_KB:-512}"
MAX_THREAD_GROWTH="${MAX_THREAD_GROWTH:-0}"

cleanup() {
    if [ -n "${PID_FILE:-}" ] && [ -f "${PID_FILE}" ]; then
        "${ATP_BIN}" -c "${CONF_FILE}" stop >/dev/null 2>&1 || true
    fi
    rm -rf "${STRESS_DIR}"
}
trap cleanup EXIT INT TERM

[ -x "${ATP_BIN}" ] || { echo "stress: missing atpd binary" >&2; exit 1; }
mkdir -p "${STRESS_DIR}/run" "${STRESS_DIR}/bin"
if command -v sing-box >/dev/null 2>&1; then
    cp "$(command -v sing-box)" "${STRESS_DIR}/bin/sing-box"
    chmod +x "${STRESS_DIR}/bin/sing-box"
fi
CONF_FILE="${STRESS_DIR}/atp.conf"
cat > "${CONF_FILE}" <<EOF
DATA_DIR="${STRESS_DIR}"
RUN_DIR="run"
CORE_USER_GROUP="root:root"
EOF
PID_FILE="${STRESS_DIR}/run/atpd.pid"

"${ATP_BIN}" -c "${CONF_FILE}" start
for _ in $(seq 1 20); do [ -s "${PID_FILE}" ] && break; sleep 0.2; done
[ -s "${PID_FILE}" ] || { echo "stress: daemon did not start" >&2; exit 1; }
ATPD_PID=$(cat "${PID_FILE}")
BASE_FD=$(find "/proc/${ATPD_PID}/fd" -mindepth 1 -maxdepth 1 -type l 2>/dev/null | wc -l)
BASE_RSS=$(awk '/^VmRSS:/ {print $2; exit}' "/proc/${ATPD_PID}/status")
BASE_THREADS=$(awk '/^Threads:/ {print $2; exit}' "/proc/${ATPD_PID}/status")

worker() {
    local n="$1" i
    for ((i=0; i<n; i++)); do timeout 2 "${ATP_BIN}" -c "${CONF_FILE}" status >/dev/null; done
}
per_worker=$(( (STATUS_STRESS_QUERIES + STATUS_STRESS_CONCURRENCY - 1) / STATUS_STRESS_CONCURRENCY ))
for ((i=0; i<STATUS_STRESS_CONCURRENCY; i++)); do worker "${per_worker}" & done
wait

for ((i=1; i<=RELOAD_CYCLES; i++)); do
    timeout 2 "${ATP_BIN}" -c "${CONF_FILE}" reload >/dev/null || { echo "stress: reload failed" >&2; exit 1; }
    [ "$(cat "${PID_FILE}")" = "${ATPD_PID}" ] || { echo "stress: reload changed PID" >&2; exit 1; }
done

for ((i=1; i<=RESTART_CYCLES; i++)); do
    old_pid=$(cat "${PID_FILE}")
    timeout 5 "${ATP_BIN}" -c "${CONF_FILE}" restart >/dev/null || { echo "stress: restart failed" >&2; exit 1; }
    for _ in $(seq 1 20); do [ -s "${PID_FILE}" ] && [ "$(cat "${PID_FILE}")" != "${old_pid}" ] && break; sleep 0.2; done
    new_pid=$(cat "${PID_FILE}" 2>/dev/null || true)
    [ -n "${new_pid}" ] && [ "${new_pid}" != "${old_pid}" ] || { echo "stress: restart PID did not change" >&2; exit 1; }
    kill -0 "${new_pid}" 2>/dev/null || { echo "stress: restarted daemon is not alive" >&2; exit 1; }
    ATPD_PID="${new_pid}"
done

for ((i=1; i<=NETLINK_CYCLES; i++)); do
    name="atpd_test0"
    ip link add "$name" type dummy 2>/dev/null || true
    ip link set "$name" up 2>/dev/null || true
    ip link del "$name" 2>/dev/null || true
done

RECOVERY_FD=$(find "/proc/${ATPD_PID}/fd" -mindepth 1 -maxdepth 1 -type l 2>/dev/null | wc -l)
RECOVERY_RSS=$(awk '/^VmRSS:/ {print $2; exit}' "/proc/${ATPD_PID}/status")
RECOVERY_THREADS=$(awk '/^Threads:/ {print $2; exit}' "/proc/${ATPD_PID}/status")
fd_delta=$((RECOVERY_FD - BASE_FD))
if [ "${fd_delta}" -gt "${MAX_FD_GROWTH}" ]; then
    echo "stress: FD growth ${fd_delta} exceeds ${MAX_FD_GROWTH}" >&2
    exit 1
fi
rss_delta=$((RECOVERY_RSS - BASE_RSS))
thread_delta=$((RECOVERY_THREADS - BASE_THREADS))
if [ "${rss_delta}" -gt "${MAX_RSS_GROWTH_KB}" ] || [ "${thread_delta}" -gt "${MAX_THREAD_GROWTH}" ]; then
    echo "stress: resource growth RSS=${rss_delta}KB Threads=${thread_delta}" >&2
    exit 1
fi
kill -0 "${ATPD_PID}" 2>/dev/null || { echo "stress: atpd exited" >&2; exit 1; }
echo "resource stress PASS: baseline_fd=${BASE_FD} recovery_fd=${RECOVERY_FD} rss_delta=${rss_delta}KB thread_delta=${thread_delta}"
