#!/system/bin/sh
# ATP Service Script for KernelSU / Magisk / APatch
#

# 1. Wait for Android boot completed
until [ "$(getprop sys.boot_completed)" = "1" ]; do
    sleep 3
done

# 2. Short delay for network/route initialization
sleep 2

# 3. Auto-detect atpd location
for candidate in /data/adb/atp/atpd /data/adb/sing-box/atpd; do
    if [ -f "${candidate}" ]; then
        chmod +x "${candidate}"
        "${candidate}" start
        break
    fi
done

exit 0
