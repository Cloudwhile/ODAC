#!/bin/sh
set -eu
BUILD=${1:?usage: boundary_test.sh BUILD_DIR}
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d)
PEER=
trap '[ -z "$PEER" ] || kill "$PEER" 2>/dev/null || true; rm -rf "$WORK"' EXIT INT TERM
SOCK="$WORK/control.sock"
LOG="$WORK/requests.log"
FAILURES=0
COOKIE_VALUE=cookievalue0001
ISSUED_VALUE=issuedvalue0002

check() {
    if [ "$2" = "$3" ]; then echo "ok - $1"; else echo "not ok - $1: expected [$3] got [$2]"; FAILURES=$((FAILURES + 1)); fi
}
contains() {
    case "$2" in *"$3"*) echo "ok - $1";; *) echo "not ok - $1: [$3] missing in [$2]"; FAILURES=$((FAILURES + 1));; esac
}
absent() {
    case "$2" in *"$3"*) echo "not ok - $1: [$3] present in [$2]"; FAILURES=$((FAILURES + 1));; *) echo "ok - $1";; esac
}
requests() { if [ -f "$LOG" ]; then wc -l < "$LOG" | tr -d ' '; else echo 0; fi; }

mkfifo "$WORK/ready"
python3 "$HERE/socket_peer.py" "$SOCK" "$LOG" "$ISSUED_VALUE" "$WORK/ready" &
PEER=$!
ready=$(timeout 5 head -n 1 "$WORK/ready")
[ "$ready" = READY ] || { echo 'test peer failed to become ready' >&2; exit 1; }

CONFIG="$WORK/config.json"
cat > "$CONFIG" <<EOF
{"enabled":1,"interface":"br-odac","gateway":"192.168.43.1","portal_hostname":"portal.odac.lan",
 "tls_cert":"/etc/odac/portal.crt","tls_key":"/etc/odac/portal.key","socket_path":"$SOCK",
 "status_file":"/tmp/odac-test-upstream.status"}
EOF
chmod 0600 "$CONFIG"

