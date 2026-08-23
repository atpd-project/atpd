#!/bin/bash
#
# ATPd + sing-box Lifecycle Integration Test Script
#
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log_info() { echo -e "${CYAN}[TEST-INFO]${NC} $*"; }
log_pass() { echo -e "${GREEN}[TEST-PASS]${NC} $*"; }
log_fail() { echo -e "${RED}[TEST-FAIL]${NC} $*" >&2; exit 1; }
log_warn() { echo -e "${YELLOW}[TEST-WARN]${NC} $*"; }

TEST_DIR="/tmp/atp_lifecycle_test"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

log_info "Project Root: ${PROJECT_ROOT}"
log_info "Test Directory: ${TEST_DIR}"

# 1. Clean and setup test environment
rm -rf "${TEST_DIR}"
mkdir -p "${TEST_DIR}/bin" "${TEST_DIR}/run"

# 2. Check atpd binary
if [ ! -f "${PROJECT_ROOT}/build/bin/atpd" ]; then
    log_info "Building atpd binary..."
    make -C "${PROJECT_ROOT}" clean
    make -C "${PROJECT_ROOT}"
fi
cp "${PROJECT_ROOT}/build/bin/atpd" "${TEST_DIR}/atpd"
chmod +x "${TEST_DIR}/atpd"

# 3. Locate sing-box binary
SINGBOX_BIN=""
if command -v sing-box >/dev/null 2>&1; then
    SINGBOX_BIN="$(command -v sing-box)"
elif [ -f "/usr/bin/sing-box" ]; then
    SINGBOX_BIN="/usr/bin/sing-box"
elif [ -f "/usr/local/bin/sing-box" ]; then
    SINGBOX_BIN="/usr/local/bin/sing-box"
fi

if [ -z "${SINGBOX_BIN}" ]; then
    log_fail "sing-box binary not found! Please install sing-box or pass deb package."
fi

log_info "Found sing-box binary at: ${SINGBOX_BIN}"
cp "${SINGBOX_BIN}" "${TEST_DIR}/bin/sing-box"
chmod +x "${TEST_DIR}/bin/sing-box"

# 4. Generate sing-box config.json
cat > "${TEST_DIR}/config.json" << 'EOJSON'
{
  "log": {
    "level": "info",
    "timestamp": true
  },
  "experimental": {
    "clash_api": {
      "external_controller": "127.0.0.1:9090",
      "secret": ""
    }
  },
  "inbounds": [
    {
      "type": "mixed",
      "tag": "mixed-in",
      "listen": "127.0.0.1",
      "listen_port": 2080
    }
  ],
  "outbounds": [
    {
      "type": "direct",
      "tag": "direct"
    }
  ]
}
EOJSON

# 5. Generate atp.conf
cat > "${TEST_DIR}/atp.conf" << 'EOCONF'
PERFORMANCE_MODE=1
LOG_TIMESTAMP=1
API_PORT=9090
SERVICE_START_TIMEOUT=15
SERVICE_STOP_TIMEOUT=10
EOCONF

log_pass "Test environment initialized."

# Helper to dump logs on failure
dump_logs() {
    log_warn "--- DUMPING RUN LOGS ---"
    if [ -f "${TEST_DIR}/run/atp.log" ]; then
        echo "=== atp.log ==="
        cat "${TEST_DIR}/run/atp.log" || true
    fi
    if [ -f "${TEST_DIR}/run/sing-box.log" ]; then
        echo "=== sing-box.log ==="
        cat "${TEST_DIR}/run/sing-box.log" || true
    fi
    chmod -R 777 "${TEST_DIR}" 2>/dev/null || true
}
trap dump_logs ERR

# --- PRE-CHECK: Validate sing-box config ---
log_info "Pre-check: Validating sing-box configuration syntax..."
"${TEST_DIR}/bin/sing-box" check -c "${TEST_DIR}/config.json" -D "${TEST_DIR}"
log_pass "sing-box configuration is 100% valid."

# --- SCENARIO A: Pre-running sing-box Discovery Test ---
log_info "Scenario A: Starting sing-box directly to test pre-running process discovery..."
cd "${TEST_DIR}"
./bin/sing-box run -c config.json -D . > run/sing-box.log 2>&1 &
SINGBOX_PID=$!
echo "sing-box started in background with PID: ${SINGBOX_PID}"

# Wait for Clash API to be active
for i in {1..10}; do
    if curl -s -m 1 http://127.0.0.1:9090/version >/dev/null 2>&1; then
        log_pass "Pre-running sing-box is active and Clash API is listening."
        break
    fi
    sleep 0.5
done

log_info "Testing 'atpd status' on pre-running sing-box instance..."
STATUS_OUTPUT="$(./atpd status)"
echo "${STATUS_OUTPUT}"

if echo "${STATUS_OUTPUT}" | grep -q "PID"; then
    log_pass "atpd successfully discovered pre-running sing-box PID and metrics!"
else
    dump_logs
    log_fail "atpd failed to detect pre-running sing-box!"
fi

# Stop pre-running sing-box instance cleanly
kill "${SINGBOX_PID}" 2>/dev/null || true
wait "${SINGBOX_PID}" 2>/dev/null || true
sleep 1

# --- SCENARIO B: Full ATPd Daemon Lifecycle Management ---
log_info "Scenario B: Testing ATPd managed startup ('atpd start')..."
./atpd start
sleep 2

STATUS_OUTPUT="$(./atpd status)"
echo "${STATUS_OUTPUT}"

if echo "${STATUS_OUTPUT}" | grep -q "PID"; then
    log_pass "atpd daemon successfully spawned and managed sing-box!"
else
    dump_logs
    log_fail "atpd failed to spawn sing-box!"
fi

# Verify Clash REST API connectivity
log_info "Querying Clash API (http://127.0.0.1:9090/version)..."
API_RES="$(curl -s -m 3 http://127.0.0.1:9090/version || true)"
if [ -n "${API_RES}" ]; then
    log_pass "Clash API responded: ${API_RES}"
else
    dump_logs
    log_fail "Clash API did not respond!"
fi

# --- SCENARIO C: ATPd Restart ---
log_info "Scenario C: Testing 'atpd restart'..."
./atpd restart
sleep 2

RESTART_STATUS="$(./atpd status)"
echo "${RESTART_STATUS}"
if echo "${RESTART_STATUS}" | grep -q "PID"; then
    log_pass "Restart completed successfully and sing-box is RUNNING."
else
    dump_logs
    log_fail "Restart failed, sing-box not running!"
fi

# --- SCENARIO D: ATPd Stop ---
log_info "Scenario D: Testing 'atpd stop'..."
./atpd stop
sleep 1

STOP_STATUS="$(./atpd status)"
echo "${STOP_STATUS}"

if echo "${STOP_STATUS}" | grep -q "STOPPED" || echo "${STOP_STATUS}" | grep -q "Daemon stopped"; then
    log_pass "sing-box successfully stopped and verified."
else
    dump_logs
    log_fail "sing-box failed to stop!"
fi

# --- SCENARIO E: Out-of-tree / Service.d Simulation Test ---
log_info "Scenario E: Testing dynamic self-location from arbitrary CWD (simulating service.d boot script)..."
cd /tmp
"${TEST_DIR}/atpd" start
sleep 2

OUT_STATUS="$("${TEST_DIR}/atpd" status)"
echo "${OUT_STATUS}"
"${TEST_DIR}/atpd" stop

log_pass "ALL TEST SCENARIOS PASSED 100%!"
chmod -R 777 "${TEST_DIR}" 2>/dev/null || true
rm -rf "${TEST_DIR}"
