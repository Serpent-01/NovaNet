#include <cassert>
#include <cstdint>
#include <string>

#include "novanet/base/Timestamp.h"
#include "novanet/rpc/stream/StreamSession.h"

using novanet::base::addTime;
using novanet::base::Timestamp;
using novanet::rpc::StreamSession;

namespace {

void testInitialState() {
    StreamSession session(1, 1001, "ChatService", "Generate");

    assert(session.streamId() == 1);
    assert(session.requestId() == 1001);
    assert(session.serviceName() == "ChatService");
    assert(session.methodName() == "Generate");

    assert(session.state() == StreamSession::State::kOpen);
    assert(session.open());
    assert(!session.closed());
    assert(!session.cancelled());
    assert(!session.terminal());

    assert(!session.localEndSent());
    assert(!session.remoteEndReceived());

    assert(session.canSendData());
    assert(session.canReceiveData());

    assert(session.lastActiveTime().valid());
}

void testLocalEndThenRemoteEnd() {
    StreamSession session(2, 1002, "ChatService", "Generate");

    const bool localEndOk = session.markLocalEnd();

    assert(localEndOk);
    assert(session.localEndSent());
    assert(!session.remoteEndReceived());
    assert(session.state() == StreamSession::State::kHalfClosedLocal);

    /*
     * 本端已经发送 END，所以不能再发送 DATA。
     * 但对端还没 END，所以仍然可以接收 DATA。
     */
    assert(!session.canSendData());
    assert(session.canReceiveData());

    const bool remoteEndOk = session.notifyEnd();

    assert(remoteEndOk);
    assert(session.remoteEndReceived());
    assert(session.state() == StreamSession::State::kClosed);
    assert(session.closed());
    assert(session.terminal());

    /*
     * closed 后不能重复 end/cancel/data。
     */
    assert(!session.markLocalEnd());
    assert(!session.notifyEnd());
    assert(!session.markCancelled("late cancel"));
    assert(!session.notifyData("late data"));
}

void testRemoteEndThenLocalEnd() {
    StreamSession session(3, 1003, "ChatService", "Generate");

    const bool remoteEndOk = session.notifyEnd();

    assert(remoteEndOk);
    assert(!session.localEndSent());
    assert(session.remoteEndReceived());
    assert(session.state() == StreamSession::State::kHalfClosedRemote);

    /*
     * 对端已经 END，所以不能再接收 DATA。
     * 但本端还没 END，full-duplex 语义下仍然可以发送 DATA。
     */
    assert(session.canSendData());
    assert(!session.canReceiveData());

    /*
     * END 后不能继续接收 DATA。
     */
    assert(!session.notifyData("late data"));

    const bool localEndOk = session.markLocalEnd();

    assert(localEndOk);
    assert(session.localEndSent());
    assert(session.state() == StreamSession::State::kClosed);
    assert(session.closed());
    assert(session.terminal());
}

void testMarkRemoteEndDoesNotTriggerCallback() {
    StreamSession session(4, 1004, "ChatService", "Generate");

    bool endCalled = false;

    session.setEndCallback([&](std::uint32_t streamId) {
        assert(streamId == 4);
        endCalled = true;
    });

    /*
     * markRemoteEnd 只做状态转换，不触发 callback。
     */
    const bool ok = session.markRemoteEnd();

    assert(ok);
    assert(session.state() == StreamSession::State::kHalfClosedRemote);
    assert(!endCalled);
}

void testNotifyEndTriggersCallback() {
    StreamSession session(5, 1005, "ChatService", "Generate");

    bool endCalled = false;

    session.setEndCallback([&](std::uint32_t streamId) {
        assert(streamId == 5);
        endCalled = true;

        /*
         * 这个调用用来验证 notifyEnd 没有持锁执行 callback。
         * 如果实现里持锁调用回调，这里可能死锁。
         */
        assert(session.remoteEndReceived());
    });

    const bool ok = session.notifyEnd();

    assert(ok);
    assert(endCalled);
    assert(session.state() == StreamSession::State::kHalfClosedRemote);
}

void testNotifyDataCallback() {
    StreamSession session(6, 1006, "ChatService", "Generate");

    int dataCount = 0;
    std::string lastPayload;

    session.setDataCallback(
        [&](std::uint32_t streamId, const std::string& payload) {
            assert(streamId == 6);
            ++dataCount;
            lastPayload = payload;

            /*
             * 验证 notifyData 没有持锁执行 callback。
             * 如果持锁调用，这里调用 state() 可能死锁。
             */
            assert(session.state() == StreamSession::State::kOpen);
        });

    const bool ok1 = session.notifyData("chunk-1");
    const bool ok2 = session.notifyData("chunk-2");

    assert(ok1);
    assert(ok2);
    assert(dataCount == 2);
    assert(lastPayload == "chunk-2");
}

void testEndThenDataRejected() {
    StreamSession session(7, 1007, "ChatService", "Generate");

    int dataCount = 0;

    session.setDataCallback(
        [&](std::uint32_t streamId, const std::string& payload) {
            (void)streamId;
            (void)payload;
            ++dataCount;
        });

    assert(session.notifyData("chunk-before-end"));
    assert(dataCount == 1);

    assert(session.notifyEnd());
    assert(session.remoteEndReceived());

    /*
     * 收到对端 END 后，不能继续接收 DATA。
     */
    assert(!session.notifyData("chunk-after-end"));
    assert(dataCount == 1);
}

void testCancelThenDataRejected() {
    StreamSession session(8, 1008, "ChatService", "Generate");

    bool errorCalled = false;
    std::string errorText;

    session.setErrorCallback(
        [&](std::uint32_t streamId, const std::string& error) {
            assert(streamId == 8);
            errorCalled = true;
            errorText = error;

            /*
             * 验证 markCancelled 没有持锁执行 callback。
             */
            assert(session.cancelled());
        });

    const bool cancelOk = session.markCancelled("client cancelled");

    assert(cancelOk);
    assert(errorCalled);
    assert(errorText == "client cancelled");

    assert(session.cancelled());
    assert(session.terminal());
    assert(session.cancelReason() == "client cancelled");

    assert(!session.canSendData());
    assert(!session.canReceiveData());

    /*
     * CANCEL 后不能继续 DATA / END。
     */
    assert(!session.notifyData("late data"));
    assert(!session.notifyEnd());
    assert(!session.markLocalEnd());
    assert(!session.markCancelled("duplicate cancel"));
}

void testTimeout() {
    StreamSession session(9, 1009, "ChatService", "Generate");

    bool errorCalled = false;
    std::string errorText;

    session.setErrorCallback(
        [&](std::uint32_t streamId, const std::string& error) {
            assert(streamId == 9);
            errorCalled = true;
            errorText = error;
        });

    const bool timeoutOk = session.markTimeout("stream idle timeout");

    assert(timeoutOk);
    assert(errorCalled);
    assert(errorText == "stream idle timeout");

    assert(session.cancelled());
    assert(session.terminal());
    assert(session.cancelReason() == "stream idle timeout");

    assert(!session.notifyData("late data"));
}

void testTouchAndExpired() {
    StreamSession session(10, 1010, "ChatService", "Generate");

    const Timestamp now = Timestamp::now();
    const Timestamp past = addTime(now, -10.0);

    session.touch(past);

    assert(session.lastActiveTime() == past);
    assert(session.idleSeconds(now) >= 10.0);
    assert(session.expired(now, 5.0));
    assert(!session.expired(now, 20.0));

    session.touch(now);

    assert(!session.expired(now, 5.0));
}

void testExpiredIgnoresTerminalSession() {
    StreamSession session(11, 1011, "ChatService", "Generate");

    const Timestamp now = Timestamp::now();
    const Timestamp past = addTime(now, -100.0);

    session.touch(past);
    assert(session.expired(now, 10.0));

    assert(session.markCancelled("cancel before timeout check"));

    /*
     * 终态 session 不应该再被 expired 判定为需要 timeout。
     * 后续 StreamManager 可以直接 remove terminal session。
     */
    assert(!session.expired(now, 10.0));
}

void testStateToString() {
    assert(StreamSession::stateToString(StreamSession::State::kOpen) ==
           "kOpen");

    assert(StreamSession::stateToString(
               StreamSession::State::kHalfClosedLocal) == "kHalfClosedLocal");

    assert(StreamSession::stateToString(
               StreamSession::State::kHalfClosedRemote) == "kHalfClosedRemote");

    assert(StreamSession::stateToString(StreamSession::State::kClosed) ==
           "kClosed");

    assert(StreamSession::stateToString(StreamSession::State::kCancelled) ==
           "kCancelled");
}

}  // namespace

int main() {
    testInitialState();
    testLocalEndThenRemoteEnd();
    testRemoteEndThenLocalEnd();
    testMarkRemoteEndDoesNotTriggerCallback();
    testNotifyEndTriggersCallback();
    testNotifyDataCallback();
    testEndThenDataRejected();
    testCancelThenDataRejected();
    testTimeout();
    testTouchAndExpired();
    testExpiredIgnoresTerminalSession();
    testStateToString();

    return 0;
}