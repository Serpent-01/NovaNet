#编写 Python “便秘型”缓慢客户端

import socket
import time

def slow_receive():
    print("准备连接到服务器...")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    
    # 故意把客户端系统层的 TCP 接收缓冲区设置积极小 (例如 4096 字节)
    # 这会迅速让 TCP 窗口变为 0，逼迫服务器触发 EAGAIN
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    
    s.connect(('127.0.0.1', 8080))
    print("连接成功！服务器应该在疯狂发数据了。")
    print("客户端开始休眠 3 秒，让服务器的发送窗口彻底堵死！")
    
    time.sleep(3) # 故意睡 3 秒，彻底憋死服务器
    
    print("开始以极慢的速度接收数据 (每次只收 8KB)...")
    total_received = 0
    
    # 50MB = 52,428,800 bytes
    target_bytes = 50 * 1024 * 1024
    
    try:
        while total_received < target_bytes:
            data = s.recv(8192) # 每次最多只读 8KB
            if not data:
                print("服务器断开了连接！")
                break
            
            total_received += len(data)
            
            if total_received % (1024 * 1024) == 0:
                print(f"已接收: {total_received / (1024*1024)} MB / 50 MB")
                
            # 模拟极其缓慢的处理速度
            time.sleep(0.01) 
            
    except KeyboardInterrupt:
        print("\n强制退出。")
        
    print(f"接收结束。总计接收: {total_received} bytes")
    s.close()

if __name__ == '__main__':
    slow_receive()