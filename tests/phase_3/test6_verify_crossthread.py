# 必测 6：跨线程 `send()` 正确
# 编写 Python 强迫症校验客户端

import socket
import sys

def verify():
    print("正在连接到 127.0.0.1:8080 ...")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.connect(('127.0.0.1', 8080))
    except Exception as e:
        print(f"连接失败: {e}")
        return

    print("连接成功！开始接收并校验数据...")
    
    expected_seq = 1
    buffer = b""
    
    while True:
        data = s.recv(8192)
        if not data:
            break # 服务器主动关闭了连接 (shutdown 生效)
        
        buffer += data
        
        # 按照换行符切分我们定义的数据包
        while b'\n' in buffer:
            line, buffer = buffer.split(b'\n', 1)
            msg = line.decode('utf-8')
            
            if not msg.startswith("SEQ:"):
                print(f"\n❌ [致命错误] 数据截断或乱码！收到: {msg[:50]}...")
                sys.exit(1)
                
            parts = msg.split("|")
            seq_str = parts[0].replace("SEQ:", "")
            
            try:
                seq = int(seq_str)
            except ValueError:
                print(f"\n❌ [致命错误] 序号解析失败，发生串包！收到: {msg[:50]}...")
                sys.exit(1)
            
            # 严格校验顺序
            if seq != expected_seq:
                print(f"\n❌ [致命错误] 乱序或丢包！期望 SEQ:{expected_seq}, 实际收到 SEQ:{seq}")
                sys.exit(1)
                
            expected_seq += 1
            
            if expected_seq % 5000 == 0:
                print(f"✅ 完美通过到 SEQ: {expected_seq - 1}")

    final_expected = 20001
    if expected_seq == final_expected:
        print(f"\n🎉 完美通关！共接收并校验 {expected_seq - 1} 个数据包！")
        print("跨线程 send() 测试满分：不崩溃、不乱序、不截断、不串包！")
    else:
        print(f"\n❌ 测试失败！提前结束，期望收到 20000 包，实际只验证到 {expected_seq - 1} 包。")

if __name__ == '__main__':
    verify()