#include "novanet/rpc/core/FakeAiProvider.h"

#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace novanet::rpc {
namespace {

[[nodiscard]] std::string toLowerAscii(std::string text) {
    for (char& ch : text) {
        const auto value = static_cast<unsigned char>(ch);
        if (value < 0x80U) {
            ch = static_cast<char>(std::tolower(value));
        }
    }

    return text;
}

[[nodiscard]] bool containsAny(const std::string& text,
                               std::initializer_list<std::string_view> keywords) {
    for (const std::string_view keyword : keywords) {
        if (text.find(keyword) != std::string::npos) {
            return true;
        }
    }

    return false;
}

}  // namespace

AiProvider::Status FakeAiProvider::generateStreaming(
    const novanet::ai::chat::GenerateRequest& request, ChunkSink onChunk,
    StopChecker shouldStop) {
    if (!onChunk) {
        return Status::invalidRequest("ChunkSink is empty");
    }

    /*
     * Phase 4 不接真实模型，但请求仍然要做基本校验。
     * FakeAiProvider 只模拟业务生成，不负责 RPC 协议校验。
     */
    const std::string userText = extractLastUserMessage(request);
    if (userText.empty()) {
        return Status::invalidRequest("GenerateRequest has no user message");
    }

    /*
     * checkStop 用于统一处理 cancel / timeout / backpressure /
     * connection closed 等上层停止信号。
     *
     * shouldStop 返回非 OK，Provider 必须尽快停止生成。
     */
    auto checkStop = [&shouldStop]() -> Status {
        if (!shouldStop) {
            return Status::success();
        }

        Status status = shouldStop();
        if (!status.ok()) {
            return status;
        }

        return Status::success();
    };

    /*
     * emit 用于生成一个 GenerateChunk 并交给上层 ChunkSink。
     *
     * 注意：
     * - emit 前检查 shouldStop；
     * - onChunk 后再次由调用方继续下一轮；
     * - onChunk 返回非 OK，说明上层不希望继续生成。
     */
    auto emit = [&](std::uint32_t index, std::string delta,
                    std::string finishReason = "") -> Status {
        Status stopStatus = checkStop();
        if (!stopStatus.ok()) {
            return stopStatus;
        }

        novanet::ai::chat::GenerateChunk chunk;
        chunk.set_index(index);
        chunk.set_delta(std::move(delta));
        chunk.set_finish_reason(std::move(finishReason));

        Status sinkStatus = onChunk(chunk);
        if (!sinkStatus.ok()) {
            return sinkStatus;
        }

        return Status::success();
    };

    const std::vector<std::string> responseChunks = buildResponseChunks(userText);

    for (std::size_t i = 0; i < responseChunks.size(); ++i) {
        const bool isLast = i + 1 == responseChunks.size();
        Status status = emit(static_cast<std::uint32_t>(i), responseChunks[i],
                             isLast ? "stop" : "");
        if (!status.ok()) {
            return status;
        }
    }

    return Status::success();
}

std::string FakeAiProvider::extractLastUserMessage(
    const novanet::ai::chat::GenerateRequest& request) {
    /*
     * 优先查找最后一条 role == "user" 的消息。
     * 这是 Chat API 风格的基本语义。
     */
    for (int i = request.messages_size() - 1; i >= 0; --i) {
        const auto& message = request.messages(i);

        if (message.role() == "user" && !message.content().empty()) {
            return message.content();
        }
    }

    /*
     * 兜底：如果没有 role=user，但最后一条消息有内容，
     * 仍然允许 fake provider 根据该消息生成本地回答。
     *
     * 这样方便测试，但真实 Provider 可以更严格。
     */
    if (request.messages_size() > 0) {
        const auto& last = request.messages(request.messages_size() - 1);
        if (!last.content().empty()) {
            return last.content();
        }
    }

    return {};
}

