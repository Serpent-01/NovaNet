#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <string>
#include <thread>

#include "calculator.pb.h"
#include "novanet/base/Logger.h"
#include "novanet/net/EventLoop.h"
#include "novanet/net/InetAddress.h"
#include "novanet/rpc/core/FakeAiProvider.h"
#include "novanet/rpc/core/RpcServer.h"
#include "novanet/rpc/protocol/FrameType.h"
#include "novanet/rpc/protocol/RpcCodec.h"
#include "novanet/rpc/protocol/RpcHeader.h"
#include "novanet/rpc/protocol/RpcMessage.h"
#include "rpc_meta.pb.h"

namespace {

constexpr uint16_t kTestPort = 19091;

class CalculatorServiceImpl final
    : public novanet::example::calculator::CalculatorService {
public:
    void Add(::google::protobuf::RpcController* controller,
             const ::novanet::example::calculator::AddRequest* request,
             ::novanet::example::calculator::AddResponse* response,
             ::google::protobuf::Closure* done) override {
        if (controller == nullptr || request == nullptr ||
            response == nullptr) {
            if (controller != nullptr) {
                controller->SetFailed(
                    "null argument in CalculatorServiceImpl::Add");
            }
            return;
        }

        response->set_result(request->lhs() + request->rhs());

        if (done != nullptr) {
            done->Run();
        }
    }
};

class UniqueFd final {
public:
    explicit UniqueFd(int fd = -1) : fd_(fd) {
    }

    ~UniqueFd() {
        reset();
    }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        reset();

        fd_ = other.fd_;
        other.fd_ = -1;
        return *this;
    }

    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = fd;
    }

    [[nodiscard]] int get() const noexcept {
        return fd_;
    }

    [[nodiscard]] bool valid() const noexcept {
        return fd_ >= 0;
    }

private:
    int fd_{-1};
};

bool sendAll(int fd, const char* data, std::size_t len) {
    std::size_t sent = 0;

    while (sent < len) {
        const ssize_t n = ::send(fd, data + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }

        if (n == 0) {
            return false;
        }

        sent += static_cast<std::size_t>(n);
    }

    return true;
}

bool recvExact(int fd, char* data, std::size_t len) {
    std::size_t received = 0;

    while (received < len) {
        const ssize_t n = ::recv(fd, data + received, len - received, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }

        if (n == 0) {
            return false;
        }

        received += static_cast<std::size_t>(n);
    }

    return true;
}

UniqueFd connectWithRetry(uint16_t port) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        UniqueFd fd(::socket(AF_INET, SOCK_STREAM, 0));
        assert(fd.valid());

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);

        const int ok = ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        assert(ok == 1);

        if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr),
                      sizeof(addr)) == 0) {
            return fd;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    return UniqueFd(-1);
}

std::string makeUnaryAddRequestBytes(uint64_t requestId, int lhs, int rhs) {
    ::novanet::example::calculator::AddRequest addRequest;
    addRequest.set_lhs(lhs);
    addRequest.set_rhs(rhs);

    std::string addRequestBytes;
    assert(addRequest.SerializeToString(&addRequestBytes));

    novanet::rpc::UnaryRequestMeta requestMeta;
    requestMeta.set_service_name("CalculatorService");
    requestMeta.set_method_name("Add");
    requestMeta.set_request_payload(std::move(addRequestBytes));

    std::string metaBytes;
    assert(requestMeta.SerializeToString(&metaBytes));

    novanet::rpc::RpcMessage requestMsg(novanet::rpc::FrameType::UNARY_REQUEST,
                                        0, requestId, std::move(metaBytes));

    assert(requestMsg.valid());

    novanet::rpc::RpcCodec codec;

    std::string wireBytes;
    assert(codec.encodeToString(requestMsg, wireBytes));
    assert(!wireBytes.empty());

    return wireBytes;
}

