#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <utility>

#include "chat.pb.h"
#include "novanet/base/Logger.h"
#include "novanet/rpc/core/RpcStatus.h"
#include "novanet/rpc/sdk/ClientChannel.h"
#include "novanet/rpc/sdk/ClientContext.h"
#include "rpc_meta.pb.h"

namespace novanet::rpc::sdk {

/*
 * ClientReader<T> 是 NovaNet SDK 的 server streaming 读取对象。
 *
 * Phase 4 主要使用：
 *
 *   ClientReader<novanet::ai::chat::GenerateChunk>
 *
 * 目标 API：
 *
 *   auto reader = chat.Generate(&ctx, req);
 *
 *   GenerateChunk chunk;
 *   while (reader->Read(&chunk)) {
 *       ...
 *   }
 *
 *   RpcStatus status = reader->Finish();
 *
 * 职责：
 * - 把 RpcChannel::StreamCallbacks 转换成阻塞式 Read；
 * - 缓存 STREAM_DATA 中的 chunk；
 * - STREAM_END 后 Read 返回 false；
 * - Finish 返回最终 RpcStatus；
 * - Cancel 发送 STREAM_CANCEL；
 * - 内部用 mutex + condition_variable 保证线程安全。
 *
 * 不负责：
 * - 不解析 RpcMessage；
 * - 不处理 RpcCodec；
 * - 不直接操作 TcpConnection；
 * - 不做服务发现 / 负载均衡 / 重试；
 * - 不处理 client streaming / bidi streaming。
 */
template <typename T>
class ClientReader final {
public:
    using MessageType = T;
    using StreamCallbacks = ClientChannel::StreamCallbacks;
    using StreamHandle = ClientChannel::StreamHandle;

    ClientReader(std::shared_ptr<ClientChannel> channel, ClientContext* context);

    ~ClientReader();

    ClientReader(const ClientReader&) = delete;
    ClientReader& operator=(const ClientReader&) = delete;

    ClientReader(ClientReader&&) = delete;
    ClientReader& operator=(ClientReader&&) = delete;

    /*
     * 读取一个 streaming message。
     *
     * 返回：
     * - true：成功读取一个 message；
     * - false：stream 已结束 / 出错 / 被取消，并且内部队列已经读空。
     *
     * 注意：
     * - out 不允许为 nullptr；
     * - 如果 out == nullptr，直接返回 false。
     */
    [[nodiscard]] bool Read(T* out);

    /*
     * 等待 stream 完成，并返回最终状态。
     *
     * 一般使用方式：
     *
     *   while (reader->Read(&chunk)) {
     *       ...
     *   }
     *
     *   RpcStatus status = reader->Finish();
     */
    [[nodiscard]] novanet::rpc::RpcStatus Finish();

    /*
     * 主动取消 stream。
     *
     * 会：
     * - 标记本地 cancelled；
     * - 唤醒 Read / Finish；
     * - 调用 ClientChannel::cancelStream(streamId)；
     * - 更新 ClientContext。
     */
    void Cancel(std::string reason = "client cancelled");

    [[nodiscard]] bool finished() const;
    [[nodiscard]] bool cancelled() const;

    [[nodiscard]] std::uint32_t streamId() const;
    [[nodiscard]] std::uint64_t requestId() const;

    /*
     * ChatServiceStub 内部使用。
     *
     * 生成底层 RpcChannel 需要的 callbacks。
     * callbacks 捕获 weak_ptr<State>，不会悬空访问 ClientReader。
     */
    [[nodiscard]] StreamCallbacks makeCallbacks() const;

    /*
     * ChatServiceStub 在 openStream 成功后调用。
     */
    void bindStream(const StreamHandle& handle);

    /*
     * ChatServiceStub 在 openStream 失败时调用。
     */
    void markStartFailed(novanet::rpc::meta::RpcErrorCode errorCode,
                         std::string errorText);

private:
    struct State {
        explicit State(std::shared_ptr<ClientChannel> ch, ClientContext* ctx)
            : channel(std::move(ch)), context(ctx) {
        }

        std::shared_ptr<ClientChannel> channel;
        ClientContext* context{nullptr};

        mutable std::mutex mutex;
        std::condition_variable cv;

        std::queue<T> messages;

        bool started{false};
        bool finished{false};
        bool cancelled{false};

        std::uint32_t streamId{0};
        std::uint64_t requestId{0};

        novanet::rpc::RpcStatus finalStatus{novanet::rpc::RpcStatus::success()};
    };

private:
    static void pushData(const std::weak_ptr<State>& weakState,
                         std::uint32_t streamId, std::uint64_t sequence,
                         const novanet::ai::chat::GenerateChunk& chunk);

    static void markEnd(const std::weak_ptr<State>& weakState,
                        std::uint32_t streamId,
                        novanet::rpc::meta::RpcErrorCode errorCode,
                        std::string errorText);

