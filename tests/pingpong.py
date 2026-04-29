import socket
import time

TARGET_IP = '127.0.0.1'
TARGET_PORT = 8080
PAYLOAD = b'x' * 100  # 100 bytes 的 payload
REQUESTS = 1000000     # 100万次请求

def run_pingpong():
    print(f"Connecting to {TARGET_IP}:{TARGET_PORT} for {REQUESTS} ping-pong roundtrips...")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    
    # 【极其关键】客户端也必须关闭 Nagle 算法！否则测试出的延迟是假的！
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    
    try:
        s.connect((TARGET_IP, TARGET_PORT))
    except Exception as e:
        print(f"Connection failed: {e}")
        return

    # 使用高精度计时器
    start_time = time.perf_counter()

    for _ in range(REQUESTS):
        s.sendall(PAYLOAD)
        # 因为我们发了 100 字节，回显服务器一定会原样退回 100 字节
        data = s.recv(100) 

    end_time = time.perf_counter()
    s.close()

    total_time = end_time - start_time
    avg_latency_us = (total_time / REQUESTS) * 1_000_000
    throughput = REQUESTS / total_time

    print("-" * 30)
    print(f"Total time   : {total_time:.4f} s")
    print(f"Requests     : {REQUESTS}")
    print(f"Avg Latency  : {avg_latency_us:.2f} us (微秒)")
    print(f"Throughput   : {throughput:.2f} req/s")

if __name__ == "__main__":
    run_pingpong()