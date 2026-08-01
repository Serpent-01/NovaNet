#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "calculator.pb.h"
#include "chat.pb.h"
#include "novanet/base/Logger.h"
#include "novanet/rpc/sdk/CalculatorServiceStub.h"
#include "novanet/rpc/sdk/ChannelOptions.h"
#include "novanet/rpc/sdk/ChatServiceStub.h"
#include "novanet/rpc/sdk/ClientContext.h"
#include "novanet/rpc/sdk/CreateChannel.h"

namespace calculator = novanet::example::calculator;
namespace chat = novanet::ai::chat;
using Clock = std::chrono::steady_clock;

namespace {

constexpr std::uint64_t kExpectedChunksPerStream = 10;

struct ThreadResult {
    std::uint64_t connectFailed{0};

    std::uint64_t workflowAttempted{0};
    std::uint64_t workflowSucceeded{0};
    std::uint64_t workflowFailed{0};
    std::uint64_t workflowTimeout{0};

    std::uint64_t addAttempted{0};
    std::uint64_t addSucceeded{0};
    std::uint64_t addFailed{0};
    std::uint64_t addTimeout{0};

    std::uint64_t chatAttempted{0};
    std::uint64_t chatSucceeded{0};
    std::uint64_t chatFailed{0};
    std::uint64_t chatTimeout{0};

    std::uint64_t readerCreateFailed{0};
    std::uint64_t finishFailed{0};
    std::uint64_t emptyStream{0};
    std::uint64_t missingFinalStop{0};
    std::uint64_t badChunkCount{0};
    std::uint64_t badChunkIndex{0};

    std::uint64_t chunksRead{0};
    std::uint64_t payloadBytesRead{0};

    std::vector<double> addLatencyMs;
    std::vector<double> firstChunkLatencyMs;
    std::vector<double> streamLatencyMs;
    std::vector<double> workflowLatencyMs;
};

struct LatencySummary {
    double average{0.0};
    double p50{0.0};
    double p95{0.0};
    double p99{0.0};
    double maximum{0.0};
};

double milliseconds(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

bool containsTimeout(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](char ch) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    });
    return text.find("timeout") != std::string::npos ||
           text.find("deadline") != std::string::npos;
}

double percentileOfSorted(const std::vector<double>& values, double p) {
    if (values.empty()) {
        return 0.0;
    }

    const double position = (p / 100.0) * static_cast<double>(values.size() - 1);
    const auto lower = static_cast<std::size_t>(position);
    const auto upper = std::min(lower + 1, values.size() - 1);
    const double fraction = position - static_cast<double>(lower);
    return values[lower] + (values[upper] - values[lower]) * fraction;
}

LatencySummary summarize(std::vector<double> values) {
    LatencySummary summary;
    if (values.empty()) {
        return summary;
    }

    std::sort(values.begin(), values.end());
    summary.average = std::accumulate(values.begin(), values.end(), 0.0) /
                      static_cast<double>(values.size());
    summary.p50 = percentileOfSorted(values, 50.0);
    summary.p95 = percentileOfSorted(values, 95.0);
    summary.p99 = percentileOfSorted(values, 99.0);
    summary.maximum = values.back();
    return summary;
}

void append(std::vector<double>& destination, const std::vector<double>& source) {
    destination.insert(destination.end(), source.begin(), source.end());
}

double percentage(std::uint64_t numerator, std::uint64_t denominator) {
    if (denominator == 0) {
        return 0.0;
    }
    return 100.0 * static_cast<double>(numerator) / static_cast<double>(denominator);
}

}  // namespace

