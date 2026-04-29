import asyncio
import time

TARGET_IP = "127.0.0.1"
TARGET_PORT = 8080
CONNECTIONS = 10000

async def connect_and_send(client_id):
    try:
        # 建立连接
        reader, writer = await asyncio.open_connection(TARGET_IP, TARGET_PORT)
        
        # 发送数据
        writer.write(f"Ping {client_id}\n".encode())
        await writer.drain()
        
        # 接收回显数据 (验证 Echo Server 是否正常工作)
        data = await reader.read(100)
        
        # 保持连接 2 秒，给服务器施加并发压力
        await asyncio.sleep(2)
        
        writer.close()
        await writer.wait_closed()
    except Exception as e:
        print(f"Client {client_id} failed: {e}")

async def main():
    print(f"Initiating {CONNECTIONS} concurrent connections using asyncio...")
    start_time = time.time()
    
    # 瞬间创建一万个协程任务
    tasks = [connect_and_send(i) for i in range(CONNECTIONS)]
    await asyncio.gather(*tasks)
    
    print(f"Stress test completed in {time.time() - start_time:.2f} seconds.")

if __name__ == "__main__":
    # 在有些系统上限制较多，这一步是为了突破本机的最大打开文件数限制
    import resource
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    try:
        resource.setrlimit(resource.RLIMIT_NOFILE, (100000, hard))
    except Exception as e:
        print(f"Warning: could not increase file limit: {e}")
        
    asyncio.run(main())