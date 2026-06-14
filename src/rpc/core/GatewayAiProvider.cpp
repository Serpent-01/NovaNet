#include "novanet/rpc/core/GatewayAiProvider.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <utility>

#include "novanet/base/Logger.h"
#include "novanet/rpc/core/AiProvider.h"

namespace novanet::rpc {
namespace {

using Json = nlohmann::json;

void initCurlGlobalOnce() {
    static std::once_flag once;
    std::call_once(once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::string trimRightCR(std::string line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

std::string statusMessage(const std::string& code, const std::string& message) {
    if (code.empty()) {
        return message;
    }
    if (message.empty()) {
        return code;
    }
    return code + ": " + message;
}

bool buildGatewayRequestJson(const novanet::ai::chat::GenerateRequest& request,
                             const GatewayAiProvider::Options& options,
                             std::string& outJson, std::string& errorText) {
    if (request.messages_size() <= 0) {
        errorText = "GenerateRequest has no messages";
        return false;
    }
    Json root;
    root["request_id"] = "";
    root["stream_id"] = "";
    root["model"] = options.model;
    root["temperature"] = 0.7;
    root["max_tokens"] = 1024;

    Json messages = Json::array();

    for (int i = 0; i < request.messages_size(); ++i) {
        const auto& msg = request.messages(i);
        if (msg.role().empty()) {
            errorText = "GenerateRequest contains message with empty role";
            return false;
        }

        if (msg.content().empty()) {
            errorText = "GenerateRequest contains message with empty content";
            return false;
        }
        Json item;
        item["role"] = msg.role();
        item["content"] = msg.content();
        messages.push_back(std::move(item));
    }
    root["messages"] = std::move(messages);
    //会把 JSON 对象转成字符串。
    outJson = root.dump();
    return true;
}

struct CurlStreamContext {
    GatewayAiProvider::Options options;

    AiProvider::ChunkSink onChunk;
    AiProvider::StopChecker shouldStop;

    std::string pendingLine;

    AiProvider::Status finalStatus{AiProvider::Status::success()};

    bool stopped{false};
    bool sawEnd{false};
};
bool processNdjsonLine(CurlStreamContext& ctx, std::string line) {
    //去掉行尾 CR
    line = trimRightCR(std::move(line));
    //空行直接忽略
    if (line.empty()) {
        return true;
    }
    //解析 JSON
    Json obj = Json::parse(line, nullptr, false);
    if (obj.is_discarded() || !obj.is_object()) {
        ctx.finalStatus = AiProvider::Status::providerError(
            "invalid NDJSON line from AI bridge: " + line.substr(0, 300));
        return false;
    }
    const std::string type = obj.value("type", "");
    if (type == "chunk") {
        novanet::ai::chat::GenerateChunk chunk;
        //从 JSON 里取 index，没有就默认为 0。
        const auto index = obj.value("index", 0);
        chunk.set_index(static_cast<std::uint32_t>(std::max(index, 0)));

        chunk.set_delta(obj.value("delta", ""));

        const std::string finishReason = obj.value("finish_reason", "");

        if (!finishReason.empty()) {
            chunk.set_finish_reason(finishReason);
        }
        // 解析出 GenerateChunk 后,立即交给上层
        //如果 onChunk 返回失败，说明上层不想继续了。
        AiProvider::Status status = ctx.onChunk(chunk);
        if (!status.ok()) {
            ctx.finalStatus = std::move(status);
            return false;
        }

        return true;
    }
    if (type == "end") {
        ctx.sawEnd = true;
        return true;
    }
    if (type == "error") {
        const std::string code = obj.value("code", "provider_error");
        const std::string message = obj.value("message", "");
        ctx.finalStatus = AiProvider::Status::providerError(statusMessage(code, message));
        return false;
    }
    ctx.finalStatus =
        AiProvider::Status::providerError("unknown NDJSON event type: " + type);
    return false;
}
// libcurl 的写回调函数。
size_t curlWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t totalBytes = size * nmemb;
    auto* ctx = static_cast<CurlStreamContext*>(userdata);

    if (ctx == nullptr || ptr == nullptr) {
        return 0;
    }
    if (ctx->stopped) {
        return 0;
    }
    if (ctx->shouldStop) {
        AiProvider::Status stopStatus = ctx->shouldStop();
        if (!stopStatus.ok()) {
            ctx->finalStatus = std::move(stopStatus);
            ctx->stopped = true;
            return 0;
        }
    }
    // curl 给你的可能是半行数据，所以先累积到 pendingLine。
    ctx->pendingLine.append(ptr, totalBytes);
    if (ctx->pendingLine.size() > ctx->options.maxLineBytes) {
        ctx->finalStatus =
            AiProvider::Status::providerError("AI bridge NDJSON line too large");
        ctx->stopped = true;

        return 0;
    }

    while (true) {
        const auto pos = ctx->pendingLine.find('\n');
        if (pos == std::string::npos) {
            break;
        }
        std::string line = ctx->pendingLine.substr(0, pos);
        ctx->pendingLine.erase(0, pos + 1);

        if (!processNdjsonLine(*ctx, std::move(line))) {
            ctx->stopped = true;
            return 0;
        }
        //每处理完一行后再次检查 shouldStop
        if (ctx->shouldStop) {
            AiProvider::Status stopStatus = ctx->shouldStop();
            if (!stopStatus.ok()) {
                ctx->finalStatus = std::move(stopStatus);
                ctx->stopped = true;
                return 0;
            }
        }
    }
    return totalBytes;
}

}  // namespace

GatewayAiProvider::GatewayAiProvider() : GatewayAiProvider(Options{}) {
}

GatewayAiProvider::GatewayAiProvider(Options options) : options_(std::move(options)) {
    initCurlGlobalOnce();
}

AiProvider::Status GatewayAiProvider::generateStreaming(
    const novanet::ai::chat::GenerateRequest& request, ChunkSink onChunk,
    StopChecker shouldStop) {
    if (!onChunk) {
        return AiProvider::Status::invalidRequest("ChunkSink is empty");
    }

    std::string requestJson;
    std::string errorText;
    //这里把 protobuf 请求转成 JSON 字符串。
    if (!buildGatewayRequestJson(request, options_, requestJson, errorText)) {
        return AiProvider::Status::invalidRequest(errorText);
    }
    //创建一个 libcurl 请求对象。
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        return AiProvider::Status::providerError("curl_easy_init failed");
    }

    CurlStreamContext ctx;
    ctx.options = options_;
    ctx.onChunk = std::move(onChunk);
    ctx.shouldStop = std::move(shouldStop);

    //这里设置 HTTP 请求头。
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/x-ndjson");
    //设置 URL
    curl_easy_setopt(curl, CURLOPT_URL, options_.endpoint.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestJson.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(requestJson.size()));

    // 设置流式响应回调
    // 服务端每返回一批数据，就调用 curlWriteCallback。
    // 并且把 &ctx 作为 userdata 传给它。
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, options_.connectTimeoutMs);