int main(int argc, char* argv[]) {
    novanet::base::Logger::setLogLevel(novanet::base::LogLevel::Warn);

    const std::string target = argc >= 2 ? argv[1] : "127.0.0.1:19090";
    const int concurrency = argc >= 3 ? std::stoi(argv[2]) : 1;
    const int requestsPerThread = argc >= 4 ? std::stoi(argv[3]) : 1000;

    if (concurrency <= 0 || requestsPerThread <= 0) {
        std::cerr << "usage: " << argv[0]
                  << " [target] [concurrency] [requests_per_thread]\n";
        return 2;
    }

    std::vector<ThreadResult> results(static_cast<std::size_t>(concurrency));
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(concurrency));

    std::atomic<int> ready{0};
    std::atomic<bool> start{false};

    for (int threadIndex = 0; threadIndex < concurrency; ++threadIndex) {
        threads.emplace_back([&, threadIndex] {
            ThreadResult& result = results[static_cast<std::size_t>(threadIndex)];

            novanet::rpc::sdk::ChannelOptions options;
            options.connectTimeoutSeconds = 3.0;
            options.defaultRpcTimeoutSeconds = 10.0;
            options.startHeartbeat = false;
            options.streamIdleTimeoutSeconds = 30.0;
            options.streamTimeoutScanIntervalSeconds = 1.0;
            options.sendHighWaterMarkBytes = 8 * 1024 * 1024;
            options.nodeId = "novanet-add-chat-benchmark";

            std::string createError;
            auto channel =
                novanet::rpc::sdk::CreateChannel(target, options, &createError);

            bool connected = channel != nullptr;
            if (!connected) {
                std::cerr << "CreateChannel failed: " << createError << '\n';
            } else {
                const auto connectStatus = channel->connect();
                if (!connectStatus.ok()) {
                    std::cerr << "connect failed: " << connectStatus.toString()
                              << '\n';
                    connected = false;
                }
            }

            if (!connected) {
                result.connectFailed = 1;
            }

            // Even failed workers reach the start barrier, so main cannot hang.
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            if (!connected) {
                return;
            }

            novanet::rpc::sdk::CalculatorServiceStub calculatorStub(channel);
            novanet::rpc::sdk::ChatServiceStub chatStub(channel);

            result.addLatencyMs.reserve(static_cast<std::size_t>(requestsPerThread));
            result.firstChunkLatencyMs.reserve(
                static_cast<std::size_t>(requestsPerThread));
            result.streamLatencyMs.reserve(
                static_cast<std::size_t>(requestsPerThread));
            result.workflowLatencyMs.reserve(
                static_cast<std::size_t>(requestsPerThread));

            for (int iteration = 0; iteration < requestsPerThread; ++iteration) {
                ++result.workflowAttempted;
                const auto workflowBegin = Clock::now();

                bool addOk = false;
                bool addTimedOut = false;
                ++result.addAttempted;

                novanet::rpc::sdk::ClientContext addContext;
                addContext.setTimeoutSeconds(3.0);

                calculator::AddRequest addRequest;
                calculator::AddResponse addResponse;
                addRequest.set_lhs(1);
                addRequest.set_rhs(2);

                const auto addBegin = Clock::now();
                const auto addStatus =
                    calculatorStub.Add(&addContext, addRequest, &addResponse);
                const auto addEnd = Clock::now();

                if (addStatus.ok() && addResponse.result() == 3) {
                    addOk = true;
                    ++result.addSucceeded;
                    result.addLatencyMs.push_back(milliseconds(addEnd - addBegin));
                } else {
                    std::string error = addStatus.toString();
                    if (addContext.failed()) {
                        error += " " + addContext.errorText();
                    }
                    if (containsTimeout(error)) {
                        addTimedOut = true;
                        ++result.addTimeout;
                    } else {
                        ++result.addFailed;
                    }
                }

                bool chatOk = false;
                bool chatTimedOut = false;
                ++result.chatAttempted;

                novanet::rpc::sdk::ClientContext chatContext;
                chatContext.setTimeoutSeconds(10.0);

                chat::GenerateRequest chatRequest;
                chatRequest.set_model("fake-llm");
                chatRequest.set_max_tokens(512);
                chatRequest.set_temperature(0.0F);

                auto* userMessage = chatRequest.add_messages();
                userMessage->set_role("user");
                userMessage->set_content("请解释 Reactor 网络编程模型。");

                const auto streamBegin = Clock::now();
                auto reader = chatStub.Generate(&chatContext, chatRequest);

                if (!reader) {
                    ++result.readerCreateFailed;
                    std::string error;
                    if (chatContext.failed()) {
                        error = chatContext.errorText();
                    }
                    if (containsTimeout(error)) {
                        chatTimedOut = true;
                        ++result.chatTimeout;
                    } else {
                        ++result.chatFailed;
                    }
                } else {
                    chat::GenerateChunk chunk;
                    std::uint64_t streamChunks = 0;
                    std::uint64_t streamBytes = 0;
                    bool receivedFirstChunk = false;
                    bool finalChunkStopped = false;
                    bool chunkIndexOk = true;
                    double firstChunkMs = 0.0;

                    while (reader->Read(&chunk)) {
                        if (!receivedFirstChunk) {
                            firstChunkMs = milliseconds(Clock::now() - streamBegin);
                            receivedFirstChunk = true;
                        }

                        if (chunk.index() != streamChunks) {
                            chunkIndexOk = false;
                        }

                        ++streamChunks;
                        streamBytes +=
                            static_cast<std::uint64_t>(chunk.delta().size());
                        finalChunkStopped = chunk.finish_reason() == "stop";
                    }

                    const auto finishStatus = reader->Finish();
                    const auto streamEnd = Clock::now();

                    result.chunksRead += streamChunks;
                    result.payloadBytesRead += streamBytes;

                    bool streamSemanticsOk = true;
                    if (!receivedFirstChunk) {
                        ++result.emptyStream;
                        streamSemanticsOk = false;
                    }
                    if (!finalChunkStopped) {
                        ++result.missingFinalStop;
                        streamSemanticsOk = false;
                    }
                    if (streamChunks != kExpectedChunksPerStream) {
                        ++result.badChunkCount;
                        streamSemanticsOk = false;
                    }
                    if (!chunkIndexOk) {
                        ++result.badChunkIndex;
                        streamSemanticsOk = false;
                    }

                    if (!finishStatus.ok()) {
                        ++result.finishFailed;
                        std::string error = finishStatus.toString();
                        if (chatContext.failed()) {
                            error += " " + chatContext.errorText();
                        }
                        if (containsTimeout(error)) {
                            chatTimedOut = true;
                            ++result.chatTimeout;
                        } else {
                            ++result.chatFailed;
                        }
                    } else if (!streamSemanticsOk) {
                        ++result.chatFailed;
                    } else {
                        chatOk = true;
                        ++result.chatSucceeded;
                        result.firstChunkLatencyMs.push_back(firstChunkMs);
                        result.streamLatencyMs.push_back(
                            milliseconds(streamEnd - streamBegin));
                    }
                }

                const auto workflowEnd = Clock::now();
                if (addOk && chatOk) {
                    ++result.workflowSucceeded;
                    result.workflowLatencyMs.push_back(
                        milliseconds(workflowEnd - workflowBegin));
                } else if (addTimedOut || chatTimedOut) {
                    ++result.workflowTimeout;
                } else {
                    ++result.workflowFailed;
                }
            }

            channel->shutdown();
        });
    }

    while (ready.load(std::memory_order_acquire) < concurrency) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const auto benchmarkBegin = Clock::now();
    start.store(true, std::memory_order_release);

    for (auto& thread : threads) {
        thread.join();
    }
    const auto benchmarkEnd = Clock::now();

    ThreadResult total;
    for (const auto& result : results) {
        total.connectFailed += result.connectFailed;
        total.workflowAttempted += result.workflowAttempted;
        total.workflowSucceeded += result.workflowSucceeded;
        total.workflowFailed += result.workflowFailed;
        total.workflowTimeout += result.workflowTimeout;
        total.addAttempted += result.addAttempted;
        total.addSucceeded += result.addSucceeded;
        total.addFailed += result.addFailed;
        total.addTimeout += result.addTimeout;
        total.chatAttempted += result.chatAttempted;
        total.chatSucceeded += result.chatSucceeded;
        total.chatFailed += result.chatFailed;
        total.chatTimeout += result.chatTimeout;
        total.readerCreateFailed += result.readerCreateFailed;
        total.finishFailed += result.finishFailed;
        total.emptyStream += result.emptyStream;
        total.missingFinalStop += result.missingFinalStop;
        total.badChunkCount += result.badChunkCount;
        total.badChunkIndex += result.badChunkIndex;
        total.chunksRead += result.chunksRead;
        total.payloadBytesRead += result.payloadBytesRead;
        append(total.addLatencyMs, result.addLatencyMs);
        append(total.firstChunkLatencyMs, result.firstChunkLatencyMs);
        append(total.streamLatencyMs, result.streamLatencyMs);
        append(total.workflowLatencyMs, result.workflowLatencyMs);
    }

    const double durationSeconds =
        std::chrono::duration<double>(benchmarkEnd - benchmarkBegin).count();
    const std::uint64_t plannedWorkflows =
        static_cast<std::uint64_t>(concurrency) *
        static_cast<std::uint64_t>(requestsPerThread);
    const std::uint64_t rpcAttempted = total.addAttempted + total.chatAttempted;
    const std::uint64_t rpcSucceeded = total.addSucceeded + total.chatSucceeded;
    const std::uint64_t rpcFailed = total.addFailed + total.chatFailed;
    const std::uint64_t rpcTimeout = total.addTimeout + total.chatTimeout;

    const auto addLatency = summarize(std::move(total.addLatencyMs));
    const auto firstChunkLatency = summarize(std::move(total.firstChunkLatencyMs));
    const auto streamLatency = summarize(std::move(total.streamLatencyMs));
    const auto workflowLatency = summarize(std::move(total.workflowLatencyMs));

    std::cout << std::fixed << std::setprecision(6) << "target=" << target << '\n'
              << "concurrency=" << concurrency << '\n'
              << "requests_per_thread=" << requestsPerThread << '\n'
              << "planned_workflows=" << plannedWorkflows << '\n'
              << "duration_sec=" << durationSeconds << '\n'
              << "connect_failed=" << total.connectFailed << '\n'
              << "workflow_attempted=" << total.workflowAttempted << '\n'
              << "workflow_success=" << total.workflowSucceeded << '\n'
              << "workflow_failed=" << total.workflowFailed << '\n'
              << "workflow_timeout=" << total.workflowTimeout << '\n'
              << "workflow_success_rate="
              << percentage(total.workflowSucceeded, total.workflowAttempted)
              << "%\n"
              << "workflow_qps="
              << static_cast<double>(total.workflowSucceeded) / durationSeconds
              << '\n'
              << "rpc_attempted=" << rpcAttempted << '\n'
              << "rpc_success=" << rpcSucceeded << '\n'
              << "rpc_failed=" << rpcFailed << '\n'
              << "rpc_timeout=" << rpcTimeout << '\n'
              << "rpc_success_rate=" << percentage(rpcSucceeded, rpcAttempted)
              << "%\n"
              << "rpc_qps=" << static_cast<double>(rpcSucceeded) / durationSeconds
              << '\n'
              << "add_attempted=" << total.addAttempted << '\n'
              << "add_success=" << total.addSucceeded << '\n'
              << "add_failed=" << total.addFailed << '\n'
              << "add_timeout=" << total.addTimeout << '\n'
              << "add_success_rate="
              << percentage(total.addSucceeded, total.addAttempted) << "%\n"
              << "add_qps="
              << static_cast<double>(total.addSucceeded) / durationSeconds << '\n'
              << "chat_attempted=" << total.chatAttempted << '\n'
              << "chat_success=" << total.chatSucceeded << '\n'
              << "chat_failed=" << total.chatFailed << '\n'
              << "chat_timeout=" << total.chatTimeout << '\n'
              << "chat_success_rate="
              << percentage(total.chatSucceeded, total.chatAttempted) << "%\n"
              << "chat_streams_per_sec="
              << static_cast<double>(total.chatSucceeded) / durationSeconds << '\n'
              << "reader_create_failed=" << total.readerCreateFailed << '\n'
              << "finish_failed=" << total.finishFailed << '\n'
              << "empty_stream=" << total.emptyStream << '\n'
              << "missing_final_stop=" << total.missingFinalStop << '\n'
              << "bad_chunk_count=" << total.badChunkCount << '\n'
              << "bad_chunk_index=" << total.badChunkIndex << '\n'
              << "chunks_read=" << total.chunksRead << '\n'
              << "chunks_per_sec="
              << static_cast<double>(total.chunksRead) / durationSeconds << '\n'
              << "payload_bytes_read=" << total.payloadBytesRead << '\n'
              << "payload_mib_per_sec="
              << static_cast<double>(total.payloadBytesRead) /
                     (1024.0 * 1024.0 * durationSeconds)
              << '\n'
              << "add_avg_latency_ms=" << addLatency.average << '\n'
              << "add_p50_latency_ms=" << addLatency.p50 << '\n'
              << "add_p95_latency_ms=" << addLatency.p95 << '\n'
              << "add_p99_latency_ms=" << addLatency.p99 << '\n'
              << "add_max_latency_ms=" << addLatency.maximum << '\n'
              << "chat_first_chunk_avg_ms=" << firstChunkLatency.average << '\n'
              << "chat_first_chunk_p50_ms=" << firstChunkLatency.p50 << '\n'
              << "chat_first_chunk_p95_ms=" << firstChunkLatency.p95 << '\n'
              << "chat_first_chunk_p99_ms=" << firstChunkLatency.p99 << '\n'
              << "chat_first_chunk_max_ms=" << firstChunkLatency.maximum << '\n'
              << "chat_stream_avg_latency_ms=" << streamLatency.average << '\n'
              << "chat_stream_p50_latency_ms=" << streamLatency.p50 << '\n'
              << "chat_stream_p95_latency_ms=" << streamLatency.p95 << '\n'
              << "chat_stream_p99_latency_ms=" << streamLatency.p99 << '\n'
              << "chat_stream_max_latency_ms=" << streamLatency.maximum << '\n'
              << "workflow_avg_latency_ms=" << workflowLatency.average << '\n'
              << "workflow_p50_latency_ms=" << workflowLatency.p50 << '\n'
              << "workflow_p95_latency_ms=" << workflowLatency.p95 << '\n'
              << "workflow_p99_latency_ms=" << workflowLatency.p99 << '\n'
              << "workflow_max_latency_ms=" << workflowLatency.maximum << '\n';

    const bool passed = total.connectFailed == 0 &&
                        total.workflowAttempted == plannedWorkflows &&
                        total.workflowSucceeded == plannedWorkflows &&
                        total.workflowFailed == 0 && total.workflowTimeout == 0;

    std::cout << "result=" << (passed ? "PASSED" : "FAILED") << '\n';
    return passed ? 0 : 1;
}