    static void markError(const std::weak_ptr<State>& weakState,
                          std::uint32_t streamId,
                          novanet::rpc::meta::RpcErrorCode errorCode,
                          std::string errorText);

    static void setContextError(ClientContext* context,
                                novanet::rpc::meta::RpcErrorCode errorCode,
                                const std::string& errorText);

    static void clearContextError(ClientContext* context);

private:
    std::shared_ptr<State> state_;
};

template <typename T>
ClientReader<T>::ClientReader(std::shared_ptr<ClientChannel> channel,
                              ClientContext* context)
    : state_(std::make_shared<State>(std::move(channel), context)) {
}

template <typename T>
ClientReader<T>::~ClientReader() {
    /*
     * 用户提前销毁 reader 时，如果 stream 还没结束，主动 cancel。
     *
     * 注意：
     * Cancel 内部使用 weak/shared state，不会导致 late callback 悬空访问。
     */
    bool needCancel = false;

    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        needCancel = state_->started && !state_->finished && !state_->cancelled &&
                     state_->streamId != 0;
    }

    if (needCancel) {
        Cancel("ClientReader destroyed");
    }
}

template <typename T>
bool ClientReader<T>::Read(T* out) {
    if (out == nullptr) {
        return false;
    }

    std::unique_lock<std::mutex> lock(state_->mutex);

    state_->cv.wait(lock, [this]() {
        return !state_->messages.empty() || state_->finished || state_->cancelled;
    });

    if (!state_->messages.empty()) {
        *out = std::move(state_->messages.front());
        state_->messages.pop();
        return true;
    }

    return false;
}

template <typename T>
novanet::rpc::RpcStatus ClientReader<T>::Finish() {
    std::unique_lock<std::mutex> lock(state_->mutex);

    state_->cv.wait(lock,
                    [this]() { return state_->finished || state_->cancelled; });

    return state_->finalStatus;
}

template <typename T>
void ClientReader<T>::Cancel(std::string reason) {
    if (reason.empty()) {
        reason = "client cancelled";
    }

    std::shared_ptr<ClientChannel> channel;
    std::uint32_t streamId = 0;

    {
        std::lock_guard<std::mutex> lock(state_->mutex);

        if (state_->finished || state_->cancelled) {
            return;
        }

        state_->cancelled = true;
        state_->finished = true;
        state_->finalStatus = novanet::rpc::RpcStatus::failure(
            novanet::rpc::meta::RPC_CANCELLED, reason);

        /*
         * 用户主动 cancel 后，未读缓存直接丢弃。
         */
        std::queue<T> empty;
        state_->messages.swap(empty);

        streamId = state_->streamId;
        channel = state_->channel;

        if (state_->context != nullptr) {
            state_->context->cancel(reason);
            state_->context->setError(novanet::rpc::meta::RPC_CANCELLED, reason);
        }
    }

    state_->cv.notify_all();

    if (channel && streamId != 0) {
        static_cast<void>(channel->cancelStream(streamId, reason));
    }

    LOG_INFO << "[ClientReader] stream cancelled, streamId=" << streamId
             << ", reason=" << reason;
}

template <typename T>
bool ClientReader<T>::finished() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->finished;
}

template <typename T>
bool ClientReader<T>::cancelled() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->cancelled;
}

template <typename T>
std::uint32_t ClientReader<T>::streamId() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->streamId;
}

template <typename T>
std::uint64_t ClientReader<T>::requestId() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->requestId;
}

template <typename T>
typename ClientReader<T>::StreamCallbacks ClientReader<T>::makeCallbacks() const {
    std::weak_ptr<State> weakState = state_;

    StreamCallbacks callbacks;

    callbacks.onData = [weakState](std::uint32_t streamId, std::uint64_t sequence,
                                   const novanet::ai::chat::GenerateChunk& chunk) {
        ClientReader<T>::pushData(weakState, streamId, sequence, chunk);
    };

    callbacks.onEnd = [weakState](std::uint32_t streamId,
                                  novanet::rpc::meta::RpcErrorCode errorCode,
                                  std::string errorText) {
        ClientReader<T>::markEnd(weakState, streamId, errorCode,
                                 std::move(errorText));
    };

    callbacks.onError = [weakState](std::uint32_t streamId,
                                    novanet::rpc::meta::RpcErrorCode errorCode,
                                    std::string errorText) {
        ClientReader<T>::markError(weakState, streamId, errorCode,
                                   std::move(errorText));
    };

    return callbacks;
}

