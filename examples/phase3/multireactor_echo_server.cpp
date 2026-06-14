#include "novanet/net/TcpConnection.h"
#include "novanet/net/TcpServer.h"
#include "novanet/net/EventLoop.h"
#include "novanet/net/InetAddress.h"
#include "novanet/base/Logger.h"

#include <string>
#include <unistd.h> // for getpid()

using namespace novanet::net;
// 假设你的 Logger 在 novanet::base 命名空间，如果没有请忽略或调整
// using namespace novanet::base; 

/**
 * @brief 高性能多线程回显服务器
 * 
 * 验证 Phase 3 核心能力：
 * 1. 主线程 (Main Loop) 仅负责监听新连接。
 * 2. 多个子线程 (Sub Loops) 并发处理连接的读写。
 * 3. 跨线程安全：连接的建立、消息收发、断开绝不发生数据竞争。
 */
class EchoServer {
public:
    using TcpConnectionPtr =  TcpConnection::TcpConnectionPtr;
    EchoServer(EventLoop* loop, const InetAddress& listenAddr, int numThreads)
        : loop_(loop),
          server_(loop, listenAddr, "MultiReactorEchoServer") 
    {
        // 现代 C++ 规范：使用 Lambda 表达式绑定回调，取代古老的 std::bind
        server_.setConnectionCallback(
            [this](const TcpConnectionPtr& conn) { this->onConnection(conn); }
        );

        server_.setMessageCallback(
            [this](const TcpConnectionPtr& conn, Buffer* buf) { this->onMessage(conn, buf); }
        );

        // Phase 3 的灵魂：开启多线程 I/O 池！
        // 如果设置为 0，就是 Phase 2 的单线程模型；设置为 N，就是完整的 Multi-Reactor 模型
        server_.setThreadNum(numThreads);
    }

    void start() {
        LOG_INFO << "EchoServer starting... Main Loop thread ID will be printed dynamically.";
        server_.start();
    }

private:
    // 连接状态改变时的回调 (由连接所属的 Sub Loop 线程调用)
    void onConnection(const TcpConnectionPtr& conn) {
        if (conn->connected()) {
            LOG_INFO << "[EchoServer] New Connection: " 
                     << conn->peerAddress().toIpPort() << " -> "
                     << conn->localAddress().toIpPort() << " is UP";
        } else {
            LOG_INFO << "[EchoServer] Connection Closed: " 
                     << conn->peerAddress().toIpPort() << " -> "
                     << conn->localAddress().toIpPort() << " is DOWN";
        }
    }

    // 收到消息时的回调 (由连接所属的 Sub Loop 线程调用)
    void onMessage(const TcpConnectionPtr& conn, Buffer* buf) {
        // 1. 将 Buffer 中累积的数据全部提取为 std::string
        // 你的 Buffer 类需要有 retrieveAllAsString() 方法，如果没有，可以用以下替代：
        // std::string msg(buf->peek(), buf->readableBytes()); buf->retrieveAll();
        std::string msg = buf->retrieveAllAsString();

        LOG_INFO << "[EchoServer] Received " << msg.size() << " bytes from " 
                 << conn->name() << "\nContent: " << msg;

        // 2. 将数据原封不动地发送回去 (Echo)
        // 这里的 send 是跨线程安全的，但因为 onMessage 本身就在连接所属的 I/O 线程，
        // 所以它会走 zero-copy 的极速路径 (sendInLoop)！
        conn->send(msg);

        // 可选：如果你想做一个 "发完即关" 的短连接服务器，可以取消下面这行的注释
        // conn->shutdown(); 
    }

    EventLoop* loop_;
    TcpServer server_;
};

int main(int argc, char* argv[]) {
    // 设置全局日志级别（如果有这个接口的话）
    
    //novanet::base::Logger::setLogLevel(novanet::base::LogLevel::Info);
    // 压测期间只记录错误，不记录过程，保证 CPU 全力跑网络 I/O
    novanet::base::Logger::setLogLevel(novanet::base::LogLevel::Warn);
    LOG_INFO << "Multi-Reactor Echo Server started. PID = " << ::getpid();

    // 1. 创建主事件循环 (Main Reactor)
    EventLoop loop;

    // 2. 配置监听地址，比如绑定 8080 端口，监听所有网卡 (0.0.0.0)
    InetAddress listenAddr(8080, false, false);

    // 3. 实例化 EchoServer，设置启用 4 个 I/O 子线程
    int threadNum = 6;
    EchoServer server(&loop, listenAddr, threadNum);

    // 4. 启动服务器 (内部会 listen，并启动 4 个 EventLoopThread)
    server.start();

    // 5. 开启主线程的死循环，专门处理 accept 事件
    loop.loop();

    return 0;
}