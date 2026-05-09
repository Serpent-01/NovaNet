import socket
import time
import sys
import struct

def test_normal_close():
    print("\n--- 测试 1: 客户端正常关闭 (Graceful Close) ---")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect(('127.0.0.1', 8080))
    s.send(b"Hello")
    time.sleep(0.5)
    s.close() # 底层发送 FIN 包
    print("已正常调用 close()")
    time.sleep(1)

def test_abnormal_close():
    print("\n--- 测试 2: 客户端异常暴力断开 (RST) ---")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    # 设置 SO_LINGER，强制 close 时直接丢弃缓冲区并发送 RST 包，模拟拔网线或进程崩溃
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
    s.connect(('127.0.0.1', 8080))
    s.send(b"Boom")
    time.sleep(0.5)
    s.close() # 触发 RST
    print("已暴力销毁 socket，发送了 RST")
    time.sleep(1)

if __name__ == '__main__':
    test_normal_close()
    test_abnormal_close()