#include "novanet/net/Buffer.h"
#include <iostream>
#include <string>
#include <cassert>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>

using namespace std;
using namespace novanet::net; // 确认你的命名空间

// --- 2.1 基础的 append 与 retrieve 测试 ---
void testBufferAppendRetrieve() {
    Buffer buf;
    assert(buf.readableBytes() == 0);
    assert(buf.writeableBytes() == Buffer::kInitialSize); // 默认初始大小，通常是 1024
    
    // Append 测试
    string str = "hello";
    buf.append(str.c_str(), str.size());
    assert(buf.readableBytes() == 5);
    assert(buf.writeableBytes() == Buffer::kInitialSize - 5);
    assert(string(buf.peek(), buf.readableBytes()) == "hello");
    
    // Retrieve 测试
    buf.retrieve(2);
    assert(buf.readableBytes() == 3);
    assert(string(buf.peek(), buf.readableBytes()) == "llo");
    
    // RetrieveAll 测试
    buf.retrieveAll();
    assert(buf.readableBytes() == 0);
    assert(buf.writeableBytes() == Buffer::kInitialSize);
    
    cout << "[PASS] 2.1 Append / Retrieve" << endl;
}

// --- 2.2 多次 Append 顺序测试 ---
void testBufferMultipleAppend() {
    Buffer buf;
    buf.append("Nova", 4);
    buf.append("Net", 3);
    buf.append(" is great!", 10);
    
    assert(buf.readableBytes() == 17);
    assert(string(buf.peek(), buf.readableBytes()) == "NovaNet is great!");
    
    cout << "[PASS] 2.2 Multiple Append" << endl;
}

// --- 2.3 极限扩容与数据挪动测试 ---
void testBufferMakeSpace() {
    Buffer buf;
    buf.append(string(500, 'a').c_str(), 500);
    buf.retrieve(400); // 读走 400 字节，前面空出了 400 字节的 prependable 空间
    assert(buf.readableBytes() == 100);
    
    // 此时往里塞 1000 个字节。由于 1000 > 剩余空间，但 1000 < 剩余空间 + 前面空出的 400
    // Buffer 内部应该发生数据挪动（shift），而不是重新分配内存！
    buf.append(string(1000, 'b').c_str(), 1000);
    assert(buf.readableBytes() == 1100);
    
    string result(buf.peek(), buf.readableBytes());
    assert(result.substr(0, 100) == string(100, 'a'));
    assert(result.substr(100, 1000) == string(1000, 'b'));
    
    // 真正的扩容测试：塞入远超初始容量的巨型数据
    buf.append(string(10000, 'c').c_str(), 10000);
    assert(buf.readableBytes() == 11100);
    
    cout << "[PASS] 2.3 Buffer Expansion & MakeSpace" << endl;
}

// --- 2.4 & 2.5 纯正的 I/O 测试 (使用 socketpair) ---
void testBufferReadWriteFd() {
    // 创建本地双向通信管道，完美模拟 TCP I/O
    int fds[2];
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    assert(ret == 0);
    
    Buffer buf;
    int savedErrno = 0;
    
    // 模拟客户端向 fds[1] 发送超大数据 70000 字节
    string hugeData(70000, 'X');
    write(fds[1], hugeData.c_str(), hugeData.size());
    
    // ==========================================
    // 2.4 readFd 测试：完美模拟 EventLoop 的两次触发
    // ==========================================
    
    // 【第一轮】：榨干一次系统调用的物理极限
    // 1024 (原生空间) + 65536 (栈上 extraBuf) = 66560
    ssize_t n1 = buf.readFd(fds[0], &savedErrno);
    assert(n1 == 66560); 
    assert(buf.readableBytes() == 66560); // 确认 Buffer 已经存下了这么多
    assert(savedErrno == 0);
    
    // 此时内核 Socket 接收缓冲区里，还安安静静地躺着 3440 个字节。
    // 在真实场景中，epoll 会在这里再次触发，通知你继续读！
    
    // 【第二轮】：把剩下的数据打扫干净
    // 此时 Buffer 在上一步已经偷偷扩过容了，空间管够！
    ssize_t n2 = buf.readFd(fds[0], &savedErrno);
    assert(n2 == 3440); // 70000 - 66560 = 3440
    assert(buf.readableBytes() == 70000); // 两次总计完美接收 70000
    assert(savedErrno == 0);
    
    cout << "[PASS] 2.4 readFd (Simulate EventLoop Two Reads)" << endl;
    
    // ==========================================
    // 2.5 write 模拟测试：把 buf 里的 70000 字节原路退回
    // ==========================================
    ssize_t nwrote = write(fds[0], buf.peek(), buf.readableBytes());
    assert(nwrote == 70000); // 一次性成功回写 70000
    
    buf.retrieve(nwrote); // 模拟写入成功后，游标推进，释放 Buffer 空间
    assert(buf.readableBytes() == 0);
    
    // 验证客户端是否完整收到退回的 70000 字节
    char recvBuf[70000];
    ssize_t nread = read(fds[1], recvBuf, sizeof(recvBuf));
    assert(nread == 70000);
    
    close(fds[0]);
    close(fds[1]);
    cout << "[PASS] 2.5 writeFd / Cursor Advancement" << endl;
}

int main() {
    cout << "=== NovaNet Buffer Unit Tests ===" << endl;
    testBufferAppendRetrieve();
    testBufferMultipleAppend();
    testBufferMakeSpace();
    testBufferReadWriteFd();
    cout << "=== ALL TESTS PASSED! ===" << endl;
    return 0;
}