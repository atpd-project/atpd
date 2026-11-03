#!/system/bin/sh
# On-device smoke test: swap in the Rust atpd, exercise it, always restore the C binary.
# Run as root on the phone:  sh pixel-smoke.sh /data/local/tmp/atpd-rust [--vpn]
# Writes a report to /data/local/tmp/atpd-smoke-report.txt. Never edits atp.conf or config.json.

set -u
ROOT=${ATP_ROOT:-/data/adb/atp}
NEW=${1:-}
VPN=${2:-}
RUN=$ROOT/run
BACKUP=$ROOT/atpd.c-backup
REPORT=/data/local/tmp/atpd-smoke-report.txt
PASS=0
FAIL=0

: > "$REPORT"
log() { echo "$*" | tee -a "$REPORT"; }
ok() { PASS=$((PASS + 1)); log "PASS  $1"; }
bad() { FAIL=$((FAIL + 1)); log "FAIL  $1"; }
check() { name=$1; shift; if "$@" >> "$REPORT" 2>&1; then ok "$name"; else bad "$name"; fi; }

conf_args() { [ -f "$ROOT/atp.conf" ] && echo "-c $ROOT/atp.conf"; }
atpd() { "$ROOT/atpd" $(conf_args) "$@"; }
pid_of() { [ -r "$RUN/$1.pid" ] && IFS= read -r p < "$RUN/$1.pid" && [ -n "$p" ] && kill -0 "$p" 2>/dev/null && echo "$p"; }
status_text() { atpd -n status 2>&1; }
wait_for() { # seconds, command...
    n=$1; shift
    while [ "$n" -gt 0 ]; do "$@" >/dev/null 2>&1 && return 0; sleep 1; n=$((n - 1)); done
    return 1
}
healthy() { status_text | grep -q 'RUNNING / healthy'; }
stopped() { [ -z "$(pid_of atpd)" ] && [ -z "$(pid_of sing-box)" ]; }

restore() {
    [ "${KEEP_RUST:-0}" = 1 ] && { log "INFO  KEEP_RUST=1: Rust atpd left installed"; return; }
    [ -f "$BACKUP" ] || return
    atpd stop >/dev/null 2>&1
    wait_for 20 stopped
    cp "$BACKUP" "$ROOT/atpd" && chmod 755 "$ROOT/atpd" && rm -f "$BACKUP"
    atpd start >/dev/null 2>&1
    if wait_for 30 healthy; then log "INFO  restored C atpd and it is healthy"; else log "WARN  restored C atpd but it is not healthy yet; check '$ROOT/atpd -n status'"; fi
    log "RESULT pass=$PASS fail=$FAIL report=$REPORT"
}

[ "$(id -u)" = 0 ] || { echo "must run as root (su)"; exit 2; }
[ -n "$NEW" ] && [ -f "$NEW" ] || { echo "usage: sh $0 /path/to/atpd-rust [--vpn]"; exit 2; }
[ -x "$ROOT/atpd" ] || { echo "no existing atpd at $ROOT/atpd"; exit 2; }
# A leftover backup means a previous run did not restore; never overwrite the real C binary with it.
[ -f "$BACKUP" ] && { echo "$BACKUP exists: restore it manually (cp it to $ROOT/atpd) before rerunning"; exit 2; }

log "INFO  device: $(getprop ro.product.model) / Android $(getprop ro.build.version.release) / $(uname -r)"
log "INFO  C atpd: $("$ROOT/atpd" version 2>&1 | head -1)"

atpd stop >/dev/null 2>&1
wait_for 20 stopped || { log "FAIL  could not stop the running C atpd; aborting without changes"; exit 1; }
cp "$ROOT/atpd" "$BACKUP" || exit 1
trap restore EXIT
trap 'exit 130' INT TERM HUP
cp "$NEW" "$ROOT/atpd" && chmod 755 "$ROOT/atpd" || { bad "install Rust binary"; exit 1; }
log "INFO  Rust atpd: $("$ROOT/atpd" version 2>&1 | head -1)"

check "1 version runs" "$ROOT/atpd" version
check "1 config check passes" atpd check

atpd start >> "$REPORT" 2>&1
if wait_for 45 healthy; then ok "2 start -> sing-box RUNNING / healthy"; else bad "2 start -> sing-box RUNNING / healthy"; fi
log "----- status -----"; status_text >> "$REPORT"; log "------------------"

