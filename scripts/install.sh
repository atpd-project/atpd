#!/system/bin/sh
# ATP Installation Script for Android
# Run with: sh install.sh

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Installation paths
ATP_DIR="/data/adb/atp"
ATP_BIN="${ATP_DIR}/bin"
ATP_CONF="${ATP_DIR}/atp.conf"
ATP_RUN="${ATP_DIR}/run"
ATP_SINGBOX="${ATP_DIR}/sing-box"

# Source paths (relative to script)
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_DIR=$(cd "${SCRIPT_DIR}/.." && pwd)

print_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

check_root() {
    if [ "$(id -u)" != "0" ]; then
        print_error "This script must be run as root"
        exit 1
    fi
    print_info "Root permission confirmed"
}

check_architecture() {
    ARCH=$(uname -m)
    case "$ARCH" in
        aarch64|arm64)
            print_info "Architecture: ARM64 (aarch64)"
            ;;
        armv7l|arm)
            print_info "Architecture: ARM32 (armv7l)"
            ;;
        x86_64)
            print_info "Architecture: x86_64"
            ;;
        *)
            print_warn "Unknown architecture: $ARCH"
            ;;
    esac
}

create_directories() {
    print_info "Creating directories..."
    
    mkdir -p "$ATP_DIR"
    mkdir -p "$ATP_BIN"
    mkdir -p "$ATP_RUN"
    mkdir -p "$ATP_SINGBOX"
    
    chmod 755 "$ATP_DIR"
    chmod 755 "$ATP_BIN"
    chmod 755 "$ATP_RUN"
    chmod 755 "$ATP_SINGBOX"
    
    print_info "Directories created"
}

install_binary() {
    print_info "Installing atpd binary..."
    
    # Check if binary exists in build directory
    if [ -f "${PROJECT_DIR}/build/bin/atpd" ]; then
        cp "${PROJECT_DIR}/build/bin/atpd" "${ATP_BIN}/"
    elif [ -f "${PROJECT_DIR}/atpd" ]; then
        cp "${PROJECT_DIR}/atpd" "${ATP_BIN}/"
    else
        print_error "atpd binary not found. Please build first: make"
        exit 1
    fi
    
    chmod 755 "${ATP_BIN}/atpd"
    print_info "Binary installed: ${ATP_BIN}/atpd"
}

install_config() {
    if [ -f "$ATP_CONF" ]; then
        print_warn "Configuration already exists: $ATP_CONF"
        echo -n "Overwrite? [y/N]: "
        read answer
        case "$answer" in
            y|Y|yes|Yes)
                cp "${PROJECT_DIR}/examples/atp.conf.example" "$ATP_CONF"
                print_info "Configuration overwritten"
                ;;
            *)
                print_info "Keeping existing configuration"
                ;;
        esac
    else
        cp "${PROJECT_DIR}/examples/atp.conf.example" "$ATP_CONF"
        print_info "Configuration installed: $ATP_CONF"
    fi
    
    chmod 644 "$ATP_CONF"
}

install_singbox_config() {
    if [ ! -f "${ATP_SINGBOX}/config.json" ]; then
        print_warn "sing-box eBPF configuration is required: ${ATP_SINGBOX}/config.json"
    else
        print_info "sing-box configuration already exists"
    fi
}

install_init_script() {
    if [ -d "/data/adb/service.d" ]; then
        print_info "Installing Magisk/KernelSU service script..."
        
        cat > "/data/adb/service.d/atp.sh" << EOF
#!/system/bin/sh
# ATP auto-start for Magisk/KernelSU

ATP_BIN="${ATP_BIN}/atpd"

# Wait for system ready
sleep 10

# Start ATP daemon
if [ -x "\$ATP_BIN" ]; then
    \$ATP_BIN start
fi
EOF
        chmod 755 "/data/adb/service.d/atp.sh"
        print_info "Service script installed: /data/adb/service.d/atp.sh"
    else
        print_warn "service.d not found, auto-start not configured"
    fi
}

setup_permissions() {
    print_info "Setting up permissions..."
    
    chown -R root:net_admin "$ATP_DIR" 2>/dev/null || true
    chmod -R 755 "$ATP_BIN" 2>/dev/null || true
    chmod -R 755 "$ATP_RUN" 2>/dev/null || true
    
    print_info "Permissions set"
}

show_summary() {
    echo ""
    echo "=========================================="
    echo "ATP Installation Complete!"
    echo "=========================================="
    echo ""
    echo "Installation path: $ATP_DIR"
    echo "Binary: $ATP_BIN/atpd"
    echo "Config: $ATP_CONF"
    echo ""
    echo "Commands:"
    echo "  Start daemon:   $ATP_BIN/atpd start"
    echo "  Stop daemon:    $ATP_BIN/atpd stop"
    echo "  Check status:   $ATP_BIN/atpd status"
    echo "  Core status:    $ATP_BIN/atpd core status"
    echo ""
    echo "To start ATP now:"
    echo "  su -c '$ATP_BIN/atpd start'"
    echo ""
    echo "To view logs:"
    echo "  cat $ATP_RUN/atp.log"
    echo "  tail -f $ATP_RUN/atp.log"
    echo ""
    echo "=========================================="
}

main() {
    echo ""
    echo "ATP (Advanced Transparent Proxy) Installer"
    echo "=========================================="
    echo ""
    
    check_root
    check_architecture
    create_directories
    install_binary
    install_config
    install_singbox_config
    install_init_script
    setup_permissions
    show_summary
}

main "$@"