novanet::rpc::RpcMessage readOneRpcMessageFromSocket(int fd) {
    std::string headerBytes;
    headerBytes.resize(novanet::rpc::RpcHeader::kFixedHeaderLen);

    const bool headerOk = recvExact(fd, headerBytes.data(), headerBytes.size());
    assert(headerOk);

    novanet::rpc::RpcHeader header;
    const bool decodedHeader = novanet::rpc::RpcHeader::decodeFrom(
        headerBytes.data(), headerBytes.size(), header);

    assert(decodedHeader);
    assert(header.isValid());

    const std::size_t payloadLen = static_cast<std::size_t>(
        header.totalLen - novanet::rpc::RpcHeader::kFixedHeaderLen);

    std::string payload;
    payload.resize(payloadLen);

    if (payloadLen > 0) {
        const bool payloadOk = recvExact(fd, payload.data(), payload.size());
        assert(payloadOk);
    }

    novanet::rpc::RpcMessage responseMsg(header, std::move(payload));
    assert(responseMsg.valid());

    return responseMsg;
}

} // namespace

int main() {
    using novanet::rpc::FrameType;

    novanet::base::Logger::setLogLevel(novanet::base::LogLevel::Info);

    std::promise<void> serverReadyPromise;
    std::future<void> serverReady = serverReadyPromise.get_future();

    std::atomic<novanet::net::EventLoop*> loopPtr{nullptr};

    /*
     * 重点：
     * EventLoop 必须在 serverThread 里创建，
     * 也必须在 serverThread 里 loop。
     */
    std::thread serverThread([&serverReadyPromise, &loopPtr]() mutable {
        novanet::net::EventLoop loop;
        loopPtr.store(&loop, std::memory_order_release);

        /*
         * 如果你的 InetAddress 构造函数不是 InetAddress(uint16_t)，
         * 这里改成你的真实接口，例如：
         *
         *   novanet::net::InetAddress listenAddr("127.0.0.1", kTestPort);
         */
        novanet::net::InetAddress listenAddr(kTestPort);

        novanet::rpc::FakeAiProvider aiProvider;
        novanet::rpc::RpcServer server(&loop, listenAddr,
                                       "RpcServerIntegrationTest", aiProvider);
        CalculatorServiceImpl calculator;

        std::string registerError;
        const bool registered =
            server.registerService(&calculator, &registerError);

        assert(registered);
        assert(registerError.empty());
        assert(server.serviceCount() == 1);

        assert(server.start());

        serverReadyPromise.set_value();

        loop.loop();
    });

    /*
     * 等 server 线程完成 bind/listen/start。
     */
    serverReady.wait();

    UniqueFd clientFd = connectWithRetry(kTestPort);
    assert(clientFd.valid());

    constexpr uint64_t kRequestId = 1001;

    const std::string requestBytes = makeUnaryAddRequestBytes(kRequestId, 1, 2);

    const bool sent =
        sendAll(clientFd.get(), requestBytes.data(), requestBytes.size());

    assert(sent);

    novanet::rpc::RpcMessage responseMsg =
        readOneRpcMessageFromSocket(clientFd.get());

    assert(responseMsg.frameType() == FrameType::UNARY_RESPONSE);
    assert(responseMsg.requestId() == kRequestId);
    assert(responseMsg.streamId() == 0);

    novanet::rpc::UnaryResponseMeta responseMeta;
    assert(responseMeta.ParseFromString(responseMsg.payload()));

    assert(responseMeta.error_code() == novanet::rpc::RPC_OK);
    assert(responseMeta.error_text().empty());
    assert(!responseMeta.response_payload().empty());

    ::novanet::example::calculator::AddResponse addResponse;
    assert(addResponse.ParseFromString(responseMeta.response_payload()));
    assert(addResponse.result() == 3);

    /*
     * 关闭 EventLoop。
     *
     * 如果你的 EventLoop::quit() 是线程安全的，这样可以。
     * 如果 quit() 内部 assertInLoopThread()，那需要改成 queueInLoop/runInLoop。
     */
    /*
     * 先关闭客户端 socket。
     *
     * 这样 server 端 TcpConnection 能收到 peer close，
     * 走正常 handleClose / connectDestroyed 流程。
     */
    clientFd.reset();

    /*
     * 给 server EventLoop 一点时间处理 EPOLLIN/EPOLLHUP/close 事件。
     *
     * 这是集成测试里的简化写法。
     * 后面如果 RpcServer/TcpServer 暴露 connection callback，
     * 可以改成 promise/future 等待连接 DOWN。
     */
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    novanet::net::EventLoop* loop = loopPtr.load(std::memory_order_acquire);
    assert(loop != nullptr);

    loop->quit();

    if (serverThread.joinable()) {
        serverThread.join();
    }

    std::cout << "[PASS] RpcServerIntegrationTest passed.\n";
    return 0;
}
