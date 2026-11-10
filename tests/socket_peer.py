import json
import socket
import sys

path, log_path, issued, ready_path = sys.argv[1:5]
with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
    server.bind(path)
    server.listen(8)
    with open(ready_path, "w", encoding="ascii") as ready:
        ready.write("READY\n")
    while True:
        connection, _ = server.accept()
        with connection:
            connection.settimeout(5)
            data = b""
            while not data.endswith(b"\n"):
                chunk = connection.recv(65536)
                if not chunk:
                    break
                data += chunk
                if len(data) > 65536:
                    sys.exit("test peer received an oversized request")
            request = json.loads(data)
            with open(log_path, "a", encoding="utf-8") as log:
                log.write(json.dumps(request, separators=(",", ":"), sort_keys=True) + "\n")
            payload = {"token": issued, "expires_at": 1} if request.get("method") == "login" else {}
            reply = {"ok": True, "data": payload}
            connection.sendall(json.dumps(reply).encode() + b"\n")
