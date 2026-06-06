#include "novanet/rpc/sdk/Endpoint.h"

#include <cctype>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

namespace novanet::rpc::sdk {

namespace {

void setError(std::string* errorText, std::string error) {
    if (errorText != nullptr) {
        *errorText = std::move(error);
    }
}

std::string trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end &&
           std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }

    while (end > begin &&
           std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

bool containsWhitespace(std::string_view text) {
    for (char ch : text) {
        if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
            return true;
        }
    }
    return false;
}

bool containsUnsupportedHostChar(std::string_view host) {
    for (char ch : host) {
        switch (ch) {
            case '/':
            case '\\':
            case '@':
            case '[':
            case ']':
                return true;

            default:
                break;
        }
    }

    return false;
}
bool isValidHost(std::string_view host, std::string* errorText) {
    if (host.empty()) {
        setError(errorText, "endpoint host is empty");
        return false;
    }
    if (containsWhitespace(host)) {
        setError(errorText, "endpoint host contains whitespace");
        return false;
    }

    if (containsUnsupportedHostChar(host)) {
        setError(errorText, "endpoint host contains unsupported character");
        return false;
    }
    /*
     * Phase 4 SDK 暂不支持 IPv6。
     * 如果 host 中仍然出现 ':'，说明用户大概率传了 IPv6 或非法地址。
     */
    if (host.find(':') != std::string_view::npos) {
        setError(errorText, "IPv6 endpoint is not supported in Phase 4 SDK");
        return false;
    }
    return true;
}

bool parsePort(std::string_view portText, std::uint16_t* outPort,
               std::string* errorText) {
    if (outPort == nullptr) {
        setError(errorText, "outPort is null");
        return false;
    }
    if (portText.empty()) {
        setError(errorText, "endpoint port is empty");
        return false;
    }
    if (containsWhitespace(portText)) {
        setError(errorText, "endpoint port contains whitespace");
        return false;
    }

    std::uint32_t portValue = 0;

    const char* begin = portText.data();
    const char* end = begin + portText.size();

    const auto result = std::from_chars(begin, end, portValue);

    if (result.ec != std::errc{} || result.ptr != end) {
        setError(errorText, "endpoint port is not a valid integer");
        return false;
    }

    if (portValue == 0 || portValue > 65535) {
        setError(errorText, "endpoint port must be in range 1..65535");
        return false;
    }

    *outPort = static_cast<uint16_t>(portValue);
    return true;
}

}  // namespace

Endpoint::Endpoint(std::string host, std::uint16_t port)
    : host_(std::move(host)), port_(port) {
}

std::optional<Endpoint> Endpoint::parse(const std::string& target,
                                        std::string* errorText) {
    const std::string text = trim(target);

    if (text.empty()) {
        setError(errorText, "endpoint target is empty");
        return std::nullopt;
    }

    /*
     * Phase 4 SDK 暂不支持 scheme。
     * 例如：
     *   novanet://127.0.0.1:19090
     */
    if (text.find("://") != std::string::npos) {
        setError(errorText, "endpoint scheme is not supported in Phase 4 SDK");
        return std::nullopt;
    }

    const std::size_t firstColon = text.find(':');
    const std::size_t lastColon = text.rfind(':');

    if (firstColon == std::string::npos) {
        setError(errorText, "endpoint target must be in host:port format");
        return std::nullopt;
    }
    /*
     * 多个冒号基本意味着 IPv6 或非法格式。
     * Phase 4 先不支持 IPv6。
     */
    if (firstColon != lastColon) {
        setError(errorText, "IPv6 endpoint is not supported in Phase 4 SDK");
        return std::nullopt;
    }

    std::string host = trim(std::string_view(text.data(), firstColon));

    std::string portString = trim(std::string_view(text.data() + firstColon + 1,
                                                   text.size() - firstColon - 1));

    if (!isValidHost(host, errorText)) {
        return std::nullopt;
    }
    std::uint16_t port = 0;
    if (!parsePort(portString, &port, errorText)) {
        return std::nullopt;
    }

    return Endpoint(std::move(host), port);
}

std::string Endpoint::toString() const {
    if (!valid()) {
        return {};
    }

    return host_ + ":" + std::to_string(port_);
}

}  // namespace novanet::rpc::sdk