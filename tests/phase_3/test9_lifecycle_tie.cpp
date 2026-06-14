#include "novanet/base/Logger.h"
#include "novanet/net/EventLoop.h"
#include "novanet/net/TcpServer.h"
#include <thread>
#include <unistd.h>

using namespace novanet::net;
using TcpConnectionPtr = TcpConnection::TcpConnectionPtr;
// 全局变量，用于在不同线程间传递连接指针进行“刺杀”
TcpConnectionPtr g_conn_to_kill;

void onMessage(const TcpConnectionPtr &conn, Buffer *buf) {
  LOG_INFO << "==== [SubLoop] 回调开始 ====";
  LOG_INFO << "1. 接收到消息，当前连接: " << conn->name()
           << " 引用计数: " << conn.use_count();
  buf->retrieveAll();

  // 重点：在这里记录下连接，交给主线程去杀
  g_conn_to_kill = conn;

  // 模拟耗时操作，此时 Channel::handleEvent 正在运行，guard 局部变量正在栈上
  LOG_INFO << "2. [SubLoop] 进入模拟耗时业务（睡眠 3 秒）...";
  ::sleep(3);

  LOG_INFO << "4. [SubLoop] 耗时业务结束。引用计数: " << conn.use_count();
  LOG_INFO << "==== [SubLoop] 回调结束 ====";
}

int main() {
  novanet::base::Logger::setLogLevel(novanet::base::LogLevel::Info);
  EventLoop loop;
  TcpServer server(&loop, InetAddress(8080), "LifecycleTieServer");

  server.setMessageCallback(onMessage);

  // 【关键修改 1】：开启多线程模式
  // 只有 SubLoop 和 MainLoop 不在同一个线程，才能产生真正的“竞态”
  server.setThreadNum(1);

  server.start();

  // 【关键修改 2】：刺杀线程逻辑
  std::thread killer([&server]() {
    LOG_INFO << "[Assassin] 杀手已就位，正在潜伏等待目标...";

    // 持续轮询，直到看到目标出现
    while (!g_conn_to_kill) {
      std::this_thread::yield(); // 别把 CPU 跑满了
    }

    // 目标出现了！先让它跑一会儿回调（进入 sleep）
    LOG_INFO << "[Assassin] 目标出现！等待 1 秒后发起致命一击...";
    ::sleep(1);

    LOG_INFO << "3. [Assassin] 核心动作：发起 forceClose！";
    g_conn_to_kill->forceClose();
    g_conn_to_kill.reset();
    LOG_INFO << "3.1 [Assassin] 刺杀完成，撤离现场。";
  });
  killer.detach();

  loop.loop(); // MainLoop 进入循环
  return 0;
}