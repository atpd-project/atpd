#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ATP_BIN="${1:-${ROOT}/build/bin/atpd}"
SINGBOX_BIN="${2:-$(command -v sing-box || true)}"
TEST_DIR="$(mktemp -d "${TMPDIR:-/tmp}/atpd-crash-recovery.XXXXXX")"
CONF_FILE="${TEST_DIR}/atp.conf"
PID_FILE="${TEST_DIR}/run/atpd.pid"
SINGBOX_PID_FILE="${TEST_DIR}/run/sing-box.pid"
SOCKET_FILE="${TEST_DIR}/run/atpd.sock"
API_PORT="${API_PORT:-9280}"
INBOUND_PORT="${INBOUND_PORT:-2280}"

cleanup() {
    if [ -s "${CONF_FILE}" ] && [ -s "${PID_FILE}" ]; then
        "${TEST_DIR}/atpd" -c "${CONF_FILE}" stop >/dev/null 2>&1 || true
    fi
    rm -rf "${TEST_DIR}"
}
trap cleanup EXIT INT TERM

[ "$(id -u)" -eq 0 ] || { echo "crash recovery test requires root" >&2; exit 77; }
[ -x "${ATP_BIN}" ] || { echo "missing ATPD binary: ${ATP_BIN}" >&2; exit 1; }
[ -n "${SINGBOX_BIN}" ] && [ -x "${SINGBOX_BIN}" ] || {
    echo "missing sing-box binary" >&2
    exit 1
}

mkdir -p "${TEST_DIR}/run" "${TEST_DIR}/bin"
cp "${ATP_BIN}" "${TEST_DIR}/atpd"
cp "${SINGBOX_BIN}" "${TEST_DIR}/bin/sing-box"
chmod +x "${TEST_DIR}/atpd" "${TEST_DIR}/bin/sing-box"

cat > "${TEST_DIR}/config.json" <<EOF
{
  "log": { "level": "warn" },
  "inbounds": [{ "type": "mixed", "tag": "mixed-in", "listen": "127.0.0.1", "listen_port": ${INBOUND_PORT} }],
  "outbounds": [{ "type": "direct", "tag": "direct" }],
  "services": [{ "type": "api", "listen": "127.0.0.1", "listen_port": ${API_PORT} }]
}
EOF
cat > "${CONF_FILE}" <<EOF
DATA_DIR="${TEST_DIR}"
RUN_DIR="run"
CORE_USER_GROUP="root:root"
API_PORT=${API_PORT}
EOF

"${TEST_DIR}/atpd" -c "${CONF_FILE}" start
for _ in $(seq 1 100); do
    if [ -s "${PID_FILE}" ] && [ -S "${SOCKET_FILE}" ] &&
       "${TEST_DIR}/atpd" -c "${CONF_FILE}" status 2>/dev/null | grep -q RUNNING; then
        break
    fi
    sleep 0.1
done
[ -s "${PID_FILE}" ] && [ -s "${SINGBOX_PID_FILE}" ] || {
    echo "daemon did not become ready" >&2
    exit 1
}
ATPD_PID="$(tr -d '[:space:]' < "${PID_FILE}")"
OLD_SINGBOX_PID="$(tr -d '[:space:]' < "${SINGBOX_PID_FILE}")"
BASE_FD="$(find "/proc/${ATPD_PID}/fd" -mindepth 1 -maxdepth 1 -type l | wc -l)"
kill -9 "${OLD_SINGBOX_PID}"

NEW_SINGBOX_PID=""
for _ in $(seq 1 100); do
    if [ -s "${SINGBOX_PID_FILE}" ]; then
        NEW_SINGBOX_PID="$(tr -d '[:space:]' < "${SINGBOX_PID_FILE}")"
        if [[ "${NEW_SINGBOX_PID}" =~ ^[1-9][0-9]*$ ]] &&
           [ "${NEW_SINGBOX_PID}" != "${OLD_SINGBOX_PID}" ] &&
           kill -0 "${NEW_SINGBOX_PID}" 2>/dev/null &&
           [ "$(tr -d '[:space:]' < "/proc/${NEW_SINGBOX_PID}/comm")" = "sing-box" ] &&
           "${TEST_DIR}/atpd" -c "${CONF_FILE}" status 2>/dev/null | grep -q RUNNING &&
           [ "$(tr -d '[:space:]' < "${PID_FILE}")" = "${ATPD_PID}" ]; then
            break
        fi
    fi
    sleep 0.2
done
[ -n "${NEW_SINGBOX_PID}" ] && [ "${NEW_SINGBOX_PID}" != "${OLD_SINGBOX_PID}" ] || {
    echo "sing-box crash recovery failed" >&2
    exit 1
}
! kill -0 "${OLD_SINGBOX_PID}" 2>/dev/null || {
    echo "old sing-box process was not reaped" >&2
    exit 1
}
RECOVERY_FD="$(find "/proc/${ATPD_PID}/fd" -mindepth 1 -maxdepth 1 -type l | wc -l)"
[ "$((RECOVERY_FD - BASE_FD))" -le 1 ] || {
    echo "ATPD FD growth exceeded one descriptor" >&2
    exit 1
}

echo "crash recovery PASS: atpd=${ATPD_PID} sing-box=${OLD_SINGBOX_PID}->${NEW_SINGBOX_PID} fd=${BASE_FD}->${RECOVERY_FD}"
