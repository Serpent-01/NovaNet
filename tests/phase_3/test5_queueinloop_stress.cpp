/*
    必测 5：高频跨线程投递不丢任务
*/


#include "novanet/net/EventLoop.h"
#include "novanet/net/EventLoopThread.h"
#include "novanet/base/Logger.h"
#include "novanet/base/Timestamp.h"

#include <thread>
#include <vector>
#include <future>
#include <iostream>

using namespace novanet::net;
// using namespace novanet::base;

const int kNumProducers = 4;        // 4 个并发生产者线程
const int kTasksPerProducer = 10000; // 每个生产者投递 1 万次
const int kTotalTasks = kNumProducers * kTasksPerProducer; // 共 4 万个任务

// 【核心架构亮点】：因为所有被 queueInLoop 的任务最终都只在唯一的一个 Sub Loop 线程中串行执行，
// 所以下面这两个状态变量，根本不需要 std::mutex 加锁，也不需要 std::atomic！
// 这完美印证了 One Loop Per Thread 极大地简化了状态管理的优势。
int g_tasksExecuted = 0;
std::vector<int> g_taskCounts(kTotalTasks, 0); 

// 生产者线程的执行体
void producerThreadFunc(EventLoop* targetLoop, int producerId) {
    for (int i = 0; i < kTasksPerProducer; ++i) {
        // 计算全局唯一的 taskId (例如 0~39999)
        int taskId = producerId * kTasksPerProducer + i;
        
        // 疯狂向 targetLoop 投递任务
        targetLoop->queueInLoop([taskId]() {
            // 这段 Lambda 会在 Sub Loop 线程中执行
            g_taskCounts[taskId]++;
            g_tasksExecuted++;
        });
    }
}

int main() {
    // 【关键】：必须把日志级别调高到 Warn，否则 4 万条连续的 Info 日志输出会让终端 I/O 成为瓶颈，严重干扰耗时测试
    novanet::base::Logger::setLogLevel(novanet::base::LogLevel::Warn);

    std::cout << "========== 开启 queueInLoop 高频并发压测 ==========\n";
    std::cout << "准备启动 " << kNumProducers << " 个生产者线程，共计投递 " << kTotalTasks << " 个任务。\n";

    // 1. 启动目标子线程 (充当消费者)
    EventLoopThread loopThread;
    EventLoop* subLoop = loopThread.startLoop();

    novanet::base::Timestamp startTime = novanet::base::Timestamp::now();

    // 2. 启动 4 个生产者线程开始狂暴轰炸
    std::vector<std::thread> producers;
    for (int i = 0; i < kNumProducers; ++i) {
        producers.emplace_back(producerThreadFunc, subLoop, i);
    }

    // 3. 等待所有生产者投递完毕
    for (auto& t : producers) {
        t.join();
    }
    
    std::cout << "[Main] 所有生产者已完成投递 (join 完毕)。\n";

    // 4. 巧妙的同步机制：我们再向 subLoop 投递最后一个任务。
    // 因为队列是 FIFO (先进先出) 的，当这个 promise 被 set_value 时，
    // 说明前面那 4 万个并发投递的任务已经全部被执行完了！
    std::promise<void> completionPromise;
    std::future<void> completionFuture = completionPromise.get_future();
    
    subLoop->queueInLoop([&completionPromise]() {
        completionPromise.set_value();
    });

    // 阻塞等待子线程消化完所有任务（设置个 5 秒超时防死锁）
    std::future_status status = completionFuture.wait_for(std::chrono::seconds(5));
    
    if (status == std::future_status::timeout) {
        std::cerr << "❌ 测试失败：死锁或处理严重超时！\n";
        subLoop->quit();
        return -1;
    }

    novanet::base::Timestamp endTime = novanet::base::Timestamp::now();
    double costSeconds = timeDifference(endTime, startTime);

    // 5. 终极数据核对
    std::cout << "========== 压测结果验证 ==========\n";
    std::cout << "总耗时: " << costSeconds * 1000 << " ms\n";
    std::cout << "实际执行任务总数: " << g_tasksExecuted << " / " << kTotalTasks << "\n";

    bool passed = true;
    if (g_tasksExecuted != kTotalTasks) {
        passed = false;
        std::cerr << "❌ 测试失败：发生丢任务现象！\n";
    }

    int missingCount = 0;
    int duplicateCount = 0;
    for (int i = 0; i < kTotalTasks; ++i) {
        if (g_taskCounts[i] == 0) {
            missingCount++;
            passed = false;
        } else if (g_taskCounts[i] > 1) {
            duplicateCount++;
            passed = false;
        }
    }

    if (missingCount > 0) std::cerr << "❌ 丢失了 " << missingCount << " 个具体编号的任务。\n";
    if (duplicateCount > 0) std::cerr << "❌ 重复执行了 " << duplicateCount << " 个具体编号的任务。\n";

    if (passed) {
        std::cout << "✅ 完美通关！不丢任务、不重执行、不死锁！引擎并发抗压满分！\n";
    } else {
        std::cout << "❌ 测试未通过！请检查 EventLoop 的 pendingFunctors_ 加锁逻辑。\n";
    }

    subLoop->quit();
    return 0;
}