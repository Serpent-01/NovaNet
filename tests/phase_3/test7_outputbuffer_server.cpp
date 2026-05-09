
/*
    编写 C++ 极限积压服务端
*/

#include "novanet/net/TcpConnection.h"
#include "novanet/net/TcpServer.h"
#include "novanet/net/EventLoop.h"
#include "novanet/base/Logger.h"

#include <string>

using namespace novanet::net;

// 构造一个 50MB 的超级大包
const std::string kHugePayload(50 * 1024 * 1024, 'A'); 
using TcpConnectionPtr = TcpConnection::TcpConnectionPtr;
void onConnection(const TcpConnectionPtr& conn) {
    if (conn->connected()) {
        LOG_INFO << "客户端连接成功！准备发送 50MB 巨型数据炸弹...";
        // 直接在主线程发，测试 sendInLoop 的积压逻辑
        conn->send(kHugePayload);
    }
}

int main() {
    novanet::base::Logger::setLogLevel(novanet::base::LogLevel::Info);

    LOG_INFO << "========== Output Buffer 极限背压测试服务端 ==========";
    EventLoop loop;
    InetAddress listenAddr(8080);
    TcpServer server(&loop, listenAddr, "OutputBufferTestServer");

    // 单线程跑就够了，更容易看清日志顺序
    server.setConnectionCallback(onConnection);
    server.start();

    loop.loop();
    return 0;
}