#include <cassert>
#include <chrono>
#include <string>
#include <thread>

#include "novanet/rpc/core/PendingCallManager.h"

using novanet::rpc::PendingCall;
using novanet::rpc::PendingCallManager;

int main() {
    {
        PendingCallManager manager;

        auto call = manager.create(1001);
        assert(call);
        assert(call->requestId() == 1001);
        assert(manager.size() == 1);
        assert(!manager.empty());

        auto found = manager.find(1001);
        assert(found == call);

        auto duplicated = manager.create(1001);
        assert(!duplicated);
        assert(manager.size() == 1);
    }

    {
        PendingCallManager manager;

        auto call = manager.create(1002);
        assert(call);

        const auto result = manager.complete(1002, "response-bytes");

        assert(result == PendingCallManager::FinishResult::kCompleted);
        assert(call->done());
        assert(call->responseBytes() == "response-bytes");
        assert(manager.empty());

        const auto late = manager.complete(1002, "late-response");

        assert(late == PendingCallManager::FinishResult::kNotFound);
        assert(call->responseBytes() == "response-bytes");
    }

    {
        PendingCallManager manager;

        auto call = manager.create(1003);
        assert(call);

        const auto result = manager.fail(1003, "connection closed");

        assert(result == PendingCallManager::FinishResult::kCompleted);
        assert(call->failed());
        assert(call->errorText() == "connection closed");
        assert(manager.empty());

        const auto late = manager.complete(1003, "late-response");
        assert(late == PendingCallManager::FinishResult::kNotFound);
    }

    {
        PendingCallManager manager;

        auto call = manager.create(1004);
        assert(call);

        const auto result = manager.timeout(1004, "call timeout");

        assert(result == PendingCallManager::FinishResult::kCompleted);
        assert(call->timeout());
        assert(call->errorText() == "call timeout");
        assert(manager.empty());

        const auto late = manager.complete(1004, "late-response");
        assert(late == PendingCallManager::FinishResult::kNotFound);
    }

    {
        PendingCallManager manager;

        auto c1 = manager.create(1);
        auto c2 = manager.create(2);
        auto c3 = manager.create(3);

        assert(c1);
        assert(c2);
        assert(c3);
        assert(manager.size() == 3);

        const std::size_t n = manager.failAll("connection closed");

        assert(n == 3);
        assert(manager.empty());

        assert(c1->failed());
        assert(c2->failed());
        assert(c3->failed());

        assert(c1->errorText() == "connection closed");
        assert(c2->errorText() == "connection closed");
        assert(c3->errorText() == "connection closed");
    }

    {
        PendingCallManager manager;

        auto call = manager.create(2001);
        assert(call);

        std::thread worker([&manager] {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});

            const auto result = manager.complete(2001, "async response");

            assert(result == PendingCallManager::FinishResult::kCompleted);
        });

        const auto state = call->waitFor(std::chrono::milliseconds{1000});

        assert(state == PendingCall::State::kDone);
        assert(call->responseBytes() == "async response");

        worker.join();
        assert(manager.empty());
    }

    {
        PendingCallManager manager;

        auto call = manager.create(3001);
        assert(call);

        const auto state = call->waitFor(std::chrono::milliseconds{10});

        assert(state == PendingCall::State::kTimeout);
        assert(call->timeout());

        /*
         * callUnary 超时后会 remove。
         * 这里模拟 callUnary 的清理行为。
         */
        auto removed = manager.remove(3001);
        assert(removed == call);
        assert(manager.empty());

        /*
         * 迟到 response 不应该重新完成。
         */
        const auto late = manager.complete(3001, "late response");

        assert(late == PendingCallManager::FinishResult::kNotFound);
        assert(call->timeout());
        assert(call->responseBytes().empty());
    }

    return 0;
}