#!/system/bin/sh
# ATP Service Script for KernelSU

ATP_BIN="/data/adb/atp/bin"
ATP_DATA="/data/adb/atp"

# Load SELinux policy
if [ -f "$ATP_BIN/sepolicy_atp.sh" ]; then
    sh "$ATP_BIN/sepolicy_atp.sh" start
fi

# Start ATP daemon
$ATP_BIN/atpd start

exit 0
