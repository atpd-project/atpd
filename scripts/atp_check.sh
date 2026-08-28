#!/system/bin/sh
# ATPd quick status for Android root shells.

ATP_ROOT="${ATP_ROOT:-/data/adb/atp}"
ATPD="${ATP_ROOT}/atpd"
ATP_CONF="${ATP_ROOT}/atp.conf"

if [ ! -x "${ATPD}" ]; then
    echo "Missing executable: ${ATPD}" >&2
    exit 1
fi

"${ATPD}" -c "${ATP_CONF}" -n status

echo ""
echo "Recent ATPd logs:"
tail -n 10 "${ATP_ROOT}/run/atp.log" 2>/dev/null || echo "No ATPd log found"
