#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "calculator.pb.h"
#include "novanet/base/Logger.h"
#include "novanet/rpc/sdk/CalculatorServiceStub.h"
#include "novanet/rpc/sdk/ChannelOptions.h"
#include "novanet/rpc/sdk/ClientContext.h"
#include "novanet/rpc/sdk/CreateChannel.h"

namespace calculator = novanet::example::calculator;

struct ThreadResult {
    int success = 0;
    int failed = 0;
    int timeout = 0;
    std::vector<double> latenciesMs;
};

static double percentile(std::vector<double>& values, double p) {
    if (values.empty()) {
        return 0.0;
    }

    std::sort(values.begin(), values.end());

    std::size_t index = static_cast<std::size_t>((p / 100.0) * (values.size() - 1));
    return values[index];
}

int main(int argc, char* argv[]) {
    novanet::base::Logger::setLogLevel(novanet::base::LogLevel::Warn);

    std::string target = "127.0.0.1:19090";
    int concurrency = 1;
    int requestsPerThread = 1000;

    if (argc >= 2) {
        target = argv[1];
    }
    if (argc >= 3) {
        concurrency = std::stoi(argv[2]);
    }
    if (argc >= 4) {
        requestsPerThread = std::stoi(argv[3]);
    }

    std::vector<ThreadResult> results(concurrency);
    std::vector<std::thread> threads;

    std::atomic<int> ready{0};
    std::atomic<bool> start{false};

    auto benchStart = std::chrono::steady_clock::now();

    for (int t = 0; t < concurrency; ++t) {
        threads.emplace_back([&, t]() {
            novanet::rpc::sdk::ChannelOptions options;
            options.connectTimeoutSeconds = 3.0;
            options.defaultRpcTimeoutSeconds = 3.0;
            options.startHeartbeat = false;
            options.nodeId = "novanet-add-benchmark";

            std::string createError;
            auto channel =
                novanet::rpc::sdk::CreateChannel(target, options, &createError);

            if (!channel) {
                std::cerr << "CreateChannel failed: " << createError << "\n";
                results[t].failed += requestsPerThread;
                return;
            }

            auto connectStatus = channel->connect();
            if (!connectStatus.ok()) {
                std::cerr << "connect failed: " << connectStatus.toString() << "\n";
                results[t].failed += requestsPerThread;
                return;
            }

            novanet::rpc::sdk::CalculatorServiceStub stub(channel);

            ready.fetch_add(1);

            while (!start.load()) {
                std::this_thread::yield();
            }

            results[t].latenciesMs.reserve(requestsPerThread);

            for (int i = 0; i < requestsPerThread; ++i) {
                calculator::AddRequest req;
                calculator::AddResponse resp;

                req.set_lhs(1);
                req.set_rhs(2);

                novanet::rpc::sdk::ClientContext ctx;
                ctx.setTimeoutSeconds(3.0);

                auto begin = std::chrono::steady_clock::now();

                auto status = stub.Add(&ctx, req, &resp);

                auto end = std::chrono::steady_clock::now();

                double latencyMs =
                    std::chrono::duration<double, std::milli>(end - begin).count();

                if (status.ok() && resp.result() == 3) {
                    results[t].success++;
                    results[t].latenciesMs.push_back(latencyMs);
                } else {
                    std::string err = status.toString();

                    if (err.find("timeout") != std::string::npos ||
                        err.find("TIMEOUT") != std::string::npos) {
                        results[t].timeout++;
                    } else {
                        results[t].failed++;
                    }
                }
            }

            channel->shutdown();
        });
    }

    while (ready.load() < concurrency) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    benchStart = std::chrono::steady_clock::now();
    start.store(true);

    for (auto& th : threads) {
        th.join();
    }

    auto benchEnd = std::chrono::steady_clock::now();

    double totalSeconds =
        std::chrono::duration<double>(benchEnd - benchStart).count();

    int totalRequests = concurrency * requestsPerThread;
    int success = 0;
    int failed = 0;
    int timeout = 0;

    std::vector<double> allLatencies;

    for (auto& r : results) {
        success += r.success;
        failed += r.failed;
        timeout += r.timeout;

        allLatencies.insert(allLatencies.end(), r.latenciesMs.begin(),
                            r.latenciesMs.end());
    }

    double qps = success / totalSeconds;

    double sum = 0.0;
    for (double v : allLatencies) {
        sum += v;
    }

    double avg = allLatencies.empty() ? 0.0 : sum / allLatencies.size();
    double p95 = percentile(allLatencies, 95.0);
    double p99 = percentile(allLatencies, 99.0);

    double successRate = totalRequests == 0 ? 0.0 : 100.0 * success / totalRequests;
    double timeoutRate = totalRequests == 0 ? 0.0 : 100.0 * timeout / totalRequests;

    std::cout << "target=" << target << "\n";
    std::cout << "concurrency=" << concurrency << "\n";
    std::cout << "requests_per_thread=" << requestsPerThread << "\n";
    std::cout << "total_requests=" << totalRequests << "\n";
    std::cout << "success=" << success << "\n";
    std::cout << "failed=" << failed << "\n";
    std::cout << "timeout=" << timeout << "\n";
    std::cout << "duration_sec=" << totalSeconds << "\n";
    std::cout << "qps=" << qps << "\n";
    std::cout << "success_rate=" << successRate << "%\n";
    std::cout << "timeout_rate=" << timeoutRate << "%\n";
    std::cout << "avg_latency_ms=" << avg << "\n";
    std::cout << "p95_latency_ms=" << p95 << "\n";
    std::cout << "p99_latency_ms=" << p99 << "\n";

    return failed == 0 && timeout == 0 ? 0 : 1;
}