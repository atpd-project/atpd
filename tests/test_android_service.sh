#!/bin/sh
set -eu

PROJECT_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
TEST_ROOT="$(mktemp -d)"
FAKE_BIN="${TEST_ROOT}/fake-bin"
SERVICE_SCRIPT="${PROJECT_ROOT}/service.d/atpd_service.sh"

cleanup() {
    for pid_file in "${TEST_ROOT}/run/atpd.pid" "${TEST_ROOT}/run/sing-box.pid"; do
        if [ -r "${pid_file}" ]; then
            IFS= read -r pid < "${pid_file}"
            kill "${pid}" 2>/dev/null || true
        fi
    done
    rm -rf "${TEST_ROOT}"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "${TEST_ROOT}/bin" "${TEST_ROOT}/run" "${TEST_ROOT}/workers" "${FAKE_BIN}"
printf '%s\n' 'API_PORT=9080' > "${TEST_ROOT}/atp.conf"
printf '%s\n' '{}' > "${TEST_ROOT}/config.json"

cat > "${TEST_ROOT}/atpd" <<'EOF'
#!/bin/sh
action=""
for argument in "$@"; do action="${argument}"; done
case "${action}" in
    check) exit 0 ;;
    version) echo "atpd test" ;;
    status)
        echo "API Engine  Native API"
        echo "Goroutines  12"
        echo "Version     sing-box test"
        ;;
    start)
        "${ATP_ROOT}/workers/atpd" 60 & echo "$!" > "${ATP_ROOT}/run/atpd.pid"
        "${ATP_ROOT}/workers/sing-box" 60 & echo "$!" > "${ATP_ROOT}/run/sing-box.pid"
        ;;
    stop)
        for pid_file in "${ATP_ROOT}/run/atpd.pid" "${ATP_ROOT}/run/sing-box.pid"; do
            if [ -r "${pid_file}" ]; then
                IFS= read -r pid < "${pid_file}"
                kill "${pid}" 2>/dev/null || true
            fi
            rm -f "${pid_file}"
        done
        ;;
esac
EOF

cat > "${TEST_ROOT}/bin/sing-box" <<'EOF'
#!/bin/sh
case "$1" in
    check) exit 0 ;;
    version) echo "sing-box test with_ebpf" ;;
esac
EOF

cat > "${FAKE_BIN}/id" <<'EOF'
#!/bin/sh
echo 0
EOF
cat > "${FAKE_BIN}/getprop" <<'EOF'
#!/bin/sh
echo 1
EOF
cat > "${FAKE_BIN}/pidof" <<'EOF'
#!/bin/sh
[ "${FAKE_SINGBOX_CONFLICT:-0}" = "1" ]
EOF
cat > "${FAKE_BIN}/log" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod 0755 "${TEST_ROOT}/atpd" "${TEST_ROOT}/bin/sing-box" "${FAKE_BIN}"/*
ln -s "$(command -v sleep)" "${TEST_ROOT}/workers/atpd"
ln -s "$(command -v sleep)" "${TEST_ROOT}/workers/sing-box"

run_service() {
    ATP_ROOT="${TEST_ROOT}" PATH="${FAKE_BIN}:${PATH}" \
        BOOT_WAIT=2 START_WAIT=2 sh "${SERVICE_SCRIPT}" "$1"
}

run_service check >/dev/null
run_service start
first_atpd_pid="$(cat "${TEST_ROOT}/run/atpd.pid")"
first_sing_box_pid="$(cat "${TEST_ROOT}/run/sing-box.pid")"
run_service start
[ "$(cat "${TEST_ROOT}/run/atpd.pid")" = "${first_atpd_pid}" ]
[ "$(cat "${TEST_ROOT}/run/sing-box.pid")" = "${first_sing_box_pid}" ]
run_service status >/dev/null
run_service stop
[ ! -e "${TEST_ROOT}/run/atpd.pid" ]
[ ! -e "${TEST_ROOT}/run/sing-box.pid" ]
touch "${TEST_ROOT}/run/atpd.sock"
run_service stop
[ ! -e "${TEST_ROOT}/run/atpd.sock" ]

if FAKE_SINGBOX_CONFLICT=1 run_service start >/dev/null 2>&1; then
    echo "service accepted a conflicting sing-box process" >&2
    exit 1
fi

echo "Android service script tests passed"
