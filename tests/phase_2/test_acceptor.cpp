#include "novanet/net/EventLoop.h"
#include "novanet/net/Acceptor.h"
#include "novanet/net/InetAddress.h"
#include <iostream>
#include <strings.h>
#include <thread>
#include <vector>
#include <atomic>
#include <cassert>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

using namespace std;
using namespace novanet::net;

atomic<int> acceptedCount{0};
/*  
    Acceptor 并发榨干测试   
    Acceptor 能够在一个 epoll 事件周期内，靠 while(accept) 循环将堆积的几十个并发连接一次性吃干抹净（直到 EAGAIN）。
*/
void clientBurst(int count) {
    for (int i = 0; i < count; ++i) {
        int sockfd = ::socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in serverAddr;
        bzero(&serverAddr, sizeof serverAddr);
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(8888);
        ::inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);
        
        // 瞬间发起连接
        ::connect(sockfd, (struct sockaddr*)&serverAddr, sizeof serverAddr);
        ::close(sockfd); // 连上就关
    }
}

int main() {
    cout << "========== 开启 Acceptor 压榨测试 ==========" << endl;

    EventLoop loop;
    InetAddress listenAddr(8888);
    Acceptor acceptor(&loop, listenAddr);

    acceptor.setNewConnectionCallback([](int sockfd, const InetAddress& peerAddr) {
        acceptedCount++;
        ::close(sockfd); // 测试只管连接，连上就关
    });

    acceptor.listen();

    // 关键设计：不开 EventLoop，先发 50 个客户端请求，让内核全连接队列（Backlog）塞满
    cout << "正在发射 50 个并发连接弹幕..." << endl;
    thread clientThread(clientBurst, 50);
    clientThread.join(); // 等待客户端全发完

    cout << "50 个连接全部积压在内核，现在放狗（启动 EventLoop）!" << endl;

    // 定时 100ms 后退出
    thread quitThread([&]() {
        usleep(100 * 1000); // 100ms
        loop.quit();
    });

    // 开始接客，它应该在第一个 EPOLLIN 触发时，在 handleRead 的 while 循环里把 50 个全捞出来
    loop.loop(); 
    quitThread.join();

    cout << "总共收到连接数: " << acceptedCount.load() << endl;
    
    // 必测 12：不能漏连接！
    assert(acceptedCount.load() == 50);

    cout << "[PASS] 必测 12: Acceptor 完美榨干 Backlog，无漏连接！" << endl;
    return 0;
}