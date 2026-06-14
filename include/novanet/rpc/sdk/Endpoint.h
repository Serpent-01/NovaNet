#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace novanet::rpc::sdk {
class Endpoint final {
public:
    Endpoint() = default;

    Endpoint(std::string hos, std::uint16_t port);

    /*
     * 解析 target。
     *
     * 成功：
     *   返回 Endpoint。
     *
     * 失败：
     *   返回 std::nullopt；
     *   如果 errorText != nullptr，会写入错误原因。
     *
     * 支持：
     *   "127.0.0.1:19090"
     *   "localhost:19090"
     *
     * 不支持：
     *   "novanet://127.0.0.1:19090"
     *   "[::1]:19090"
     *   "127.0.0.1"
     *   "127.0.0.1:"
     */
    [[nodiscard]] static std::optional<Endpoint> parse(
        const std::string& target, std::string* errorText = nullptr);

    [[nodiscard]] const std::string& host() const noexcept {
        return host_;
    }

    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_;
    }

    [[nodiscard]] bool valid() const noexcept {
        return !host_.empty() && port_ != 0;
    }

    [[nodiscard]] std::string toString() const;

private:
    std::string host_{};
    std::uint16_t port_{0};
};
}  // namespace novanet::rpc::sdk