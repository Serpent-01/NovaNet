#使用我们改造过的 Python 脚本

import socket
import time
import sys

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
# 开启 SO_LINGER，并设置超时为 0。这样 close() 时会强行发 RST 包，而不是正常的 FIN 挥手
s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b'\x01\x00\x00\x00\x00\x00\x00\x00')
s.connect(('127.0.0.1', 8080))

s.send(b"data that server wants to echo back")
# 服务端刚想把这些数据发回来，我们立刻发送 RST 强制切断！
s.close() 
print("Sent RST!")