#pragma once

#include <cstddef>

namespace novanet::rpc::sdk {

/*
 * ChannelOptions 保存客户端连接级配置。
 *
 * 它只放配置，不放逻辑。
 *
 * 使用位置：
 * - CreateChannel(target, options)
 * - ClientChannel::create(endpoint, options)
 * - ClientChannel 内部转换为 RpcChannel::Options / RpcClient 配置
 *
 * Phase 4 SDK 范围：
 * - 单机客户端连接；
 * - 不做服务发现；
 * - 不做负载均衡；
 * - 不做重试策略；
 * - 不做认证；
 * - 不做拦截器。
 */
struct ChannelOptions {
    /*
     * 连接超时时间，单位：秒。
     *
     * 用于 ClientChannel::connect()。
     * 如果底层 RpcClient 目前还没有异步连接超时，
     * 可以先保留该字段，后续接入 TimerQueue。
     */
    double connectTimeoutSeconds{3.0};

    /*
     * 默认 unary RPC 超时时间，单位：秒。
     *
     * 如果 ClientContext 没有单独设置 timeout，
     * CalculatorServiceStub / ClientChannel::callUnary 可以使用该默认值。
     */
    double defaultRpcTimeoutSeconds{5.0};

    /*
     * 是否在连接成功后自动启动 heartbeat。
     *
     * true:
     *   ClientChannel connect 成功后启动 heartbeat 定时器。
     *
     * false:
     *   不自动启动 heartbeat。
     */
    bool startHeartbeat{true};

    /*
     * heartbeat ping 发送间隔，单位：秒。
     *
     * 对应 RpcChannel::sendHeartbeatPing() 的周期。
     */
    double heartbeatIntervalSeconds{10.0};

    /*
     * heartbeat 检查间隔，单位：秒。
     *
     * 对应 RpcChannel::checkHeartbeatTimeout() 的周期。
     */
    double heartbeatCheckIntervalSeconds{5.0};

    /*
     * heartbeat 超时时间，单位：秒。
     *
     * 如果超过该时间没有收到 HEARTBEAT_PONG，
     * ClientChannel / RpcChannel 应认为连接不可用。
     */
    double heartbeatTimeoutSeconds{30.0};

    /*
     * stream idle timeout，单位：秒。
     *
     * 用于 server streaming。
     * 如果某个 stream 长时间没有收到 DATA / END，
     * RpcChannel 应通过 TimerQueue 周期扫描并触发 timeout。
     */
    double streamIdleTimeoutSeconds{60.0};

    /*
     * stream timeout 扫描间隔，单位：秒。
     *
     * 对应 RpcChannel::checkStreamTimeouts() 的周期。
     */
    double streamTimeoutScanIntervalSeconds{5.0};

    /*
     * 发送侧 high water mark，单位：字节。
     *
     * 用于限制客户端发送侧 outputBuffer 膨胀。
     *
     * 例如：
     * - 发送 STREAM_OPEN
     * - 发送 STREAM_CANCEL
     * - 发送 HEARTBEAT_PING
     *
     * 如果 TcpConnection::outputBufferSize() 超过该值，
     * RpcChannel 可以主动关闭连接，避免无限堆积。
     *
     * 0 表示禁用该检查。
     */
    std::size_t sendHighWaterMarkBytes{8 * 1024 * 1024};

    /*
     * 客户端节点名。
     *
     * 用于 HeartbeatMeta.node_id。
     * Phase 4 本地项目中主要用于日志和调试。
     */
    const char* nodeId{"novanet-client"};
};

}  // namespace novanet::rpc::sdk