#验证服务器生命周期管理（防 Use-After-Free 和 Double Close）。
import socket
import time
import os
import signal

HOST = '127.0.0.1'
PORT = 8080 # 替换为你的服务器端口

print("=== 必测 13: 正常关闭 (Graceful Close) ===")
s1 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s1.connect((HOST, PORT))
s1.send(b"Hello")
time.sleep(0.1) # 等待服务器回显
s1.close() # 正常发送 TCP FIN
print("[PASS] 客户端正常关闭完毕")
time.sleep(1)

print("\n=== 必测 14: 暴力断开 (Abnormal Disconnect/Kill) ===")
# 采用子进程模拟，建立连接后直接自杀！这会产生极其生硬的 TCP RST 包。
pid = os.fork()
if pid == 0:
    s2 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s2.connect((HOST, PORT))
    s2.send(b"Half message...")
    os.kill(os.getpid(), signal.SIGKILL) # 直接暴力自杀，不留遗言
else:
    os.waitpid(pid, 0)
    print("[PASS] 客户端已暴力自杀")

print("\n[验收]: 请去查看服务器日志，如果不崩，且能打印出清理日志，就算过！")