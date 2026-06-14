#include <cassert>
#include <string>

#include "novanet/rpc/sdk/Endpoint.h"

int main() {
    {
        std::string error;
        auto ep = novanet::rpc::sdk::Endpoint::parse("127.0.0.1:19090", &error);
        assert(ep.has_value());
        assert(ep->host() == "127.0.0.1");
        assert(ep->port() == 19090);
        assert(ep->toString() == "127.0.0.1:19090");
    }

    {
        std::string error;
        auto ep = novanet::rpc::sdk::Endpoint::parse("localhost:8080", &error);
        assert(ep.has_value());
        assert(ep->host() == "localhost");
        assert(ep->port() == 8080);
    }

    {
        std::string error;
        auto ep = novanet::rpc::sdk::Endpoint::parse("127.0.0.1", &error);
        assert(!ep.has_value());
    }

    {
        std::string error;
        auto ep = novanet::rpc::sdk::Endpoint::parse("127.0.0.1:", &error);
        assert(!ep.has_value());
    }

    {
        std::string error;
        auto ep = novanet::rpc::sdk::Endpoint::parse("127.0.0.1:0", &error);
        assert(!ep.has_value());
    }

    {
        std::string error;
        auto ep = novanet::rpc::sdk::Endpoint::parse("127.0.0.1:70000", &error);
        assert(!ep.has_value());
    }

    {
        std::string error;
        auto ep = novanet::rpc::sdk::Endpoint::parse("[::1]:19090", &error);
        assert(!ep.has_value());
    }

    return 0;
}