template <typename T>
void ClientReader<T>::bindStream(const StreamHandle& handle) {
    std::lock_guard<std::mutex> lock(state_->mutex);

    if (!handle.ok) {
        state_->started = true;
        state_->finished = true;
        state_->finalStatus = novanet::rpc::RpcStatus::failure(
            novanet::rpc::meta::RPC_UNKNOWN_ERROR,
            handle.errorText.empty() ? "open stream failed" : handle.errorText);

        setContextError(
            state_->context, novanet::rpc::meta::RPC_UNKNOWN_ERROR,
            handle.errorText.empty() ? "open stream failed" : handle.errorText);

        state_->cv.notify_all();
        return;
    }

    state_->started = true;
    state_->streamId = handle.streamId;
    state_->requestId = handle.requestId;

    if (state_->context != nullptr) {
        state_->context->setStreamId(handle.streamId);
        state_->context->setRequestId(handle.requestId);
        state_->context->clearError();
    }
}

template <typename T>
void ClientReader<T>::markStartFailed(novanet::rpc::meta::RpcErrorCode errorCode,
                                      std::string errorText) {
    if (errorText.empty()) {
        errorText = "open stream failed";
    }

    {
        std::lock_guard<std::mutex> lock(state_->mutex);

        state_->started = true;
        state_->finished = true;
        state_->cancelled = false;
        state_->finalStatus = novanet::rpc::RpcStatus::failure(errorCode, errorText);

        setContextError(state_->context, errorCode, errorText);
    }

    state_->cv.notify_all();
}

template <typename T>
void ClientReader<T>::pushData(const std::weak_ptr<State>& weakState,
                               std::uint32_t streamId, std::uint64_t sequence,
                               const novanet::ai::chat::GenerateChunk& chunk) {
    auto state = weakState.lock();
    if (!state) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);

        if (state->finished || state->cancelled) {
            LOG_WARN
                << "[ClientReader] drop late data after stream finished, streamId="
                << streamId;
            return;
        }

        if (state->streamId != 0 && state->streamId != streamId) {
            LOG_WARN << "[ClientReader] drop data for mismatched stream, expected="
                     << state->streamId << ", actual=" << streamId;
            return;
        }

        /*
         * Phase 4 当前只实例化 ClientReader<GenerateChunk>。
         * 如果未来支持更多 streaming 类型，需要让 RpcChannel::StreamCallbacks
         * 不再写死 GenerateChunk。
         */
        static_cast<void>(sequence);
        state->messages.emplace(chunk);
    }

    state->cv.notify_all();
}

template <typename T>
void ClientReader<T>::markEnd(const std::weak_ptr<State>& weakState,
                              std::uint32_t streamId,
                              novanet::rpc::meta::RpcErrorCode errorCode,
                              std::string errorText) {
    auto state = weakState.lock();
    if (!state) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);

        if (state->finished || state->cancelled) {
            return;
        }

        if (state->streamId != 0 && state->streamId != streamId) {
            LOG_WARN << "[ClientReader] ignore STREAM_END for mismatched stream, "
                        "expected="
                     << state->streamId << ", actual=" << streamId;
            return;
        }

        state->finished = true;

        if (errorCode == novanet::rpc::meta::RPC_OK) {
            state->finalStatus = novanet::rpc::RpcStatus::success();
            clearContextError(state->context);
        } else {
            if (errorText.empty()) {
                errorText = "stream finished with error";
            }

            state->finalStatus =
                novanet::rpc::RpcStatus::failure(errorCode, errorText);

            setContextError(state->context, errorCode, errorText);
        }
    }

    state->cv.notify_all();

    LOG_INFO << "[ClientReader] stream ended, streamId=" << streamId;
}

template <typename T>
void ClientReader<T>::markError(const std::weak_ptr<State>& weakState,
                                std::uint32_t streamId,
                                novanet::rpc::meta::RpcErrorCode errorCode,
                                std::string errorText) {
    auto state = weakState.lock();
    if (!state) {
        return;
    }

    if (errorText.empty()) {
        errorText = "stream error";
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);

        if (state->finished || state->cancelled) {
            return;
        }

        if (state->streamId != 0 && state->streamId != streamId) {
            LOG_WARN
                << "[ClientReader] ignore error for mismatched stream, expected="
                << state->streamId << ", actual=" << streamId;
            return;
        }

        /*
         * 出错时不清空已有 messages。
         * 用户仍然可以先 Read() 已经收到的 chunk，
         * 最后 Finish() 拿到错误状态。
         */
        state->finished = true;
        state->finalStatus = novanet::rpc::RpcStatus::failure(errorCode, errorText);

        setContextError(state->context, errorCode, errorText);
    }

    state->cv.notify_all();

    LOG_WARN << "[ClientReader] stream error, streamId=" << streamId
             << ", error=" << errorText;
}

template <typename T>
void ClientReader<T>::setContextError(ClientContext* context,
                                      novanet::rpc::meta::RpcErrorCode errorCode,
                                      const std::string& errorText) {
    if (context != nullptr) {
        context->setError(errorCode, errorText);
    }
}

template <typename T>
void ClientReader<T>::clearContextError(ClientContext* context) {
    if (context != nullptr) {
        context->clearError();
    }
}

}  // namespace novanet::rpc::sdk