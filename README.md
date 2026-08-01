# NovaNet

> 基于 C++17、Linux epoll LT 与 Multi-Reactor 架构实现的 Streaming RPC 框架。

## 项目简介

NovaNet 是一个面向 Linux 平台的 C++17 网络通信与 RPC 框架。项目从非阻塞 Socket 和 epoll I/O 模型开始，逐步实现单线程 Reactor、Multi-Reactor 网络库以及支持 Unary RPC、Server Streaming、超时、心跳、取消和背压控制的 RPC 系统。

NovaNet 使用自定义二进制 RPC 协议承载 Protobuf 数据，并提供类似 gRPC 的客户端 SDK 使用方式：

```text
CreateChannel(target)
    -> ServiceStub(channel)
    -> stub.Method(...)
```

当前项目定位为单机 RPC 框架。服务发现、负载均衡、自动重试等分布式能力不属于当前实现范围。

## 项目标识

| 项目属性 | 说明 |
|---|---|
| 项目名称 | NovaNet |
| 当前阶段 | Phase 4 Streaming RPC |
| 编程语言 | C++17 |
| 构建系统 | CMake 3.16+ |
| 运行平台 | Linux |
| I/O 模型 | epoll LT + 非阻塞 Socket |
| 并发架构 | One Loop Per Thread / Multi-Reactor |
| 序列化 | Protocol Buffers |
| RPC 类型 | Unary RPC、Server Streaming RPC |
| AI 接入 | C++ GatewayAiProvider + Python AI Bridge |

## 核心特性

### 网络基础设施

- `Socket`、`SocketsOps` 和 `InetAddress` 网络封装
- 非阻塞 Socket 与 epoll LT 事件驱动
- `Channel`、`Poller`、`EventLoop` Reactor 抽象
- `eventfd` 跨线程任务唤醒
- `timerfd` 定时任务管理
- `EventLoopThread` 与 `EventLoopThreadPool`
- One Loop Per Thread 多 Reactor 模型
- `TcpConnection`、`TcpClient` 和 `TcpServer`
- 输入、输出 Buffer 管理
- 部分写与动态 `EPOLLOUT` 注册
- 高水位回调和发送背压控制
- `SIGPIPE`、`EINTR`、`EMFILE` 等网络边界处理
- Channel tie 生命周期保护

### RPC 协议

- 固定 20 字节网络序协议头
- 默认最大帧长度 16 MiB
- 支持半包、粘包和连续多帧解析
- 请求通过 `request_id` 关联
- 流通过 `stream_id` 关联
- 同一 TCP 连接支持多个逻辑 RPC 流

协议帧类型包括：

```text
UNARY_REQUEST       UNARY_RESPONSE
STREAM_OPEN         STREAM_DATA
STREAM_END          STREAM_CANCEL
HEARTBEAT_PING      HEARTBEAT_PONG
ERROR_FRAME
```

### RPC 服务端

- Protobuf Service 注册与方法查找
- `ServiceRegistry` 服务注册中心
- `MethodInvoker` Unary 方法调用
- `RpcDispatcher` 协议帧分发
- `RpcServer` 连接和请求生命周期管理
- `StreamSession` 与 `StreamManager`
- `StreamResponder` 流式响应
- Worker Pool 异步执行 AI 请求
- Stream 空闲超时和连接定时器清理
- 高水位和待发送消息数量限制

### 客户端 SDK

- `CreateChannel("127.0.0.1:19090")`
- `ChannelOptions` 连接和心跳配置
- `ClientContext` 超时、Metadata 和错误状态
- `CalculatorServiceStub` Unary 服务代理
- `ChatServiceStub` Streaming 服务代理
- `ClientReader<T>` 流式结果读取
- `ProtobufRpcChannelAdapter`
- Unary PendingCall 请求等待与响应匹配
- 显式 `connect()` 和 `shutdown()` 生命周期管理

### RPC 治理能力

- Unary 调用超时
- Stream 空闲超时
- Streaming 主动取消
- PING/PONG 心跳检测
- 发送高水位背压
- Stream 待发送消息数量限制
- 错误帧与状态码传递
- 客户端 Metadata 透传
- 请求 ID 和流 ID 跟踪

### 日志系统

- `TRACE`、`DEBUG`、`INFO`、`WARN`
- `ERROR`、`SYSERR`、`FATAL`、`SYSFATAL`
- 微秒级时间戳
- PID、线程 ID、文件名和行号
- 系统错误自动附加 `errno`
- 可替换日志输出和刷新回调
- 线程安全日志输出


### Unary 调用链

```text
CalculatorServiceStub::Add
    -> ClientChannel
    -> RpcClient / RpcChannel
    -> UNARY_REQUEST
    -> RpcServer / RpcDispatcher
    -> MethodInvoker
    -> CalculatorServiceImpl::Add
    -> UNARY_RESPONSE
    -> PendingCall
    -> Client Response
```

### Streaming 调用链

```text
ChatServiceStub::Generate
    -> ClientReader
    -> ClientChannel::openStream
    -> STREAM_OPEN
    -> RpcServer / RpcDispatcher
    -> AiExecutor
    -> GatewayAiProvider
    -> StreamResponder
    -> STREAM_DATA*
    -> STREAM_END
    -> ClientReader::Read / Finish
```

## 技术栈

