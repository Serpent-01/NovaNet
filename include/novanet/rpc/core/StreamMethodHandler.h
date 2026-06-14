#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "novanet/rpc/core/RpcStatus.h"
#include "novanet/rpc/core/StreamResponder.h"

namespace novanet::rpc {

/*
 * StreamMethodHandler 是服务端 streaming 业务处理抽象。
 *
 * 它把具体业务 request bytes 适配成异步 streaming 任务，避免
 * RpcDispatcher 直接认识 chat.proto、AiProvider 或具体生成逻辑。
 */
class StreamMethodHandler {
public:
    struct StartContext {
        std::uint32_t streamId{0};
        std::uint64_t requestId{0};
        std::string requestPayload;
        std::shared_ptr<StreamResponder> responder;
    };

    virtual ~StreamMethodHandler() = default;

    StreamMethodHandler(const StreamMethodHandler&) = delete;
    StreamMethodHandler& operator=(const StreamMethodHandler&) = delete;

    StreamMethodHandler(StreamMethodHandler&&) = delete;
    StreamMethodHandler& operator=(StreamMethodHandler&&) = delete;

    [[nodiscard]] virtual std::string serviceName() const = 0;
    [[nodiscard]] virtual std::string methodName() const = 0;

    /*
     * 必须只提交异步任务或完成轻量校验，不得在 EventLoop 线程执行
     * 慢速生成。失败时返回明确 RpcStatus。
     */
    [[nodiscard]] virtual RpcStatus start(StartContext context) = 0;

protected:
    StreamMethodHandler() = default;
};

}  // namespace novanet::rpc
