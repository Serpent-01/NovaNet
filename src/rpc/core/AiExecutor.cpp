#include "novanet/rpc/core/AiExecutor.h"

#include <algorithm>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace novanet::rpc {

AiExecutor::AiExecutor() : AiExecutor(Options{}) {
}

AiExecutor::AiExecutor(Options options) : options_(options) {
    if (options_.workerCount == 0) {
        options_.workerCount = 1;
    }
}
AiExecutor::~AiExecutor() {
    stop(StopMode::kDrain);
}

bool AiExecutor::start() {
    std::vector<std::thread> createdWorkers;
    std::string startError;

    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (state_ == State::kRunning) {
            return true;
        }
        if (state_ != State::kNotStarted) {
            return false;
        }
        state_ = State::kRunning;
        stopMode_ = StopMode::kDrain;
        try {
            createdWorkers.reserve(options_.workerCount);
            for (std::size_t i = 0; i < options_.workerCount; ++i) {
                createdWorkers.emplace_back([this]() { workerLoop(); });
            }
        } catch (const std::exception& ex) {
            state_ = State::kStopping;
            stopMode_ = StopMode::kDiscardPending;
            tasks_.clear();
            startError =
                std::string("failed to start AiExecutor: ") + ex.what();

            lock.unlock();
            cv_.notify_all();
            for (auto& worker : createdWorkers) {
                if (worker.joinable()) {
                    worker.join();
                }
            }
            lock.lock();
            state_ = State::kStopped;
            workers_.clear();

            lock.unlock();
            reportError(std::move(startError));
            return false;
        } catch (...) {
            state_ = State::kStopping;
            stopMode_ = StopMode::kDiscardPending;
            tasks_.clear();
            startError = "failed to start AiExecutor: unknown exception";

            lock.unlock();
            cv_.notify_all();

            for (auto& worker : createdWorkers) {
                if (worker.joinable()) {
                    worker.join();
                }
            }

            lock.lock();
            state_ = State::kStopped;
            workers_.clear();

            lock.unlock();
            reportError(std::move(startError));
            return false;
        }
        workers_ = std::move(createdWorkers);
    }
    return true;
}

void AiExecutor::stop(StopMode mode) {
    std::vector<std::thread> workerToJoin;
    bool calledFromWorker = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::thread::id currentThreadId = std::this_thread::get_id();
        calledFromWorker = isWorkerThreadLocked(currentThreadId);
        if (calledFromWorker) {
            /*
             * 生产级约束：
             * stop() 不允许从 AiExecutor worker 线程中调用。
             *
             * 原因：
             * - 不能 join 自己；
             * - detach 自己会导致生命周期失控；
             * - 可能引入 use-after-free。
             */

        } else if (state_ == State::kNotStarted) {
            state_ = State::kStopped;
            return;
        } else if (state_ == State::kStopped) {
            return;
        } else if (state_ == State::kStopping) {
            return;
        } else {
            state_ = State::kStopping;
            stopMode_ = mode;
            if (stopMode_ == StopMode::kDiscardPending) {
                tasks_.clear();
            }
            workerToJoin.swap(workers_);
        }
    }
    if (calledFromWorker) {
        reportError("AiExecutor::stop() must not be called from worker thread");
        return;
    }

    cv_.notify_all();
    for (auto& worker : workerToJoin) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.clear();
        state_ = State::kStopped;
    }
}

AiExecutor::SubmitResult AiExecutor::submit(Task task) {
    if (!task) {
        return SubmitResult::kInvalidTask;
    }

    {
        //检查 AiExecutor 是否正在运行；检查任务队列是否已满；
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::kRunning) {
            return SubmitResult::kNotRunning;
        }
        if (options_.maxQueueSize != 0 &&
            tasks_.size() >= options_.maxQueueSize) {
            return SubmitResult::kQueueFull;
        }
        //把 task 放入任务队列；
        tasks_.push_back(std::move(task));
    }
    // 唤醒一个 worker 线程；
    cv_.notify_one();
    return SubmitResult::kOk;
}

void AiExecutor::setErrorHandler(ErrorHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    errorHandler_ = std::move(handler);
}

bool AiExecutor::running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::kRunning;
}

std::size_t AiExecutor::pendingTaskCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
}

std::size_t AiExecutor::workerCount() const noexcept {
    return options_.workerCount;
}
std::size_t AiExecutor::maxQueueSize() const noexcept {
    return options_.maxQueueSize;
}

std::string_view AiExecutor::toString(SubmitResult result) noexcept {
    switch (result) {
        case SubmitResult::kOk:
            return "kOk";

        case SubmitResult::kInvalidTask:
            return "kInvalidTask";

        case SubmitResult::kNotRunning:
            return "kNotRunning";

        case SubmitResult::kQueueFull:
            return "kQueueFull";

        default:
            return "UnknownSubmitResult";
    }
}

void AiExecutor::workerLoop() {
    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            //当前 worker
            //线程睡眠等待，直到“线程池正在停止”或者“任务队列不为空”。
            cv_.wait(lock, [this]() {
                return state_ == State::kStopping || !tasks_.empty();
            });

            if (state_ == State::kStopping) {
                //不再执行等待队列中的任务，worker 尽快退出。
                if (stopMode_ == StopMode::kDiscardPending) {
                    break;
                }
                //如果任务队列空了就退出
                if (tasks_.empty()) {
                    break;
                }
            }
            if (tasks_.empty()) {
                continue;
            }
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        try {
            task();
        } catch (const std::exception& ex) {
            reportError(std::string("AiExecutor task exception: ") + ex.what());
        } catch (...) {
            reportError("AiExecutor task exception: unknown exception");
        }
    }
}

void AiExecutor::reportError(std::string error) {
    ErrorHandler handler;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        handler = errorHandler_;
    }

    if (handler) {
        handler(std::move(error));
    }
}

bool AiExecutor::isWorkerThreadLocked(std::thread::id id) const noexcept {
    for (const auto& worker : workers_) {
        if (worker.joinable() && worker.get_id() == id) {
            return true;
        }
    }
    return false;
}

}  // namespace novanet::rpc
