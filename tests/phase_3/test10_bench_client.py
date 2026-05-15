import multiprocessing
import socket
import time

# 压力参数
TARGET_IP = '127.0.0.1'
TARGET_PORT = 8080
CONNS_PER_PROC = 50  # 每个进程建立的连接数
MSG = b"ping" * 250  # 1KB 数据包
DURATION = 30        # 每次测试持续 30 秒

def run_stress():
    conns = []
    for _ in range(CONNS_PER_PROC):
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.connect((TARGET_IP, TARGET_PORT))
            s.setblocking(False)
            conns.append(s)
        except: pass

    count = 0
    end_time = time.time() + DURATION
    while time.time() < end_time:
        for s in conns:
            try:
                s.send(MSG)
                data = s.recv(2048)
                if data: count += 1
            except: continue
    return count

if __name__ == '__main__':
    # 模拟总计 400 个并发长连接
    cpu_count = multiprocessing.cpu_count()
    pool = multiprocessing.Pool(processes=cpu_count)
    print(f"开始压测，持续 {DURATION} 秒...")
    
    results = [pool.apply_async(run_stress) for _ in range(cpu_count)]
    total_requests = sum([r.get() for r in results])
    
    print(f"总吞吐量: {total_requests / DURATION:.2f} QPS")