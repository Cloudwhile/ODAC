import socket
import sys
import time

path, ready_path = sys.argv[1], sys.argv[2]
server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(path)
server.listen(0)
filler = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
filler.connect(path)
with open(ready_path, "w", encoding="ascii") as ready:
    ready.write("READY\n")
while True:
    time.sleep(3600)
