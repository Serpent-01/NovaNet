import socket, time
sockets = []
for i in range(50):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect(('127.0.0.1', 8080))
    sockets.append(s)
time.sleep(100)