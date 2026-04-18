#!/system/bin/sh
# SELinux policy injection for ATP (supports both Magisk and KernelSU)

MODULE_NAME="atp"
LOG_TAG="ATP_SEPOLICY"

log_info() {
    echo "[$LOG_TAG] $1"
    log -p i -t "$LOG_TAG" "$1"
}

log_error() {
    echo "[$LOG_TAG] ERROR: $1" >&2
    log -p e -t "$LOG_TAG" "$1"
}

# Detect root manager
detect_root_manager() {
    if [ -d "/data/adb/ksu" ]; then
        echo "ksu"
    elif [ -d "/data/adb/magisk" ]; then
        echo "magisk"
    else
        echo "none"
    fi
}

# Inject SELinux policy for INET_DIAG
setup_selinux_policy() {
    local root_mgr=$(detect_root_manager)
    
    log_info "Detected root manager: $root_mgr"
    
    # Check if magiskpolicy command exists
    if command -v magiskpolicy >/dev/null 2>&1; then
        log_info "Found magiskpolicy, injecting SELinux rules..."
        
        # Allow atpd to use NETLINK_INET_DIAG socket
        magiskpolicy --live "allow atpd self netlink_tcpdiag_socket { create read write nlmsg_read }" 2>/dev/null
        
        # Allow atpd to read socket diag information
        magiskpolicy --live "allow atpd self socket_device { read write }" 2>/dev/null
        
        # Allow atpd to get socket credentials
        magiskpolicy --live "allow atpd self capability { net_admin net_raw }" 2>/dev/null
        
        # Allow atpd to read /proc/net/tcp (fallback method)
        magiskpolicy --live "allow atpd proc_net_tcp file { getattr open read }" 2>/dev/null
        
        # Allow atpd to read /proc/[pid]/fd (for socket lookup)
        magiskpolicy --live "allow atpd proc file { getattr open read }" 2>/dev/null
        
        log_info "SELinux policy injected successfully"
        return 0
    else
        log_error "magiskpolicy not found, SELinux policy not applied"
        log_error "INET_DIAG may fail with EACCES"
        return 1
    fi
}

# Main entry
case "$1" in
    start)
        setup_selinux_policy
        ;;
    stop)
        log_info "SELinux policy injection not needed on stop"
        ;;
    *)
        setup_selinux_policy
        ;;
esac

exit 0
