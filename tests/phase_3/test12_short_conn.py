import asyncio
import time

TARGET_IP = '127.0.0.1'
TARGET_PORT = 8080
TOTAL_CONNECTIONS = 50000
CONCURRENCY_LIMIT = 500  # 控制并发，防止把本地端口打满导致 TIME_WAIT 耗尽

SUCCESS_COUNT = 0
FAIL_COUNT = 0

async def short_connection(sem):
    global SUCCESS_COUNT, FAIL_COUNT
    async with sem:
        try:
            # 1. 建连
            reader, writer = await asyncio.open_connection(TARGET_IP, TARGET_PORT)
            
            # 2. 发包
            writer.write(b"S")
            await writer.drain()
            
            # 3. 收包
            data = await reader.read(1)
            
            # 4. 立即主动断连
            writer.close()
            await writer.wait_closed()
            
            if data:
                SUCCESS_COUNT += 1
            else:
                FAIL_COUNT += 1
        except Exception as e:
            FAIL_COUNT += 1

async def main():
    print(f"🚀 开始短连接风暴压测...")
    print(f"目标总数: {TOTAL_CONNECTIONS}, 兵分 {CONCURRENCY_LIMIT} 路持续攻击")
    
    start_time = time.time()
    
    sem = asyncio.Semaphore(CONCURRENCY_LIMIT)
    tasks = [short_connection(sem) for _ in range(TOTAL_CONNECTIONS)]
    
    # 进度条打印任务
    async def progress_reporter():
        while SUCCESS_COUNT + FAIL_COUNT < TOTAL_CONNECTIONS:
            await asyncio.sleep(1)
            print(f"已完成: {SUCCESS_COUNT + FAIL_COUNT}/{TOTAL_CONNECTIONS} (成功:{SUCCESS_COUNT}, 失败:{FAIL_COUNT})")
            
    reporter = asyncio.create_task(progress_reporter())
    await asyncio.gather(*tasks)
    reporter.cancel()
    
    cost = time.time() - start_time
    print(f"\n✅ 测试结束！耗时: {cost:.2f} 秒")
    print(f"成功: {SUCCESS_COUNT}, 失败: {FAIL_COUNT}")
    print(f"建连/断连速率: {TOTAL_CONNECTIONS / cost:.2f} 次/秒")

if __name__ == '__main__':
    asyncio.run(main())