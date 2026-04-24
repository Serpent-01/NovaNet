import socket
import threading
import time

TARGET_IP = "127.0.0.1"
TARGET_PORT = 8080
CONNECTIONS = 10000

def connect_and_send(client_id):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect((TARGET_IP, TARGET_PORT))
        s.sendall(f"Ping from client {client_id}\n".encode())
        # 保持连接 2 秒钟，给服务器施压
        time.sleep(2)
        s.close()
    except Exception as e:
        print(f"Client {client_id} failed: {e}")

threads = []
print(f"Initiating {CONNECTIONS} concurrent connections...")

# 瞬间爆破
for i in range(CONNECTIONS):
    t = threading.Thread(target=connect_and_send, args=(i,))
    threads.append(t)
    t.start()

for t in threads:
    t.join()

print("Stress test completed.")