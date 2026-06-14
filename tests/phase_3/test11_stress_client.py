import asyncio
import time
import numpy as np

LATENCIES = []
TOTAL_QUERIES = 0

async def stress_client(client_id):
    global TOTAL_QUERIES
    reader, writer = await asyncio.open_connection('127.0.0.1', 8080)
    msg = b"ping"
    
    end_time = time.time() + 600
    while time.time() < end_time:
        start = time.time()
        writer.write(msg)
        await writer.drain()
        data = await reader.read(1024)
        
        LATENCIES.append((time.time() - start) * 1000) # ms
        TOTAL_QUERIES += 1
        # 适当控制频率，避免把本地回环网卡跑死
        await asyncio.sleep(0.001) 

    writer.close()
    await writer.wait_closed()

async def main():
    # 模拟 100 个并发连接
    tasks = [stress_client(i) for i in range(100)]
    await asyncio.gather(*tasks)

if __name__ == '__main__':
    start_bench = time.time()
    asyncio.run(main())
    duration = time.time() - start_bench
    
    qps = TOTAL_QUERIES / duration
    p99 = np.percentile(LATENCIES, 99)
    print(f"Test Finished!")
    print(f"Total QPS: {qps:.2f}")
    print(f"p99 Latency: {p99:.2f} ms")