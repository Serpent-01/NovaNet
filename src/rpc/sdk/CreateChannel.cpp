#include "novanet/rpc/sdk/CreateChannel.h"

#include <utility>

#include "novanet/base/Logger.h"

namespace novanet::rpc::sdk {
std::shared_ptr<sdk::ClientChannel> CreateChannel(const std::string& target) {
    return CreateChannel(target, sdk::ChannelOptions{}, nullptr);
}
std::shared_ptr<sdk::ClientChannel> CreateChannel(const std::string& target,
                                                  std::string* errorText) {
    return CreateChannel(target, sdk::ChannelOptions{}, errorText);
}

std::shared_ptr<sdk::ClientChannel> CreateChannel(const std::string& target,
                                                  const sdk::ChannelOptions& options,
                                                  std::string* errorText) {
    if (errorText != nullptr) {
        errorText->clear();
    }

    std::string parseError;
    auto endpoint = sdk::Endpoint::parse(target, &parseError);

    if (!endpoint.has_value()) {
        if (errorText != nullptr) {
            *errorText =
                parseError.empty() ? "failed to parse channel target" : parseError;
        }

        LOG_ERROR << "[CreateChannel] invalid target=" << target << ", error="
                  << (parseError.empty() ? "failed to parse channel target"
                                         : parseError);

        return nullptr;
    }

    auto channel = sdk::ClientChannel::create(std::move(*endpoint), options);

    if (!channel) {
        const std::string error = "failed to create ClientChannel";

        if (errorText != nullptr) {
            *errorText = error;
        }

        LOG_ERROR << "[CreateChannel] " << error << ", target=" << target;

        return nullptr;
    }

    LOG_INFO << "[CreateChannel] channel created, target=" << target;

    return channel;
}

std::shared_ptr<sdk::ClientChannel> CreateChannel(sdk::Endpoint endpoint,
                                                  sdk::ChannelOptions options) {
    if (!endpoint.valid()) {
        LOG_ERROR << "[CreateChannel] invalid Endpoint";
        return nullptr;
    }

    auto channel =
        sdk::ClientChannel::create(std::move(endpoint), std::move(options));

    if (!channel) {
        LOG_ERROR << "[CreateChannel] failed to create ClientChannel from Endpoint";
        return nullptr;
    }

    LOG_INFO << "[CreateChannel] channel created from Endpoint";

    return channel;
}

}  // namespace novanet::rpc::sdk
