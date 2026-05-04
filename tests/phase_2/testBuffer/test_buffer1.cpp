#include "novanet/net/Buffer.h" 
#include <iostream>
#include <string>
#include <cassert>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <vector>

using namespace std;
using namespace novanet::net; // 确认你的命名空间

// --- 必测 4: 基础的 append 与 retrieve 测试 ---
void testBufferAppendRetrieve() {
    Buffer buf;
    assert(buf.readableBytes() == 0);
    assert(buf.writeableBytes() == Buffer::kInitialSize); // 通常初始大小是 1024
    
    // Append 验证
    string str = "hello";
    buf.append(str.c_str(), str.size());
    assert(buf.readableBytes() == 5);
    assert(buf.writeableBytes() == Buffer::kInitialSize - 5);
    assert(string(buf.peek(), buf.readableBytes()) == "hello");
    
    // Retrieve 验证
    buf.retrieve(2);
    assert(buf.readableBytes() == 3);
    assert(string(buf.peek(), buf.readableBytes()) == "llo");
    
    // RetrieveAll 验证
    buf.retrieveAll();
    assert(buf.readableBytes() == 0);
    assert(buf.writeableBytes() == Buffer::kInitialSize);
    
    cout << "[PASS] 必测 4: Append / Retrieve (状态恢复正确)" << endl;
}

// --- 必测 5: 多次 Append 顺序测试 ---
void testBufferMultipleAppend() {
    Buffer buf;
    buf.append("abc", 3);
    buf.append("def", 3);
    buf.append("ghi", 3);
    
    assert(buf.readableBytes() == 9);
    assert(string(buf.peek(), buf.readableBytes()) == "abcdefghi");
    
    buf.retrieve(4); // 读走 "abcd"
    assert(buf.readableBytes() == 5);
    assert(string(buf.peek(), buf.readableBytes()) == "efghi");
    
    cout << "[PASS] 必测 5: Multiple Append (连续追加顺序无误)" << endl;
}

// --- 必测 6: 极限扩容与数据挪动测试 ---
void testBufferMakeSpace() {
    Buffer buf;
    
    // 1. 先塞入一些数据
    buf.append(string(500, 'a').c_str(), 500);
    // 2. 读走前面大部分，腾出 prependable 空洞
    buf.retrieve(400); 
    assert(buf.readableBytes() == 100);
    
    // 3. 此时往里塞 1000 个字节。由于 1000 > 当前 writable，但 1000 < writable + prependable
    // Buffer 内部应该发生数据前移（shift），而不是重新 malloc 扩容！
    buf.append(string(1000, 'b').c_str(), 1000);
    assert(buf.readableBytes() == 1100);
    
    string result(buf.peek(), buf.readableBytes());
    assert(result.substr(0, 100) == string(100, 'a')); // 原数据不丢
    assert(result.substr(100, 1000) == string(1000, 'b')); // 新数据不乱
    
    // 4. 真正的暴力扩容测试：塞入远超初始容量的巨型数据
    buf.append(string(10000, 'c').c_str(), 10000);
    assert(buf.readableBytes() == 11100);
    
    cout << "[PASS] 必测 6: Buffer Expansion & MakeSpace (内部挪动与扩容均完美)" << endl;
}

// --- 必测 7 & 必测 8: 纯正的 I/O 测试 (使用 socketpair) ---
void testBufferReadWriteFd() {
    // 创建本地双向通信管道，纯内存态，完美模拟 TCP I/O，不涉及网卡
    int fds[2];
    int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    assert(ret == 0);
    
    Buffer buf;
    int savedErrno = 0;
    
    // --- 必测 7: readFd 测试 ---
    // 魔数 70000：这非常关键！Muduo 的 extrabuf 通常是 65536 字节。
    // 我们故意写入大于 64KB 的数据，强迫 readFd 内部触发 readv 溢出到 extrabuf，
    // 并自动调用 append 进行底层扩容。这是考验分散读的最严苛边界！
    string hugeData(70000, 'X');
    write(fds[1], hugeData.c_str(), hugeData.size());
    
    // 服务端从 fds[0] 读
    ssize_t n = buf.readFd(fds[0], &savedErrno);
    assert(n == 70000);
    assert(buf.readableBytes() == 70000);
    assert(savedErrno == 0);
    cout << "[PASS] 必测 7: readFd (Vector I/O 分散读与越界自动扩容完美)" << endl;
    
    // --- 必测 8: writeFd 模拟测试 ---
    // 我们手动模拟将 buf 里的数据通过系统调用写回 fds[0]
    ssize_t nwrote = write(fds[0], buf.peek(), buf.readableBytes());
    assert(nwrote == 70000);
    
    // 模拟写入成功后，应用程序代码负责推进读游标
    buf.retrieve(nwrote); 
    assert(buf.readableBytes() == 0); // 游标应该归零
    
    // 验证另一端 (fds[1]) 是否完整收到那 70000 个 'X'
    vector<char> recvBuf(70000); // 使用 vector 避免爆栈
    ssize_t nread = read(fds[1], recvBuf.data(), recvBuf.size());
    assert(nread == 70000);
    assert(recvBuf[0] == 'X' && recvBuf[69999] == 'X');
    
    close(fds[0]);
    close(fds[1]);
    cout << "[PASS] 必测 8: writeFd / Cursor Advancement (反向写入与游标推进正确)" << endl;
}

int main() {
    cout << "========== NovaNet Buffer 核心单元测试 ==========" << endl;
    testBufferAppendRetrieve();
    testBufferMultipleAppend();
    testBufferMakeSpace();
    testBufferReadWriteFd();
    cout << "================ ALL TESTS PASSED! ================" << endl;
    return 0;
}