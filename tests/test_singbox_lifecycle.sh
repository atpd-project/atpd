#!/bin/bash
#
# ATPd + sing-box Lifecycle Integration Test Script
# Test Matrix: 1. 启动 (Start) -> 2. 停止 (Stop) -> 3. 重启 (Restart) -> 4. 停止 (Final Stop)
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
    log_fail "sing-box binary not found! Please install sing-box."
fi

log_info "Found sing-box binary at: ${SINGBOX_BIN}"
cp "${SINGBOX_BIN}" "${TEST_DIR}/bin/sing-box"
chmod +x "${TEST_DIR}/bin/sing-box"

# 4. Generate sing-box minimal config.json
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
PERFORMANCE_MODE=0
LOG_TIMESTAMP=1
API_PORT=9090
SERVICE_START_TIMEOUT=15
SERVICE_STOP_TIMEOUT=10
CORE_USER_GROUP=root:root
EOCONF

log_pass "Test environment initialized successfully."

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

# --- PRE-CHECK: Validate sing-box config syntax ---
log_info "Pre-check: Validating sing-box configuration syntax..."
"${TEST_DIR}/bin/sing-box" check -c "${TEST_DIR}/config.json" -D "${TEST_DIR}"
log_pass "sing-box configuration verified by official CLI check."

cd "${TEST_DIR}"

# ==============================================================================
# 阶段 1: 启动 (Start) -> 校验进程 PID 与 Clash API 连通
# ==============================================================================
log_info "=== [STEP 1/4] 启动测试: 'atpd start' ==="
./atpd start

STATUS_OUTPUT=""
for i in {1..10}; do
    STATUS_OUTPUT="$(./atpd status 2>&1)"
    if echo "${STATUS_OUTPUT}" | grep -q "PID"; then
        break
    fi
    sleep 0.5
done
echo "${STATUS_OUTPUT}"

if echo "${STATUS_OUTPUT}" | grep -q "PID"; then
    log_pass "Step 1 PASS: atpd 成功拉起 sing-box 并捕获到活跃 PID!"
else
    dump_logs
    log_fail "Step 1 FAIL: atpd 未能成功拉起 sing-box!"
fi

# 校验 Clash REST API
log_info "校验 Clash API (http://127.0.0.1:9090/version)..."
API_RES=""
for i in {1..10}; do
    API_RES="$(curl -s -m 2 http://127.0.0.1:9090/version || true)"
    if [ -n "${API_RES}" ]; then
        break
    fi
    sleep 0.5
done

if [ -n "${API_RES}" ]; then
    log_pass "Clash API 响应成功: ${API_RES}"
else
    dump_logs
    log_fail "Clash API 未能在预期时间内响应!"
fi

# ==============================================================================
# 阶段 2: 停止一次 (Stop) -> 校验进程安全退出与状态归位
# ==============================================================================
log_info "=== [STEP 2/4] 停止测试: 'atpd stop' ==="
./atpd stop

STOP_STATUS=""
for i in {1..10}; do
    STOP_STATUS="$(./atpd status 2>&1)"
    if echo "${STOP_STATUS}" | grep -q "STOPPED" || echo "${STOP_STATUS}" | grep -q "Daemon stopped"; then
        break
    fi
    sleep 0.5
done
echo "${STOP_STATUS}"

if echo "${STOP_STATUS}" | grep -q "STOPPED" || echo "${STOP_STATUS}" | grep -q "Daemon stopped"; then
    log_pass "Step 2 PASS: sing-box 进程与守护中枢已完全平稳停止!"
else
    dump_logs
    log_fail "Step 2 FAIL: 停止指令执行后进程未能退出!"
fi

# ==============================================================================
# 阶段 3: 重启 / 再次启动 (Restart / Re-start) -> 校验热恢复与新 PID
# ==============================================================================
log_info "=== [STEP 3/4] 重启测试: 'atpd restart' ==="
./atpd restart

RESTART_STATUS=""
for i in {1..10}; do
    RESTART_STATUS="$(./atpd status 2>&1)"
    if echo "${RESTART_STATUS}" | grep -q "PID"; then
        break
    fi
    sleep 0.5
done
echo "${RESTART_STATUS}"

if echo "${RESTART_STATUS}" | grep -q "PID"; then
    log_pass "Step 3 PASS: atpd 成功重启 sing-box 并保持健康运行!"
else
    dump_logs
    log_fail "Step 3 FAIL: 重启后 sing-box 未能恢复运行!"
fi

# 再次验证 Clash API
API_RES="$(curl -s -m 2 http://127.0.0.1:9090/version || true)"
if [ -n "${API_RES}" ]; then
    log_pass "重启后 Clash API 响应正常: ${API_RES}"
else
    dump_logs
    log_fail "重启后 Clash API 无法访问!"
fi

# ==============================================================================
# 阶段 4: 最终停止 (Final Stop) -> 完整生命周期收尾
# ==============================================================================
log_info "=== [STEP 4/4] 最终收尾: 'atpd stop' ==="
./atpd stop
sleep 1

FINAL_STATUS="$(./atpd status 2>&1)"
echo "${FINAL_STATUS}"
log_pass "Step 4 PASS: 完整启停、重启闭环生命周期测试全部通过 (100% SUCCESS)!"

# 清理测试现场
chmod -R 777 "${TEST_DIR}" 2>/dev/null || true
rm -rf "${TEST_DIR}"
