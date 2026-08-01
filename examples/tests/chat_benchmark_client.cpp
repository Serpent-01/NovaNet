#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "chat.pb.h"
#include "novanet/base/Logger.h"
#include "novanet/rpc/core/RpcStatus.h"
#include "novanet/rpc/sdk/ChannelOptions.h"
#include "novanet/rpc/sdk/ChatServiceStub.h"
#include "novanet/rpc/sdk/ClientContext.h"
#include "novanet/rpc/sdk/CreateChannel.h"

namespace chat = novanet::ai::chat;

namespace {

struct ThreadResult {
    std::int64_t success{0};
    std::int64_t failed{0};
    std::int64_t timeout{0};
    std::int64_t responseChunks{0};
    std::int64_t responseBytes{0};
    std::vector<double> latenciesMs;
    std::string firstError;
};

[[nodiscard]] double percentile(const std::vector<double>& sortedValues,
                                double p) {
    if (sortedValues.empty()) {
        return 0.0;
    }

    const double position =
        (p / 100.0) * static_cast<double>(sortedValues.size() - 1);
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const auto upper = static_cast<std::size_t>(std::ceil(position));

    if (lower == upper) {
        return sortedValues[lower];
    }

    const double fraction = position - static_cast<double>(lower);
    return sortedValues[lower] +
           (sortedValues[upper] - sortedValues[lower]) * fraction;
}

void recordFailure(ThreadResult& result,
                   const novanet::rpc::RpcStatus& status) {
    if (status.errorCode() == novanet::rpc::meta::RPC_TIMEOUT) {
        ++result.timeout;
    } else {
        ++result.failed;
    }

    if (result.firstError.empty()) {
        result.firstError = status.toString();
    }
}

void printUsage(const char* program) {
    std::cerr << "Usage: " << program
              << " [target] [concurrency] [requests_per_thread]"
                 " [timeout_seconds]\n"
              << "Example: " << program
              << " 127.0.0.1:19090 16 100 10\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    novanet::base::Logger::setLogLevel(novanet::base::LogLevel::Warn);

    std::string target = "127.0.0.1:19090";
    int concurrency = 1;
    int requestsPerThread = 100;
    int timeoutSeconds = 10;

    try {
        if (argc >= 2) {
            target = argv[1];
        }
        if (argc >= 3) {
            concurrency = std::stoi(argv[2]);
        }
        if (argc >= 4) {
            requestsPerThread = std::stoi(argv[3]);
        }
        if (argc >= 5) {
            timeoutSeconds = std::stoi(argv[4]);
        }
    } catch (const std::exception& ex) {
        std::cerr << "Invalid argument: " << ex.what() << "\n";
        printUsage(argv[0]);
        return 2;
    }

    if (argc > 5 || target.empty() || concurrency <= 0 ||
        requestsPerThread <= 0 || timeoutSeconds <= 0) {
        printUsage(argv[0]);
        return 2;
    }

    std::vector<ThreadResult> results(static_cast<std::size_t>(concurrency));
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(concurrency));

    std::atomic<int> ready{0};
    std::atomic<bool> start{false};

    for (int threadIndex = 0; threadIndex < concurrency; ++threadIndex) {
        threads.emplace_back([&, threadIndex]() {
            ThreadResult& result =
                results[static_cast<std::size_t>(threadIndex)];

            novanet::rpc::sdk::ChannelOptions options;
            options.connectTimeoutSeconds = 3.0;
            options.defaultRpcTimeoutSeconds =
                static_cast<double>(timeoutSeconds);
            options.startHeartbeat = false;
            options.nodeId = "novanet-chat-benchmark";

            std::string createError;
            auto channel =
                novanet::rpc::sdk::CreateChannel(target, options, &createError);

            if (!channel) {
                result.failed = requestsPerThread;
                result.firstError = "CreateChannel failed: " + createError;
                ready.fetch_add(1);
                return;
            }

            const auto connectStatus = channel->connect();
            if (!connectStatus.ok()) {
                result.failed = requestsPerThread;
                result.firstError =
                    "connect failed: " + connectStatus.toString();
                ready.fetch_add(1);
                return;
            }

            novanet::rpc::sdk::ChatServiceStub stub(channel);

            result.latenciesMs.reserve(
                static_cast<std::size_t>(requestsPerThread));

            ready.fetch_add(1);
            while (!start.load()) {
                std::this_thread::yield();
            }

            for (int requestIndex = 0;
                 requestIndex < requestsPerThread; ++requestIndex) {
                chat::GenerateRequest request;
                request.set_model("fake-llm");
                request.set_max_tokens(256);
                request.set_temperature(0.0F);

                auto* message = request.add_messages();
                message->set_role("user");
                message->set_content(
                    "请简要说明 Reactor 网络模型的工作原理和关键组件。");

                novanet::rpc::sdk::ClientContext context;
                context.setTimeoutSeconds(
                    static_cast<double>(timeoutSeconds));

                const auto begin = std::chrono::steady_clock::now();
                auto reader = stub.Generate(&context, request);

                std::uint32_t expectedIndex = 0;
                std::int64_t chunks = 0;
                std::int64_t bytes = 0;
                bool indexValid = true;
                std::string finishReason;

                chat::GenerateChunk chunk;
                while (reader->Read(&chunk)) {
                    if (chunk.index() != expectedIndex) {
                        indexValid = false;
                    }

                    ++expectedIndex;
                    ++chunks;
                    bytes +=
                        static_cast<std::int64_t>(chunk.delta().size());

                    if (!chunk.finish_reason().empty()) {
                        finishReason = chunk.finish_reason();
                    }
                }

                const auto status = reader->Finish();
                const auto end = std::chrono::steady_clock::now();
                const double latencyMs =
                    std::chrono::duration<double, std::milli>(end - begin)
                        .count();

                const bool responseValid =
                    chunks > 0 && bytes > 0 && indexValid &&
                    finishReason == "stop";

                if (status.ok() && responseValid) {
                    ++result.success;
                    result.responseChunks += chunks;
                    result.responseBytes += bytes;
                    result.latenciesMs.push_back(latencyMs);
                    continue;
                }

                if (!status.ok()) {
                    recordFailure(result, status);
                } else {
                    ++result.failed;
                    if (result.firstError.empty()) {
                        result.firstError =
                            "invalid stream response: chunks=" +
                            std::to_string(chunks) +
                            ", bytes=" + std::to_string(bytes) +
                            ", index_valid=" +
                            std::string(indexValid ? "true" : "false") +
                            ", finish_reason=" + finishReason;
                    }
                }
            }

            channel->shutdown();
        });
    }

