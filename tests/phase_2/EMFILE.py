#测试 Acceptor 的空闲 fd 逃生舱机制。

import socket
import time

conns = []
for i in range(100):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect(('127.0.0.1', 8080))
        conns.append(s)
        print(f"Connected {i}")
    except Exception as e:
        print(f"Failed at {i}: {e}")
    time.sleep(0.01)

time.sleep(1000)