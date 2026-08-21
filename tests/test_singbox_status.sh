#!/usr/bin/env bash
set -eu

binary=${1:?usage: test_singbox_status.sh /path/to/atpd}
test_root=$(mktemp -d /tmp/atpd-singbox-status-test.XXXXXX)
case "$test_root" in
    /tmp/atpd-singbox-status-test.*) ;;
    *) echo "unsafe temporary path: $test_root" >&2; exit 1 ;;
esac
server_pid=
cleanup() {
    [ -z "$server_pid" ] || kill "$server_pid" 2>/dev/null || true
    rm -rf -- "$test_root"
}
trap cleanup EXIT
mkdir -p "$test_root/run"
socket_path="$test_root/run/atpd.sock"
config_path="$test_root/atp.conf"

python3 - "$socket_path" <<'PY' &
import os
import socket
import sys

path = sys.argv[1]
try:
    os.unlink(path)
except FileNotFoundError:
    pass
server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(path)
server.listen(1)
client, _ = server.accept()
if client.recv(128) != b"sing-box status\n":
    raise SystemExit("unexpected UDS command")
client.sendall(b"""SINGBOX_STATUS 0
sing-box                                      HEALTHY

Core
  State               RUNNING
  API                 REACHABLE
  Endpoint            http://127.0.0.1:9090
  PID / Uptime        123 / 00:01:00
  Version             1.12.0
  Mode                Rule
  eBPF                redirect, auto
  Config              /tmp/config.json
  RSS / Threads / FDs 10.0 MiB / 2 / 8
  Restarts            1
  Last error          none

Overall               HEALTHY
""")
client.close()
server.close()
PY
server_pid=$!
for _ in $(seq 1 100); do
    [ -S "$socket_path" ] && break
    sleep 0.01
done
printf 'ATP_DATA=%s\n' "$test_root" > "$config_path"
uds_output=$("$binary" -c "$config_path" sing-box status)
wait "$server_pid"
server_pid=
case "$uds_output" in
    *"State               RUNNING"*"RSS / Threads / FDs 10.0 MiB / 2 / 8"*"Restarts            1"*) ;;
    *) echo "incomplete UDS sing-box status" >&2; exit 1 ;;
esac

rm -f -- "$socket_path"
port_file="$test_root/api.port"
python3 - "$port_file" <<'PY' &
import socket
import sys

port_file = sys.argv[1]
server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.bind(("127.0.0.1", 0))
server.listen(2)
with open(port_file, "w", encoding="ascii") as output:
    output.write(str(server.getsockname()[1]))

valid = True
for _ in range(2):
    client, _ = server.accept()
    request = b""
    while b"\r\n\r\n" not in request:
        chunk = client.recv(2048)
        if not chunk:
            break
        request += chunk
    valid &= b"Authorization: Bearer test-secret\r\n" in request
    if request.startswith(b"GET /version "):
        body = b'{"version":"1.12.0"}'
    elif request.startswith(b"GET /configs "):
        body = b'{"mode":"Rule"}'
    else:
        valid = False
        body = b'{}'
    response = (b"HTTP/1.1 200 OK\r\nContent-Length: " +
                str(len(body)).encode() + b"\r\nConnection: close\r\n\r\n" + body)
    client.sendall(response)
    client.close()
server.close()
raise SystemExit(0 if valid else 1)
PY
server_pid=$!
for _ in $(seq 1 100); do
    [ -s "$port_file" ] && break
    sleep 0.01
done
api_port=$(cat "$port_file")
cat > "$config_path" <<EOF
ATP_DATA=$test_root
API_HOST=127.0.0.1
API_PORT=$api_port
CLASH_SECRET=test-secret
EOF
api_output=$("$binary" -c "$config_path" sing-box status)
wait "$server_pid"
server_pid=
case "$api_output" in
    *"API                 REACHABLE"*"Version             1.12.0"*"Mode                Rule"*"Overall               HEALTHY"*) ;;
    *) echo "incomplete direct sing-box status" >&2; exit 1 ;;
esac

echo "sing-box status tests passed"