| 层级 | 技术 |
|---|---|
| 系统编程 | C++17、POSIX、pthread |
| 网络 I/O | Socket、epoll LT、eventfd、timerfd |
| 并发模型 | Reactor、Multi-Reactor、线程池 |
| RPC 协议 | 自定义二进制帧协议 |
| 数据序列化 | Protocol Buffers |
| HTTP 客户端 | libcurl |
| JSON | nlohmann/json |
| AI Bridge | Python、FastAPI、HTTPX、NDJSON |
| 构建系统 | CMake |
| 测试依赖 | GoogleTest |

NovaNet 使用了 Protobuf 的 Service 和 `RpcChannel` 接口，但不使用 gRPC HTTP/2 协议，因此不能直接与标准 gRPC 客户端或服务端互通。

## 项目结构

```text
NovaNet/
├── include/novanet/
│   ├── base/               # 日志、时间等基础设施
│   ├── net/                # Reactor 与 TCP 网络层
│   └── rpc/
│       ├── protocol/       # RPC 协议和编解码
│       ├── core/           # RPC 客户端、服务端和分发
│       ├── stream/         # Streaming 状态管理
│       └── sdk/            # Channel、Context 和 Stub
├── src/                    # 对应实现
├── proto/                  # Protobuf 协议定义
├── generated/              # 已生成的 Protobuf 源码
├── examples/               # Phase 示例及最终客户端/服务端
├── ai_bridge/              # Python AI Bridge
├── tests/                  # 测试脚本
└── CMakeLists.txt
```

## 构建要求

- Linux
- 支持 C++17 的 GCC 或 Clang
- CMake 3.16+
- pthread
- Protobuf 与 `protoc`
- libcurl
- nlohmann/json
- GoogleTest

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
```

默认构建以下程序：

```text
server_main
client_main
add_timeout_client
add_benchmark_client
```

## 快速运行

启动 NovaNet 服务端：

```bash
./build/server_main 19090
```

运行 Calculator 压测客户端：

```bash
./build/add_benchmark_client 127.0.0.1:19090 1 1
```

完整的 `client_main` 会依次调用 `Calculator.Add` 和 `Chat.Generate`。其中 Chat 调用需要先启动 AI Bridge。

## AI Bridge

AI Bridge 默认监听：

```text
http://127.0.0.1:18080
```

提供以下接口：

```text
GET  /health
POST /chat/stream
```

启动方式：

```bash
cd ai_bridge
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt

export AI_BRIDGE_API_KEY="your-api-key"
export AI_BRIDGE_UPSTREAM_URL="https://api.example.com/v1/chat/completions"
export AI_BRIDGE_MODEL="your-model"

python app.py
```

随后运行完整客户端：

```bash
./build/client_main 127.0.0.1:19090
```

AI Bridge 将 OpenAI-compatible SSE 响应转换为 NDJSON，`GatewayAiProvider` 再将其转换为 NovaNet `STREAM_DATA` 和 `STREAM_END` 帧。

## 当前范围

NovaNet 当前已经覆盖：

- Linux 非阻塞网络库
- Multi-Reactor TCP 服务端与客户端
- 自定义 RPC 协议
- Unary RPC
- Server Streaming RPC
- SDK Channel、Stub、Context 和 Reader
- 心跳、超时、流式取消和背压
- 外部 AI Streaming 接入

当前没有实现：

- 服务发现与服务注册中心
- 客户端负载均衡
- 自动重试与故障转移
- TLS 和身份认证
- Client Streaming
- Bidirectional Streaming
- 分布式链路追踪
- 标准 gRPC 协议兼容

## 开发阶段

| 阶段 | 内容 |
|---|---|
| Phase 1 | 非阻塞 Socket、epoll LT、Echo I/O 引擎 |
| Phase 2 | Channel、Poller、EventLoop 单线程 Reactor |
| Phase 3 | Multi-Reactor、TcpConnection、TcpServer |
| Phase 4 | Unary、Streaming、SDK、心跳、超时、取消、背压 |
| Phase 5 | 服务发现、负载均衡与重试，暂未实现 |


## Benchmark

### Calculator.Add Unary RPC

在 Intel Core i5-9300H、本机 TCP loopback（回环网络）环境下，对 `Calculator.Add` Unary RPC 进行了 64 并发、约 40 分钟的稳定性测试。

#### 测试结果

| 指标 | 结果 |
|---|---:|
| 并发数 | 64 |
| 测试时长 | 约 40 分钟 |
| RPC 总数 | 1.28 亿次 |
| 成功率 | 100% |
| 持续吞吐 | 约 53.3K QPS |
| 平均延迟 | 1.19 ms |
| P99 延迟 | 1.86 ms |
| 服务端 RSS | 稳定在约 14.9 MB |
| 服务端文件描述符 | 20 → 84 → 20 |

测试期间未出现 RPC 失败、超时、进程崩溃或异常退出。64 个客户端连接建立后，服务端文件描述符由 20 增至 84；测试结束并断开连接后恢复至 20，未观察到文件描述符泄漏。服务端 RSS 在持续压测过程中保持稳定，未观察到持续增长。

#### perf 性能分析

`perf` 采样结果显示，主要 CPU 开销集中在：

- RPC 响应构造与发送路径；
- `TcpConnection` 数据发送流程；
- `write/send` 等系统调用；
- Linux TCP/IP 网络协议栈。

结果表明，在当前轻量级 Unary RPC 场景下，业务计算开销较低，整体性能主要受响应发送和本机 TCP 网络栈开销影响。

> 该结果为 Intel i5-9300H 单机 loopback 环境下的微基准测试，用于验证 NovaNet 基础 RPC 链路的吞吐、延迟和资源稳定性，不代表跨机器网络或真实生产业务环境的性能。
