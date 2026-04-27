import socket
import time

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(("127.0.0.1", 8080))

print("开始触发半包攻击...")

# 1. 发前半截，立刻收！
s.sendall(b"Hello")
print(f"立刻收到前半截响应: {s.recv(1024)}") 

# 2. 模拟网络延迟卡顿
print("网络卡顿 1 秒...")
time.sleep(1)

# 3. 发后半截，立刻收！
s.sendall(b"World")
print(f"立刻收到后半截响应: {s.recv(1024)}")