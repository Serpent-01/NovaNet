#pragma once

#include <cstddef>
#include <string>

#include "chat.pb.h"
#include "novanet/rpc/core/AiProvider.h"

namespace novanet::rpc {

/*
 * GatewayAiProvider:
 *
 * 作用：
 * - 连接 NovaNet 和 Python AI Bridge；
 * - 输入 chat::GenerateRequest；
 * - HTTP POST 到 Python Bridge 的 /chat/stream；
 * - 读取 NDJSON streaming；
 * - 每个 chunk 转成 chat::GenerateChunk；
 * - 调用 onChunk(chunk)，最终由 StreamResponder 转成 STREAM_DATA。
 *
 * 它不负责：
 * - 不认识 RpcMessage；
 * - 不认识 TcpConnection；
 * - 不认识 Buffer；
 * - 不直接发送 STREAM_DATA；
 * - 不直接接 DeepSeek/OpenAI 原始 API。
 */
class GatewayAiProvider final : public AiProvider {
public:
    struct Options {
        std::string endpoint{"http://127.0.0.1:8000/chat/stream"};
        std::string model{"deepseek-chat"};

        long connectTimeoutMs{3000};

        /*
         * totalTimeoutMs:
         *   0 表示不设置总超时。
         *   streaming 请求可能持续较久，所以默认不设总超时。
         */
        long totalTimeoutMs{0};

        /*
         * 如果长时间没有任何数据，libcurl 可以中断。
         */
        long lowSpeedTimeSeconds{60};
        long lowSpeedLimitBytesPerSecond{1};

        /*
         * 防止 Python Bridge 返回异常超长单行。
         */
        std::size_t maxLineBytes{1024 * 1024};
    };

    GatewayAiProvider();
    explicit GatewayAiProvider(Options options);

    ~GatewayAiProvider() override = default;

    GatewayAiProvider(const GatewayAiProvider&) = delete;
    GatewayAiProvider& operator=(const GatewayAiProvider&) = delete;

    GatewayAiProvider(GatewayAiProvider&&) = delete;
    GatewayAiProvider& operator=(GatewayAiProvider&&) = delete;

    [[nodiscard]] Status generateStreaming(
        const novanet::ai::chat::GenerateRequest& request, ChunkSink onChunk,
        StopChecker shouldStop) override;

private:
    Options options_;
};

}  // namespace novanet::rpc