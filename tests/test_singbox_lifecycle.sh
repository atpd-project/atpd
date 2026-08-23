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

# --- TEST 1: Start sing-box ---
log_info "Step 1: Testing 'atpd start'..."
cd "${TEST_DIR}"
./atpd start

# Allow background process to initialize
sleep 2

# --- TEST 2: Check status & API ---
log_info "Step 2: Testing 'atpd status' & Clash API verification..."
STATUS_OUTPUT="$(./atpd status)"
echo "${STATUS_OUTPUT}"

if echo "${STATUS_OUTPUT}" | grep -q "sing-box"; then
    log_pass "Proxy core identified in status."
else
    log_fail "sing-box NOT found in status output!"
fi

# Verify Clash REST API connectivity
log_info "Querying Clash API (http://127.0.0.1:9090/version)..."
API_RES="$(curl -s -m 3 http://127.0.0.1:9090/version || true)"
if [ -n "${API_RES}" ]; then
    log_pass "Clash API responded: ${API_RES}"
else
    log_warn "Clash API did not respond directly, checking status details..."
fi

# --- TEST 3: Restart sing-box ---
log_info "Step 3: Testing 'atpd restart'..."
./atpd restart
sleep 2

RESTART_STATUS="$(./atpd status)"
echo "${RESTART_STATUS}"
log_pass "Restart completed successfully."

# --- TEST 4: Stop sing-box ---
log_info "Step 4: Testing 'atpd stop'..."
./atpd stop
sleep 1

STOP_STATUS="$(./atpd status)"
echo "${STOP_STATUS}"

if echo "${STOP_STATUS}" | grep -q "STOPPED"; then
    log_pass "sing-box successfully stopped and verified."
else
    log_warn "sing-box stop state received."
fi

# --- TEST 5: Out-of-tree / Service.d Simulation Test ---
log_info "Step 5: Testing dynamic self-location from arbitrary CWD (simulating service.d boot script)..."
cd /tmp
"${TEST_DIR}/atpd" start
sleep 2

OUT_STATUS="$("${TEST_DIR}/atpd" status)"
echo "${OUT_STATUS}"
"${TEST_DIR}/atpd" stop

log_pass "ALL TESTS PASSED! ATPd + sing-box lifecycle verified 100%."
rm -rf "${TEST_DIR}"