portal() {
    action=$1 method=$2 body=$3
    shift 3
    printf '%s' "$body" | env -i ODAC_TEST_CONFIG="$CONFIG" QUERY_STRING="action=$action" REQUEST_METHOD="$method" \
        REMOTE_ADDR=192.168.43.20 HTTP_HOST=portal.odac.lan:8443 HTTP_ORIGIN=https://portal.odac.lan:8443 \
        CONTENT_TYPE=application/json CONTENT_LENGTH=${#body} "$@" "$BUILD/odac-portal-test"
}

out=$(portal login POST '{"username":"a","password":"b","csrf":"c"}')
contains "plain HTTP login is rejected" "$out" '"code":"HTTPS_REQUIRED"'
check "plain HTTP login never reaches the daemon" "$(requests)" 0

out=$(portal login POST '{"username":"alice","password":"pw","csrf":"c","token":"forgedvalue","peer":"192.168.43.99","mac":"02:00:00:00:00:09"}' \
    HTTPS=on HTTP_COOKIE="theme=dark; odac_session=$COOKIE_VALUE")
last=$(tail -n 1 "$LOG")
contains "peer comes from REMOTE_ADDR" "$last" '"peer":"192.168.43.20"'
contains "cookie token is injected" "$last" "\"token\":\"$COOKIE_VALUE\""
absent "body token is dropped" "$last" 'forgedvalue'
check "exactly one token field reaches the daemon" "$(printf '%s' "$last" | grep -o '"token":' | wc -l | tr -d ' ')" 1
contains "request is marked https" "$last" '"https":true'
contains "scope is portal" "$last" '"scope":"portal"'
contains "issued token becomes an HttpOnly cookie" "$out" "Set-Cookie: odac_session=$ISSUED_VALUE; Path=/; Secure; HttpOnly; SameSite=Strict"
absent "issued token is removed from the JSON body" "$out" "\"token\""

portal logout POST '{"csrf":"c","token":"forgedalone"}' HTTPS=on > /dev/null
absent "body token without a cookie is dropped" "$(tail -n 1 "$LOG")" 'forgedalone'
before=$(requests)
out=$(portal approve POST '{}' HTTPS=on)
contains "admin method is unreachable from the portal" "$out" '"code":"BAD_REQUEST"'
out=$(portal login GET '' HTTPS=on)
contains "GET login is rejected" "$out" '"code":"METHOD_NOT_ALLOWED"'
out=$(portal status GET '' HTTPS=on REMOTE_ADDR=fe80::1)
contains "IPv6 peer is rejected" "$out" '"code":"UNSUPPORTED_CLIENT"'
out=$(portal login POST '{"username":"a"}' HTTPS=on CONTENT_TYPE=text/plain)
contains "non-JSON content type is rejected" "$out" '"code":"BAD_REQUEST"'
check "locally rejected requests never reach the daemon" "$(requests)" "$before"

out=$(env -i ODAC_TEST_CONFIG="$CONFIG" QUERY_STRING='redir=https://evil.test' HTTP_HOST=evil.test "$BUILD/landing")
contains "landing redirects to the configured hostname" "$out" 'Location: https://portal.odac.lan:8443/'
absent "landing ignores attacker Host and query" "$out" 'evil.test'

out=$(printf '{"ids":[3]}' | "$BUILD/odacctl" --socket "$SOCK" approve)
last=$(tail -n 1 "$LOG")
contains "odacctl uses admin scope" "$last" '"scope":"admin"'
contains "odacctl forwards the params object" "$last" '"params":{"ids":[3]}'
out=$(printf 'not json' | "$BUILD/odacctl" --socket "$SOCK" approve || true)
contains "odacctl rejects invalid params" "$out" '"code":"BAD_REQUEST"'

out=$(printf '{}' | timeout 5 "$BUILD/odacctl" --socket "$SOCK" --wait-ready)
contains "wait-ready succeeds when the daemon is already available" "$out" '"ok":true'
contains "wait-ready requests overview" "$(tail -n 1 "$LOG")" '"method":"overview"'

# A connected daemon that never replies consumes the readiness budget itself.
# No filesystem event follows: polling again must not grant another 20 seconds.
python3 - "$BUILD/odacctl" "$WORK/silent.sock" <<'PY'
import json
import selectors
import socket
import subprocess
import sys
import time

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
    server.bind(sys.argv[2])
    server.listen(1)
    with selectors.DefaultSelector() as ready:
        ready.register(server, selectors.EVENT_READ)
        started = time.monotonic()
        process = subprocess.Popen(
            [sys.argv[1], "--socket", sys.argv[2], "--wait-ready"],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, text=True)
        try:
            assert ready.select(5), "wait-ready never attempted an exchange"
            with server.accept()[0] as connection:
                output, _ = process.communicate(timeout=25)
            elapsed = time.monotonic() - started
            assert 19 <= elapsed < 25, f"readiness deadline took {elapsed:.3f}s"
            assert process.returncode == 1, process.returncode
            assert json.loads(output)["error"]["code"] == "UNAVAILABLE", output
        finally:
            if process.poll() is None:
                process.kill()
            process.wait()
print("ok - wait-ready shares one deadline between exchange and event wait")
PY

# A full accept queue must not block the client past its deadline.
STUCK="$WORK/stuck.sock"
mkfifo "$WORK/stuck.ready"
python3 "$HERE/full_backlog_peer.py" "$STUCK" "$WORK/stuck.ready" &
BLOCKER=$!
trap '[ -z "$PEER" ] || kill "$PEER" 2>/dev/null || true; kill "$BLOCKER" 2>/dev/null || true; rm -rf "$WORK"' EXIT INT TERM
ready=$(timeout 5 head -n 1 "$WORK/stuck.ready")
[ "$ready" = READY ] || { echo 'backlog peer failed to become ready' >&2; exit 1; }
started=$(date +%s)
out=$(printf '{}' | timeout 30 "$BUILD/odacctl" --socket "$STUCK" overview || true)
elapsed=$(( $(date +%s) - started ))
if [ "$elapsed" -lt 25 ]; then echo "ok - odacctl returns within its deadline on a full accept queue"; else echo "not ok - odacctl hung for ${elapsed}s on a full accept queue"; FAILURES=$((FAILURES + 1)); fi
contains "odacctl reports the daemon as unavailable" "$out" '"code":"UNAVAILABLE"'

if [ "$FAILURES" -eq 0 ]; then echo "boundary tests passed"; else echo "$FAILURES boundary test(s) failed"; exit 1; fi
