import socket
import threading
import time

HOST = '127.0.0.1'
PORT = 8080
NUM_CONNECTIONS = 150

conns = []

def connect_to_server():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect((HOST, PORT))
        conns.append(s)
    except Exception as e:
        # 如果服务端满了拒绝连接，会走到这里
        pass

threads = []
print(f"🚀 准备瞬间发射 {NUM_CONNECTIONS} 个并发连接...")

# 多线程齐发，瞬间打满服务器全连接队列
for _ in range(NUM_CONNECTIONS):
    t = threading.Thread(target=connect_to_server)
    threads.append(t)
    t.start()

for t in threads:
    t.join()

print(f"✅ 成功建立并保持了 {len(conns)} 个连接！")
print("保持连接中... 请去观察服务器日志和 CPU 占用率。10秒后自动断开。")
time.sleep(10)

for s in conns:
    s.close()
print("💥 客户端已全部断开。")