import socket
import time
import statistics

TARGET_IP = "127.0.0.1"
TARGET_PORT = 8080
NUM_PINGS = 10000  # 发送一万次测试包

def test_latency():
    try:
        # 创建客户端 Socket
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        
        # 【极其关键】：客户端也必须关闭 Nagle 算法，否则测试出来的全是客户端的粘滞延迟！
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        
        s.connect((TARGET_IP, TARGET_PORT))
        print(f"Connected to {TARGET_IP}:{TARGET_PORT}. Starting {NUM_PINGS} pings...")
        
        latencies = []
        msg = b"A" * 64  # 模拟一个 64 字节的游戏玩家操作指令
        
        for _ in range(NUM_PINGS):
            start_time = time.perf_counter()  # 获取纳秒级高精度时间
            
            s.sendall(msg)
            data = s.recv(64)
            
            end_time = time.perf_counter()
            
            if data == msg:
                # 记录耗时，转换为毫秒 (ms)
                latencies.append((end_time - start_time) * 1000)
            else:
                print("Data mismatch!")
                
        s.close()
        
        # 统计分析
        print("\n--- Latency Test Results ---")
        print(f"Total Packets : {len(latencies)}")
        print(f"Min Latency   : {min(latencies):.4f} ms")
        print(f"Max Latency   : {max(latencies):.4f} ms")
        print(f"Avg Latency   : {statistics.mean(latencies):.4f} ms")
        print(f"P99 Latency   : {statistics.quantiles(latencies, n=100)[98]:.4f} ms") # 99% 的包延迟低于这个值
        
    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    test_latency()