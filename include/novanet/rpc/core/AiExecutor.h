#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace novanet::rpc {
/*
 *RPC 的 AI 任务执行器。
 *
 * 设计目标：
 * - 不在 EventLoop 线程中执行慢速 AI 生成任务；
 * - 使用 worker 线程执行 AiProvider::generateStreaming；
 * - submit 非阻塞；
 * - 支持有界任务队列，避免任务无限堆积；
 * - stop 时安全退出；
 * - 单个任务抛异常不会杀死 worker 线程。
 */
class AiExecutor final {
public:
    using Task = std::function<void()>;
    using ErrorHandler = std::function<void(std::string)>;

    struct Options {
        std::size_t workerCount{1};
        std::size_t maxQueueSize{1024};
    };

    enum class SubmitResult : std::uint8_t {
        kOk = 0,
        kInvalidTask,
        kNotRunning,
        kQueueFull,
    };

    enum class StopMode : std::uint8_t {
        kDrain = 0,
        kDiscardPending,
    };

    AiExecutor();
    explicit AiExecutor(Options options);
    ~AiExecutor();

    AiExecutor(const AiExecutor&) = delete;
    AiExecutor& operator=(const AiExecutor&) = delete;

    AiExecutor(AiExecutor&&) = delete;
    AiExecutor& operator=(AiExecutor&&) = delete;

    /*
     * 启动 worker 线程。
     *
     * 返回：
     * - true：启动成功，或者已经处于 running 状态；
     * - false：已经 stop 后再次 start，或者线程创建失败。
     *
     * 生命周期建议：
     *   RpcServer::start() 中调用 aiExecutor_.start()
     *   RpcServer 析构或 stop 中调用 aiExecutor_.stop()
     */
    [[nodiscard]] bool start();
    void stop(StopMode mode = StopMode::kDrain);

    /*
     * 非阻塞提交任务。
     *
     * 设计要求：
     * - EventLoop 线程可以安全调用；
     * - 不能在队列满时阻塞 EventLoop；
     * - 队列满时返回 kQueueFull，由上层转成 backpressure / resource exhausted。
     */
    [[nodiscard]] SubmitResult submit(Task task);

    /*
     * 设置任务异常处理函数。
     *
     * worker 会捕获 task 抛出的异常，调用 ErrorHandler，
     * 然后继续执行后续任务。
     */
    void setErrorHandler(ErrorHandler handler);

    [[nodiscard]] bool running() const;
    [[nodiscard]] std::size_t pendingTaskCount() const;
    [[nodiscard]] std::size_t workerCount() const noexcept;
    [[nodiscard]] std::size_t maxQueueSize() const noexcept;

    [[nodiscard]] static std::string_view toString(
        SubmitResult result) noexcept;

private:
    enum class State : std::uint8_t {
        kNotStarted = 0,
        kRunning,
        kStopping,
        kStopped,
    };

    void workerLoop();
    void reportError(std::string error);

    [[nodiscard]] bool isWorkerThreadLocked(std::thread::id id) const noexcept;

private:
    Options options_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;

    std::deque<Task> tasks_;
    std::vector<std::thread> workers_;

    State state_{State::kNotStarted};
    StopMode stopMode_{StopMode::kDrain};

    ErrorHandler errorHandler_{};
};

}  // namespace novanet::rpc