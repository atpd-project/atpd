#!/system/bin/sh
# ATPd boot service for KernelSU, Magisk, and APatch.

until [ "$(getprop sys.boot_completed)" = "1" ]; do
    sleep 3
done

sleep 2

ATPD=/data/adb/atp/atpd
if [ -x "${ATPD}" ]; then
    "${ATPD}" start
fi
