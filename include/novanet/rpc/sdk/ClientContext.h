#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "novanet/base/Timestamp.h"
#include "rpc_meta.pb.h"

namespace novanet::rpc::sdk {
/*
 * ClientContext 表示一次客户端 RPC 调用的上下文。
 *
 * 使用方式：
 *   ClientContext ctx;
 *   ctx.setTimeoutSeconds(3.0);
 *   ctx.setMetadata("trace_id", "xxx");
 *
 *   stub.Add(&ctx, req, &resp);
 *
 * Phase 4 SDK 范围：
 * - 支持 timeout / deadline；
 * - 支持 metadata；
 * - 支持 cancel 标记；
 * - 保存 requestId / streamId；
 * - 保存最终 errorCode / errorText；
 * - 不做服务发现；
 * - 不做负载均衡；
 * - 不做重试；
 * - 不做认证；
 * - 不做拦截器。
 *
 * 线程模型：
 * - ClientContext 通常由调用线程创建；
 * - Channel / RpcChannel 会写入 requestId / streamId / error；
 * - streaming reader 或用户线程可能调用 cancel()；
 * - 所以内部对可变状态做了基本同步保护。
 */
class ClientContext final {
public:
    using MetadataMap = std::unordered_map<std::string, std::string>;

    ClientContext();

    ~ClientContext() = default;

    ClientContext(const ClientContext&) = delete;
    ClientContext& operator=(const ClientContext&) = delete;

    ClientContext(ClientContext&&) = delete;
    ClientContext& operator=(ClientContext&&) = delete;

    /*
     * 设置相对超时时间，单位秒。
     *
     * seconds > 0:
     *   deadline = Timestamp::now() + seconds
     *
     * seconds <= 0:
     *   清除 deadline，表示不在 context 层设置超时。
     *
     * 如果没有设置 deadline，Channel 可以使用 ChannelOptions.defaultRpcTimeout。
     */
    void setTimeoutSeconds(double seconds);

    /*
     * 设置绝对 deadline。
     *
     * deadline.valid() == false 时，等价于 clearDeadline()。
     */
    void setDeadline(novanet::base::Timestamp deadline);

    void clearDeadline();

    [[nodiscard]] bool hasDeadline() const;

    [[nodiscard]] novanet::base::Timestamp deadline() const;

    /*
     * 返回剩余超时时间，单位秒。
     *
     * - 没有 deadline：返回 -1.0
     * - 已超时：返回 0.0
     * - 未超时：返回剩余秒数
     */

    [[nodiscard]] double remainingTimeoutSeconds() const;

    [[nodiscard]] bool expired() const;

    /*
     * metadata。
     *
     * 会被 Channel 写入 UnaryRequestMeta.metadata
     * 或 StreamOpenMeta.metadata。
     */
    void setMetadata(std::string key, std::string value);

    [[nodiscard]] std::optional<std::string> findMetadata(
        const std::string& key) const;

    [[nodiscard]] MetadataMap metadata() const;

    void removeMetadata(const std::string& key);

    void clearMetadata();

    /*
     * cancel。
     *
     * 对 unary：
     *   Channel 可以在等待 response 时检查 cancelled()。
     *
     * 对 streaming：
     *   ClientReader::Cancel() / ChatServiceStub 可以调用 cancel()，
     *   然后 Channel 发送 STREAM_CANCEL。
     */
    void cancel(std::string reason = "client cancelled");

    [[nodiscard]] bool cancelled() const noexcept;

    [[nodiscard]] std::string cancelReason() const;

    /*
     * requestId / streamId 由 Channel / RpcChannel 写入。
     *
     * 用户一般只读，不应该手动设置。
     */
    void setRequestId(std::uint64_t requestId) noexcept;
    [[nodiscard]] std::uint64_t requestId() const noexcept;

    void setStreamId(std::uint32_t streamId) noexcept;
    [[nodiscard]] std::uint32_t streamId() const noexcept;

    /*
     * 最终错误状态。
     *
     * 成功：
     *   errorCode = RPC_OK
     *   errorText 为空
     *
     * 失败：
     *   errorCode != RPC_OK
     *   errorText 保存错误原因
     */
    void setError(novanet::rpc::meta::RpcErrorCode errorCode, std::string errorText);

    void clearError();

    [[nodiscard]] bool failed() const;

    [[nodiscard]] novanet::rpc::meta::RpcErrorCode errorCode() const;

    [[nodiscard]] std::string errorText() const;

    /*
     * 重置上下文。
     */
    void reset();

private:
    mutable std::mutex mutex_;

    bool hasDeadline_{false};
    novanet::base::Timestamp deadline_{novanet::base::Timestamp::invalid()};

    MetadataMap metadata_{};

    std::atomic<bool> cancelled_{false};
    std::string cancelReason_{};

    std::atomic<std::uint64_t> requestId_{0};
    std::atomic<std::uint32_t> streamId_{0};

    novanet::rpc::meta::RpcErrorCode errorCode_{novanet::rpc::meta::RPC_OK};
    std::string errorText_{};
};

}  // namespace novanet::rpc::sdk