    //设置总超时
    if (options_.totalTimeoutMs > 0) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, options_.totalTimeoutMs);
    }

    //设置 low speed 超时
    if (options_.lowSpeedTimeSeconds > 0 && options_.lowSpeedLimitBytesPerSecond > 0) {
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, options_.lowSpeedTimeSeconds);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT,
                         options_.lowSpeedLimitBytesPerSecond);
    }

    CURLcode rc = curl_easy_perform(curl);
    //获取 HTTP 状态码
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    //释放资源
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    /*
     * 如果 curlWriteCallback 因 cancel/backpressure/timeout/consumer stop
     * 主动返回 0，中断 curl，libcurl 通常返回 CURLE_WRITE_ERROR。
     * 这种情况下以 ctx.finalStatus 为准。
     */
    if (!ctx.finalStatus.ok()) {
        LOG_WARN << "[GatewayAiProvider] stream stopped, reason="
                 << ctx.finalStatus.errorText;
        return ctx.finalStatus;
    }

    if (rc != CURLE_OK) {
        std::ostringstream oss;
        oss << "curl_easy_perform failed: " << curl_easy_strerror(rc);
        return AiProvider::Status::providerError(oss.str());
    }

    if (httpCode < 200 || httpCode >= 300) {
        std::ostringstream oss;
        oss << "AI bridge HTTP status=" << httpCode;
        return AiProvider::Status::providerError(oss.str());
    }
    /*
     * 正常情况下 Python Bridge 会发送 {"type":"end",...}。
     */
    if (!ctx.sawEnd) {
        return AiProvider::Status::providerError(
            "AI bridge stream ended without end event");
    }

    return AiProvider::Status::success();
}

}  // namespace novanet::rpc
