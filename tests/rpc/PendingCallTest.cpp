#include <cassert>
#include <chrono>
#include <string>
#include <thread>

#include "novanet/rpc/core/PendingCall.h"

using novanet::rpc::PendingCall;

int main() {
    {
        PendingCall call(1001);

        assert(call.requestId() == 1001);
        assert(call.pending());
        assert(!call.done());
        assert(!call.timeout());
        assert(!call.failed());
        assert(!call.completed());

        const bool ok = call.markDone("response-bytes");

        assert(ok);
        assert(call.done());
        assert(call.completed());
        assert(call.responseBytes() == "response-bytes");
        assert(call.errorText().empty());

        const bool timeoutOk = call.markTimeout("late timeout");
        assert(!timeoutOk);

        const bool failedOk = call.markFailed("late failed");
        assert(!failedOk);

        assert(call.done());
        assert(call.responseBytes() == "response-bytes");
    }

    {
        PendingCall call(1002);

        const auto state = call.waitFor(std::chrono::milliseconds{10});

        assert(state == PendingCall::State::kTimeout);
        assert(call.timeout());
        assert(call.completed());
        assert(call.errorText() == "rpc call timeout");

        const bool doneOk = call.markDone("late response");
        assert(!doneOk);
        assert(call.timeout());
        assert(call.responseBytes().empty());
    }

    {
        PendingCall call(1003);

        std::thread worker([&call] {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
            const bool ok = call.markDone("async response");
            assert(ok);
        });

        const auto state = call.waitFor(std::chrono::milliseconds{1000});

        assert(state == PendingCall::State::kDone);
        assert(call.done());
        assert(call.responseBytes() == "async response");

        worker.join();
    }

    {
        PendingCall call(1004);

        std::thread worker([&call] {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
            const bool ok = call.markFailed("connection closed");
            assert(ok);
        });

        const auto state = call.wait();

        assert(state == PendingCall::State::kFailed);
        assert(call.failed());
        assert(call.errorText() == "connection closed");

        worker.join();
    }

    return 0;
}