std::vector<std::string> FakeAiProvider::buildResponseChunks(
    const std::string& userText) {
    const std::string searchableText = toLowerAscii(userText);

    if (containsAny(searchableText, {"reactor", "事件驱动", "事件循环"})) {
        return {
            "Reactor 是一种事件驱动的网络编程模式，用于高效处理大量并发连接。",
            "它通过 I/O "
            "多路复用统一监听连接、读写和关闭事件，再把就绪事件分发给对应的处理器。",
            "典型实现通常由 EventLoop、Poller、Channel "
            "和回调函数组成，并通过线程池扩展并发能力。",
            "这种模式可以减少阻塞等待和线程切换，特别适合网络服务、网关以及 RPC "
            "框架。",
            "实际实现时还需要正确处理非阻塞读写、缓冲区、背压、超时和连接生命周期。",
            "RPC 是远程过程调用，它让客户端能够像调用本地方法一样请求远程服务。",
            "一次完整调用通常包括服务定位、请求序列化、网络传输、服务端分发、业务执"
            "行和响应反序列化。",
            "请求 ID 用于匹配响应，流 ID 用于管理同一连接上的多个流式会话。",
            "工程实现还要处理超时、取消、心跳、背压、连接关闭以及重复请求等边界情况"
            "。",
            "良好的 RPC 框架会把网络细节封装在 Channel 和 Stub "
            "之后，让业务代码只关注服务与方法。",
        };
    }

    if (containsAny(searchableText, {"rpc", "远程调用", "远程过程调用"})) {
        return {
            "RPC 是远程过程调用，它让客户端能够像调用本地方法一样请求远程服务。",
            "一次完整调用通常包括服务定位、请求序列化、网络传输、服务端分发、业务执"
            "行和响应反序列化。",
            "请求 ID 用于匹配响应，流 ID 用于管理同一连接上的多个流式会话。",
            "工程实现还要处理超时、取消、心跳、背压、连接关闭以及重复请求等边界情况"
            "。",
            "良好的 RPC 框架会把网络细节封装在 Channel 和 Stub "
            "之后，让业务代码只关注服务与方法。",
        };
    }

    if (containsAny(searchableText, {"epoll", "socket", "network", "网络", "i/o"})) {
        return {
            "高并发网络程序的核心是让少量线程高效管理大量连接。",
            "在 Linux 中，可以使用非阻塞 Socket 配合 epoll "
            "监听可读、可写和连接关闭事件。",
            "读取数据时要处理半包、粘包和 "
            "EAGAIN，发送数据时要处理部分写以及动态注册可写事件。",
            "跨线程任务通常通过 eventfd 唤醒事件循环，定时任务则可以使用 timerfd "
            "统一管理。",
            "稳定性主要取决于连接生命周期、缓冲区上限、错误关闭路径和线程间状态同步"
            "是否完整。",
        };
    }

    if (containsAny(searchableText, {"c++", "cpp", "cmake", "并发", "线程"})) {
        return {
            "C++ 工程应先明确对象所有权、线程归属以及资源释放顺序。",
            "可以使用 RAII "
            "和智能指针管理资源，但仍需避免循环引用、跨线程析构和回调悬空。",
            "并发代码中的共享状态应通过互斥量、原子变量或事件循环串行化访问。",
            "构建系统负责统一依赖、编译选项和目标关系，测试则应覆盖正常流程与异常生"
            "命周期。",
            "相比增加复杂抽象，清晰的接口边界和可验证的关闭流程通常更重要。",
        };
    }

    if (containsAny(searchableText,
                    {"稳定性", "压测", "压力测试", "benchmark", "测试"})) {
        return {
            "稳定性测试需要同时观察业务结果、延迟、资源占用和进程退出状态。",
            "建议逐步提高并发数和请求总量，并记录成功率、超时率、平均延迟、P95 与 "
            "P99 延迟。",
            "测试过程中还应覆盖主动取消、连接中断、慢消费者、背压和服务端超时等异常"
            "场景。",
            "每轮结束后检查线程、文件描述符和内存是否回到稳定水平，并确认没有崩溃或"
            "异常日志。",
            "固定输入、固定响应和可重复的执行条件有助于定位生命周期问题与性能回退。",
        };
    }

    if (containsAny(searchableText, {"你好", "您好", "hello"}) ||
        searchableText == "hi" || searchableText == "hi!" ||
        searchableText == "hi.") {
        return {
            "你好，我是 NovaNet 本地模拟 AI。",
            "我可以返回稳定、确定的流式内容，用于验证 RPC "
            "调用、分片传输、取消和背压流程。",
            "你可以继续输入网络编程、RPC、C++ 或稳定性测试相关的问题。",
        };
    }

    const std::string question = summarizeQuestion(userText);
    return {
        "我理解你的问题是：“" + question + "”。",
        "可以先明确目标、输入条件和期望结果，再把问题拆分为几个可以独立验证的部分。",
        "分析时应同时关注正常流程、边界条件、错误处理以及资源生命周期。",
        "如果涉及并发或网络通信，还要检查状态同步、超时、取消和连接关闭是否完整。",
        "最后通过小规模验证和重复测试确认结论，这样得到的结果会更稳定，也更容易维护"
        "。",
    };
}

std::string FakeAiProvider::summarizeQuestion(const std::string& userText) {
    constexpr std::size_t kMaxCodePoints = 48;

    std::string summary;
    summary.reserve(userText.size());

    std::size_t offset = 0;
    std::size_t codePoints = 0;
    bool previousWasSpace = false;

    while (offset < userText.size() && codePoints < kMaxCodePoints) {
        const auto first = static_cast<unsigned char>(userText[offset]);
        std::size_t width = 1;

        if ((first & 0xE0U) == 0xC0U) {
            width = 2;
        } else if ((first & 0xF0U) == 0xE0U) {
            width = 3;
        } else if ((first & 0xF8U) == 0xF0U) {
            width = 4;
        }

        if (offset + width > userText.size()) {
            width = 1;
        }

        if (width == 1 && std::isspace(first) != 0) {
            if (!summary.empty() && !previousWasSpace) {
                summary.push_back(' ');
                previousWasSpace = true;
            }
        } else {
            summary.append(userText, offset, width);
            previousWasSpace = false;
            ++codePoints;
        }

        offset += width;
    }

    if (!summary.empty() && summary.back() == ' ') {
        summary.pop_back();
    }

    if (offset < userText.size()) {
        summary += "...";
    }

    return summary;
}

bool FakeAiProvider::isValidRole(const std::string& role) noexcept {
    return role == "system" || role == "user" || role == "assistant" ||
           role == "tool";
}

}  // namespace novanet::rpc
