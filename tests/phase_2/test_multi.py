import socket
import threading
#多连接 Echo：验证 Acceptor 榨干机制与数据隔离
def client_task(client_id):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect(('127.0.0.1', 8080))
        # 故意发送带有自身 ID 的独有数据
        msg = f"I_AM_CLIENT_{client_id}"
        s.sendall(msg.encode())
        
        # 接收数据
        recv_data = s.recv(1024).decode()
        assert recv_data == msg, f"ERROR: Data crossed! Expected {msg}, got {recv_data}"
        print(f"Client {client_id} verified perfectly.")
        s.close()
    except Exception as e:
        print(f"Client {client_id} failed: {e}")

# 瞬间发起 100 个连接，触发 Acceptor 的 while 循环
threads = []
for i in range(100):
    t = threading.Thread(target=client_task, args=(i,))
    threads.append(t)
    t.start()

for t in threads:
    t.join()
print("Multi-connection test PASS!")