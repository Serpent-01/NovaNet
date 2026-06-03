#include "novanet/rpc/core/AiExecutor.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace novanet::rpc {

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

}  // namespace novanet::rpc