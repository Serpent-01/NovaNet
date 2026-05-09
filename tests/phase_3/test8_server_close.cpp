#include "novanet/net/TcpConnection.h"
#include "novanet/net/TcpServer.h"
#include "novanet/net/EventLoop.h"
#include "novanet/base/Logger.h"

using namespace novanet::net;
using TcpConnectionPtr = TcpConnection::TcpConnectionPtr;

void onMessage(const TcpConnectionPtr& conn, Buffer* buf) {
    std::string msg = buf->retrieveAllAsString();
    if (msg.find("DOWNLOAD") != std::string::npos) {
        LOG_INFO << "客户端请求下载大文件...";
        std::string hugeData(10 * 1024 * 1024, 'A'); // 10MB 数据
        
        conn->send(hugeData); // 数据进入 OutputBuffer，开始排队发送
        
        LOG_INFO << "立刻调用 shutdown()！测试是否会把数据发完再关。";
        conn->shutdown(); 
    }
}

int main() {
    novanet::base::Logger::setLogLevel(novanet::base::LogLevel::Info);
    EventLoop loop;
    TcpServer server(&loop, InetAddress(8080), "CloseTestServer");
    server.setMessageCallback(onMessage);
    server.start();
    loop.loop();
    return 0;
}