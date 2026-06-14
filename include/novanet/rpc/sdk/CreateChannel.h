#pragma once

#include <memory>
#include <string>

#include "novanet/rpc/sdk/ChannelOptions.h"
#include "novanet/rpc/sdk/ClientChannel.h"
#include "novanet/rpc/sdk/Endpoint.h"

namespace novanet::rpc::sdk {
/*
 * CreateChannel 是 NovaNet Phase 4 SDK 的用户入口。
 *
 * 目标 API：
 *
 *   auto channel = novanet::rpc::CreateChannel("127.0.0.1:19090");
 *
 *   CalculatorServiceStub calculator(channel);
 *   calculator.Add(&ctx, req, &resp);
 *
 * 注意：
 * - 返回的是 sdk::ClientChannel；
 * - CreateChannel 不主动连接；
 * - 真正连接由 ClientChannel::connect() 或 Stub 调用时触发。
 */
[[nodiscard]] std::shared_ptr<sdk::ClientChannel> CreateChannel(
    const std::string& target);
/*
 * 带 options 的创建入口。
 */
[[nodiscard]] std::shared_ptr<sdk::ClientChannel> CreateChannel(
    const std::string& target, const sdk::ChannelOptions& options);

/*
 * 带错误输出的创建入口。
 *
 * 成功：
 *   返回非空 channel；
 *   errorText 被清空。
 *
 * 失败：
 *   返回 nullptr；
 *   errorText 保存原因。
 */
[[nodiscard]] std::shared_ptr<sdk::ClientChannel> CreateChannel(
    const std::string& target, std::string* errorText);

/*
 * 带 options + errorText 的完整入口。
 */
[[nodiscard]] std::shared_ptr<sdk::ClientChannel> CreateChannel(
    const std::string& target, const sdk::ChannelOptions& options,
    std::string* errorText);
/*
 * 如果上层已经解析好了 Endpoint，也可以直接创建。
 *
 * 这个接口主要给测试或高级 SDK 内部使用。
 */
[[nodiscard]] std::shared_ptr<sdk::ClientChannel> CreateChannel(
    sdk::Endpoint endpoint, sdk::ChannelOptions options = sdk::ChannelOptions{});

}  // namespace novanet::rpc::sdk
