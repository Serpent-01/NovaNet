import socket
import time

def test_loop_affinity():
    print("Connecting to server...")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect(('127.0.0.1', 8080))
    
    print("Sending 1000 messages on the SAME connection...")
    for i in range(1, 1001):
        msg = f"Ping {i}\n".encode()
        s.send(msg)
        s.recv(1024) # 等待 echo 返回，确保一问一答
        
    print("Done! Check your server logs.")
    s.close()

if __name__ == '__main__':
    test_loop_affinity()