#!/system/bin/sh
# Quick version display script for Android

ATP_BIN="/data/adb/atp/bin/atpd"

if [ -x "$ATP_BIN" ]; then
    echo "ATP Version Information:"
    echo "========================="
    $ATP_BIN --version
else
    echo "ATP binary not found at $ATP_BIN"
    exit 1
fi
