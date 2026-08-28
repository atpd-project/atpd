#!/system/bin/sh
# ATPd boot/service entry for KernelSU, Magisk, and APatch.

ATP_ROOT="${ATP_ROOT:-/data/adb/atp}"
ATPD="${ATP_ROOT}/atpd"
SING_BOX="${ATP_ROOT}/bin/sing-box"
ATP_CONF="${ATP_ROOT}/atp.conf"
SING_BOX_CONF="${ATP_ROOT}/config.json"
RUN_DIR="${ATP_ROOT}/run"
ATPD_PID_FILE="${RUN_DIR}/atpd.pid"
SING_BOX_PID_FILE="${RUN_DIR}/sing-box.pid"
ATPD_SOCKET="${RUN_DIR}/atpd.sock"
BOOT_LOG="${RUN_DIR}/boot.log"
BOOT_WAIT="${BOOT_WAIT:-180}"
START_WAIT="${START_WAIT:-45}"

mkdir -p "${RUN_DIR}" 2>/dev/null

write_log() {
    message="$(date '+%Y-%m-%d %H:%M:%S') $*"
    printf '%s\n' "${message}" >> "${BOOT_LOG}" 2>/dev/null
    if command -v log >/dev/null 2>&1; then
        log -p i -t ATPD "${message}" 2>/dev/null
    fi
}

fail() {
    write_log "ERROR: $*"
    printf 'ATPD: %s\n' "$*" >&2
    return 1
}

pid_alive() {
    pid_file="$1"
    [ -r "${pid_file}" ] || return 1
    IFS= read -r pid < "${pid_file}"
    case "${pid}" in
        ''|*[!0-9]*) return 1 ;;
    esac
    kill -0 "${pid}" 2>/dev/null
}

pid_matches() {
    pid_file="$1"
    expected_name="$2"
    pid_alive "${pid_file}" || return 1
    IFS= read -r pid < "${pid_file}"
    [ -r "/proc/${pid}/comm" ] || return 1
    IFS= read -r process_name < "/proc/${pid}/comm"
    [ "${process_name}" = "${expected_name}" ]
}

check_atpd_layout() {
    [ "$(id -u)" = "0" ] || { fail "must run as root"; return 1; }
    [ -x "${ATPD}" ] || { fail "missing executable: ${ATPD}"; return 1; }
    [ -r "${ATP_CONF}" ] || { fail "missing configuration: ${ATP_CONF}"; return 1; }
}

check_layout() {
    check_atpd_layout || return 1
    [ -x "${SING_BOX}" ] || { fail "missing executable: ${SING_BOX}"; return 1; }
    [ -r "${SING_BOX_CONF}" ] || { fail "missing configuration: ${SING_BOX_CONF}"; return 1; }
}

wait_for_boot() {
    command -v getprop >/dev/null 2>&1 || return 0
    waited=0
    while [ "$(getprop sys.boot_completed 2>/dev/null)" != "1" ]; do
        if [ "${waited}" -ge "${BOOT_WAIT}" ]; then
            fail "Android boot did not complete"
            return 1
        fi
        sleep 2
        waited=$((waited + 2))
    done
}

check_config() {
    check_layout || return 1
    "${ATPD}" -c "${ATP_CONF}" check || return 1
    "${SING_BOX}" check -c "${SING_BOX_CONF}" -D "${ATP_ROOT}" || return 1
    "${ATPD}" version
    "${SING_BOX}" version
}

start_daemon() {
    check_layout || return 1
    wait_for_boot || return 1

    if pid_matches "${ATPD_PID_FILE}" atpd &&
       pid_matches "${SING_BOX_PID_FILE}" sing-box; then
        write_log "already running"
        return 0
    fi

    if pid_matches "${ATPD_PID_FILE}" atpd; then
        fail "atpd is running but sing-box is not ready"
        return 1
    fi

    if command -v pidof >/dev/null 2>&1 && pidof sing-box >/dev/null 2>&1; then
        fail "another sing-box is running; disable the old module first"
        return 1
    fi

    write_log "starting atpd"
    "${ATPD}" -c "${ATP_CONF}" start >> "${BOOT_LOG}" 2>&1 || return 1

    waited=0
    while [ "${waited}" -lt "${START_WAIT}" ]; do
        if pid_matches "${ATPD_PID_FILE}" atpd &&
           pid_matches "${SING_BOX_PID_FILE}" sing-box; then
            "${ATPD}" -c "${ATP_CONF}" -n status > "${RUN_DIR}/startup-status.log" 2>&1
            if grep -q "API Engine" "${RUN_DIR}/startup-status.log" &&
               grep -Eq 'Goroutines[[:space:]]+[1-9][0-9]*' "${RUN_DIR}/startup-status.log"; then
                write_log "ready"
                return 0
            fi
        fi
        sleep 1
        waited=$((waited + 1))
    done

    "${ATPD}" -c "${ATP_CONF}" -n status >> "${BOOT_LOG}" 2>&1
    "${ATPD}" -c "${ATP_CONF}" stop >> "${BOOT_LOG}" 2>&1
    fail "startup timed out after ${START_WAIT}s"
}

stop_daemon() {
    check_atpd_layout || return 1
    if pid_matches "${ATPD_PID_FILE}" atpd; then
        "${ATPD}" -c "${ATP_CONF}" stop
    elif pid_matches "${SING_BOX_PID_FILE}" sing-box; then
        fail "orphaned sing-box is still running; inspect it before cleanup"
        return 1
    else
        rm -f "${ATPD_PID_FILE}" "${SING_BOX_PID_FILE}" "${ATPD_SOCKET}"
        write_log "already stopped"
    fi
}

case "${1:-start}" in
    start)   start_daemon ;;
    stop)    stop_daemon ;;
    restart) stop_daemon && start_daemon ;;
    status)  check_atpd_layout && "${ATPD}" -c "${ATP_CONF}" -n status ;;
    check)   check_config ;;
    *)
        echo "Usage: $0 {start|stop|restart|status|check}" >&2
        exit 2
        ;;
esac
