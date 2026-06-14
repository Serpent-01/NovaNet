import socket
import time

TARGET_IP = '127.0.0.1'
TARGET_PORT = 8080

def run_bad_client():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((TARGET_IP, TARGET_PORT))
    
    # 构造一块 10MB 的巨大数据
    huge_data = b'X' * (10 * 1024 * 1024) 
    
    print("1. 瞬间向服务器扔过去 10MB 数据...")
    s.sendall(huge_data)
    print("   发送完毕！")
    
    print("2. 现在我开始装死，每秒钟只读取 1 个字节...")
    print("   去看看你服务端的日志吧，内核缓冲区马上就要爆了！")
    
    # 故意读得极其缓慢，逼迫服务器的内核发送缓冲区爆满！
    while True:
        try:
            data = s.recv(1) # 每次只读 1 个字节！
            time.sleep(1)    # 睡 1 秒！
        except Exception:
            break

if __name__ == "__main__":
    run_bad_client()