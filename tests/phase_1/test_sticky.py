import socket
import time

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(("127.0.0.1", 8080))

print("开始触发粘包攻击...")
# 模拟应用层快速发送 5 条独立指令
s.sendall(b"CMD1")
s.sendall(b"CMD2")
s.sendall(b"CMD3")
s.sendall(b"CMD4")
s.sendall(b"CMD5")

# 看看你的 Echo Server 吐回来了什么？
response = s.recv(1024)
print(f"服务器返回: {response}")