mode=$(stat -c %a "$RUN/atpd.sock" 2>/dev/null)
if [ "$mode" = 600 ]; then ok "3 socket mode 0600"; else bad "3 socket mode 0600 (got '$mode')"; fi
sb=$(pid_of sing-box)
if [ -n "$sb" ]; then
    log "INFO  sing-box $(grep -E '^(Uid|Gid|Groups):' /proc/"$sb"/status | tr '\t\n' '  ')"
    grep -q '^Uid:[[:space:]]*0[[:space:]]' /proc/"$sb"/status && ok "3 sing-box uid 0" || bad "3 sing-box uid 0"
    if grep -q '^CORE_USER_GROUP' "$ROOT/atp.conf" 2>/dev/null; then
        log "INFO  CORE_USER_GROUP is customised; compare Gid above with $(grep '^CORE_USER_GROUP' "$ROOT/atp.conf")"
    else
        grep -q '^Gid:[[:space:]]*3005[[:space:]]' /proc/"$sb"/status && ok "3 sing-box gid net_admin(3005)" || bad "3 sing-box gid net_admin(3005)"
    fi
    um=$(grep '^Umask:' /proc/"$sb"/status | tr -dc '0-9')
    [ "$um" = 0027 ] && ok "3 sing-box umask 0027" || bad "3 sing-box umask 0027 (got '$um')"
else
    bad "3 sing-box process present"
fi

lc=$(logcat -d -s atpd 2>/dev/null | tail -n 5)
if [ -n "$lc" ]; then ok "4 logcat has atpd lines"; else bad "4 logcat has atpd lines"; fi
echo "$lc" >> "$REPORT"
last=$(grep -E '^\[[0-9]{4}-' "$RUN/atp.log" 2>/dev/null | tail -n 1 | cut -c2-14)
now=$(date '+%Y-%m-%d %H')
# Can misfire within a minute of an hour boundary; rerun if so.
if [ "$last" = "$now" ]; then ok "4 log timestamps use local time"; else bad "4 log timestamps use local time (log '$last' vs date '$now')"; fi

if [ "$VPN" = --vpn ]; then
    log "ACTION  turn your secondary VPN (WARP / Google VPN) ON now - waiting up to 120 s"
    if wait_for 120 sh -c "! $ROOT/atpd $(conf_args) -n status 2>&1 | grep -q 'STANDALONE / DIRECT'"; then
        ok "5 VPN on -> state leaves STANDALONE"
        ip rule 2>/dev/null | grep 12400 >> "$REPORT" && ok "5 bypass rules at pref 12400" || log "INFO  no pref-12400 rules (expected only for ipsec*/Google VPN)"
    else
        bad "5 VPN on -> state leaves STANDALONE"
    fi
    log "ACTION  turn the VPN OFF now - waiting up to 120 s"
    if wait_for 120 sh -c "$ROOT/atpd $(conf_args) -n status 2>&1 | grep -q 'STANDALONE / DIRECT'"; then ok "5 VPN off -> STANDALONE / DIRECT"; else bad "5 VPN off -> STANDALONE / DIRECT"; fi
    grep 'VPN_STATE:' "$RUN/atp.log" | tail -n 6 >> "$REPORT"
else
    log "SKIP  5 VPN transitions (rerun with --vpn when someone can toggle the VPN)"
fi

old=$(pid_of sing-box)
[ -n "$old" ] && kill -9 "$old"
if wait_for 45 sh -c "p=\$(cat $RUN/sing-box.pid 2>/dev/null); [ -n \"\$p\" ] && [ \"\$p\" != '$old' ] && kill -0 \"\$p\"" && wait_for 45 healthy; then
    ok "6 sing-box restarted after kill -9"
else
    bad "6 sing-box restarted after kill -9"
fi

d=$(pid_of atpd)
lines=$(wc -l < "$RUN/atp.log")
kill -HUP "$d" 2>/dev/null
sleep 3
if [ "$(pid_of atpd)" = "$d" ] && tail -n +"$((lines + 1))" "$RUN/atp.log" | grep -q 'Config reload completed successfully'; then
    ok "7 SIGHUP reload succeeds, daemon stays up"
else
    bad "7 SIGHUP reload succeeds, daemon stays up"
    tail -n +"$((lines + 1))" "$RUN/atp.log" | tail -n 10 >> "$REPORT"
fi

log "INFO  idling 60 s for RSS"
sleep 60
rss=$(grep '^VmRSS:' /proc/"$(pid_of atpd)"/status 2>/dev/null | tr -dc '0-9')
log "INFO  atpd VmRSS ${rss:-?} kB (plan target < 4096, hard ceiling < 6144)"
[ -n "$rss" ] && [ "$rss" -lt 6144 ] && ok "9 RSS under 6 MB ceiling" || bad "9 RSS under 6 MB ceiling"

atpd stop >> "$REPORT" 2>&1
if wait_for 20 stopped; then ok "8 stop exits both processes"; else bad "8 stop exits both processes"; fi
leftover=$(ls "$RUN"/atpd.pid "$RUN"/sing-box.pid "$RUN"/atpd.sock 2>/dev/null)
[ -z "$leftover" ] && ok "8 pid files and socket removed" || bad "8 pid files and socket removed (left: $leftover)"
log "----- last 30 lines of atp.log -----"; tail -n 30 "$RUN/atp.log" >> "$REPORT" 2>&1
# restore() runs on exit and prints RESULT.
[ "$FAIL" = 0 ]
