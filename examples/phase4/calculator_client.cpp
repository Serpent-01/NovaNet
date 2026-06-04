#include <cstdint>
#include <chrono>
#include <iostream>
#include <string>

#include "Phase4ExampleSupport.h"
#include "novanet/net/InetAddress.h"
#include "novanet/rpc/core/RpcClient.h"

int main(int argc, char** argv) {
    using namespace novanet::examples::phase4;

    const std::string host = argOr(argc, argv, 1, kLocalhost);
    const std::uint16_t port = parsePort(argc, argv, 2, kCalculatorPort);
    const std::int64_t lhs = argc > 3 ? std::stoll(argv[3]) : 1;
    const std::int64_t rhs = argc > 4 ? std::stoll(argv[4]) : 2;

    novanet::rpc::RpcClient client(novanet::net::InetAddress(host, port),
                                   "phase4_calculator_client");

    std::string errorText;
    if (!client.connect(&errorText)) {
        std::cerr << "connect failed: " << errorText << "\n";
        return 1;
    }

    novanet::example::calculator::AddRequest request;
    request.set_lhs(lhs);
    request.set_rhs(rhs);

    novanet::example::calculator::AddResponse response;
    const auto status = client.callUnary(
        "novanet.example.calculator.CalculatorService", "Add", request,
        &response, std::chrono::seconds(3));

    if (status.failed()) {
        std::cerr << "RPC failed: " << status.toString() << "\n";
        return 1;
    }

    std::cout << lhs << " + " << rhs << " = " << response.result() << "\n";
    client.disconnect();
    return 0;
}
