#!/system/bin/sh
# ATP Quick Check Script for KernelSU

ATP_DATA="/data/adb/atp"
ATP_BIN="${ATP_DATA}/bin"
PID_FILE="${ATP_DATA}/run/atpd.pid"

echo ""
echo "┌─────────────────────────────────────────────┐"
echo "│         ATP Daemon Quick Snapshot          │"
echo "├─────────────────────────────────────────────┤"

# Check daemon status
if [ -f "$PID_FILE" ]; then
    PID=$(cat "$PID_FILE" 2>/dev/null)
    if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
        echo "│ Status:  \033[1;32mRUNNING\033[0m (PID: $PID)                 │"
    else
        echo "│ Status:  \033[1;31mOFFLINE\033[0m (Stale PID file)            │"
    fi
else
    echo "│ Status:  \033[1;31mOFFLINE\033[0m (Not running)                 │"
fi

# Get proxy mode
if [ -x "${ATP_BIN}/atpd" ]; then
    MODE=$(${ATP_BIN}/atpd core status 2>/dev/null |
        awk '$1 == "Mode" { print $2; exit }')
    if [ -n "$MODE" ]; then
        echo "│ Mode:    $MODE                                  │"
    fi
fi

# Check memory usage
if [ -f "$PID_FILE" ]; then
    PID=$(cat "$PID_FILE" 2>/dev/null)
    if [ -n "$PID" ] && [ -d "/proc/$PID" ]; then
        MEM=$(grep -w VmRSS "/proc/$PID/status" 2>/dev/null | awk '{print $2" "$3}')
        echo "│ Memory:  $MEM (atpd)                         │"
    fi
fi

echo "└─────────────────────────────────────────────┘"
echo ""

# Show last 3 log lines
echo "Recent logs (last 3 lines):"
echo "---------------------------------------------"
tail -3 "${ATP_DATA}/run/atp.log" 2>/dev/null || echo "No logs found"
echo "---------------------------------------------"
