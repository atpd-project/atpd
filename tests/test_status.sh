#!/usr/bin/env bash
set -eu

binary=${1:?usage: test_status.sh /path/to/atpd}
test_root=$(mktemp -d /tmp/atpd-status-test.XXXXXX)
case "$test_root" in
    /tmp/atpd-status-test.*) ;;
    *) echo "unsafe temporary path: $test_root" >&2; exit 1 ;;
esac
trap 'rm -rf -- "$test_root"' EXIT
mkdir -p "$test_root/run"
socket_path="$test_root/run/atpd.sock"

serve_status() {
    local status=$1
    rm -f -- "$socket_path"
    python3 - "$socket_path" "$status" <<'PY' &
import os
import socket
import sys

path = sys.argv[1]
status = sys.argv[2]
try:
    os.unlink(path)
except FileNotFoundError:
    pass
server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(path)
server.listen(1)
client, _ = server.accept()
if client.recv(128) != b"status\n":
    raise SystemExit("unexpected UDS command")
client.sendall(f"""ATPD_STATUS {status}
ATPd
Clash policy
  Configured mode     Rule
  Desired mode        Direct
  Applied mode        Direct
Monitors
  Netlink             ACTIVE
  XFRM listener       ACTIVE
  FCM                 ACTIVE (waiting)
VPN
  Google VPN          DISCONNECTED
  Other VPN           CONNECTED
  Other interface     tun0
  Other  RX / TX      1.0 MiB / 2.0 MiB
State machines
  Reactor             RUNNING
  VPN                 IDLE (5s, 2 transitions)
  Direct Wi-Fi        DIRECT (3s, 1 transitions)
  Current SSID        Home
  Direct SSID         Home
  Restore mode        Rule
System
  CPU temperature     31 C

Overall               HEALTHY
""".encode())
client.close()
server.close()
PY
    server_pid=$!
    for _ in $(seq 1 100); do
        [ -S "$socket_path" ] && return
        sleep 0.01
    done
    echo "fake status server did not start" >&2
    return 1
}

run_status() {
    "$binary" -c <(printf 'ATP_DATA=%s\nOBSOLETE_OPTION=legacy\n' "$test_root") status
}

for expected in 0 1; do
    serve_status "$expected"
    set +e
    output=$(run_status)
    actual=$?
    set -e
    wait "$server_pid"
    [ "$actual" -eq "$expected" ]
    case "$output" in
        *"Configured mode     Rule"*"Desired mode        Direct"*"FCM                 ACTIVE (waiting)"*"Google VPN          DISCONNECTED"*"Other VPN           CONNECTED"*"VPN                 IDLE"*"Direct Wi-Fi        DIRECT"*"Restore mode        Rule"*"Overall               HEALTHY") ;;
        *) echo "incomplete ATPd status output" >&2; exit 1 ;;
    esac
done

rm -f -- "$socket_path"
set +e
offline_output=$(run_status)
offline_status=$?
set -e
[ "$offline_status" -eq 2 ]
case "$offline_output" in
    *"Overall               STOPPED"*) ;;
    *) echo "missing offline status output" >&2; exit 1 ;;
esac

echo "status protocol tests passed"
