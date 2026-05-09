

/*  
    必测 6：跨线程 `send()` 正确
    编写 C++ 异线程轰炸服务端
*/

#include "novanet/net/TcpConnection.h"
#include "novanet/net/TcpServer.h"
#include "novanet/net/EventLoop.h"
#include "novanet/base/Logger.h"

#include <string>
#include <thread>
#include <iostream>

using namespace novanet::net;
using namespace novanet::base;
using TcpConnectionPtr =  TcpConnection::TcpConnectionPtr;
// 当新连接建立时，我们直接在后台开一个独立线程，向这个连接疯狂灌入 20000 个带编号的包
void onConnection(const TcpConnectionPtr& conn) {
    if (conn->connected()) {
        LOG_INFO << "新连接建立: " << conn->name() << "。准备在后台线程启动狂暴发送！";

        // 【核心精髓】：这里按值捕获 conn (TcpConnectionPtr)，
        // 确保后台线程发数据时，这个连接的 shared_ptr 引用计数大于 0，绝对不会被提前析构！
        std::thread([conn]() {
            LOG_INFO << "[Worker] 异线程 (ID: " << std::this_thread::get_id() 
                     << ") 开始向连接 " << conn->name() << " 灌入 20000 个包...";
            
            for (int i = 1; i <= 20000; ++i) {
                // 构造大小混合的 payload
                // 格式: "SEQ:00001|PAYLOAD:XXXX...\n"
                std::string payload(i % 100 + 10, 'A' + (i % 26)); // 长度 10~109 不等的字符
                std::string msg = "SEQ:" + std::to_string(i) + "|PAYLOAD:" + payload + "\n";
                
                // 【测试点】：这是在异线程调用的 send！
                // 底层必须自动切入 runInLoop，并将数据安全追加到该连接专属的 outputBuffer_ 中
                conn->send(msg);
            }
            
            LOG_INFO << "[Worker] 异线程发送完毕，请求安全关闭连接 (shutdown)。";
            // 同样，shutdown 也必须是跨线程安全的（内部也会走 runInLoop）
            conn->shutdown(); 

        }).detach(); // 分离线程，让它自己在后台跑
    } else {
        LOG_INFO << "连接断开: " << conn->name();
    }
}

int main() {
    // 开启 INFO 日志
    novanet::base::Logger::setLogLevel(novanet::base::LogLevel::Info);

    EventLoop loop;
    InetAddress listenAddr(8080);
    TcpServer server(&loop, listenAddr, "CrossThreadSendServer");

    // 开启 4 个 Sub Loop 用于处理底层 I/O
    server.setThreadNum(4);
    server.setConnectionCallback(onConnection);

    LOG_INFO << "========== CrossThread Send Test Server Started ==========";
    server.start();
    loop.loop();

    return 0;
}