    while (ready.load() < concurrency) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const auto benchmarkStart = std::chrono::steady_clock::now();
    start.store(true);

    for (auto& thread : threads) {
        thread.join();
    }

    const auto benchmarkEnd = std::chrono::steady_clock::now();
    const double totalSeconds =
        std::chrono::duration<double>(benchmarkEnd - benchmarkStart).count();

    const std::int64_t totalRequests =
        static_cast<std::int64_t>(concurrency) * requestsPerThread;
    std::int64_t success = 0;
    std::int64_t failed = 0;
    std::int64_t timeout = 0;
    std::int64_t responseChunks = 0;
    std::int64_t responseBytes = 0;
    std::vector<double> allLatencies;

    allLatencies.reserve(static_cast<std::size_t>(totalRequests));

    for (const auto& result : results) {
        success += result.success;
        failed += result.failed;
        timeout += result.timeout;
        responseChunks += result.responseChunks;
        responseBytes += result.responseBytes;
        allLatencies.insert(allLatencies.end(), result.latenciesMs.begin(),
                            result.latenciesMs.end());
    }

    std::sort(allLatencies.begin(), allLatencies.end());

    double latencySum = 0.0;
    for (const double latency : allLatencies) {
        latencySum += latency;
    }

    const double qps =
        totalSeconds > 0.0 ? static_cast<double>(success) / totalSeconds : 0.0;
    const double successRate =
        totalRequests > 0
            ? 100.0 * static_cast<double>(success) /
                  static_cast<double>(totalRequests)
            : 0.0;
    const double timeoutRate =
        totalRequests > 0
            ? 100.0 * static_cast<double>(timeout) /
                  static_cast<double>(totalRequests)
            : 0.0;
    const double averageLatency =
        allLatencies.empty()
            ? 0.0
            : latencySum / static_cast<double>(allLatencies.size());
    const double averageChunks =
        success > 0 ? static_cast<double>(responseChunks) / success : 0.0;
    const double averageBytes =
        success > 0 ? static_cast<double>(responseBytes) / success : 0.0;

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "target=" << target << "\n";
    std::cout << "provider=FakeAiProvider\n";
    std::cout << "concurrency=" << concurrency << "\n";
    std::cout << "requests_per_thread=" << requestsPerThread << "\n";
    std::cout << "timeout_seconds=" << timeoutSeconds << "\n";
    std::cout << "total_requests=" << totalRequests << "\n";
    std::cout << "success=" << success << "\n";
    std::cout << "failed=" << failed << "\n";
    std::cout << "timeout=" << timeout << "\n";
    std::cout << "duration_sec=" << totalSeconds << "\n";
    std::cout << "qps=" << qps << "\n";
    std::cout << "success_rate=" << successRate << "%\n";
    std::cout << "timeout_rate=" << timeoutRate << "%\n";
    std::cout << "avg_latency_ms=" << averageLatency << "\n";
    std::cout << "p50_latency_ms=" << percentile(allLatencies, 50.0) << "\n";
    std::cout << "p95_latency_ms=" << percentile(allLatencies, 95.0) << "\n";
    std::cout << "p99_latency_ms=" << percentile(allLatencies, 99.0) << "\n";
    std::cout << "avg_response_chunks=" << averageChunks << "\n";
    std::cout << "avg_response_bytes=" << averageBytes << "\n";

    for (std::size_t i = 0; i < results.size(); ++i) {
        if (!results[i].firstError.empty()) {
            std::cout << "thread_" << i
                      << "_first_error=" << results[i].firstError << "\n";
        }
    }

    return failed == 0 && timeout == 0 ? 0 : 1;
}
