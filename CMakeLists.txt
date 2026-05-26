cmake_minimum_required(VERSION 3.16)

project(NovaNet LANGUAGES CXX)

# ==========================================================
# C++ 标准
# ==========================================================

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 给 clangd 使用
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 如果用户没有手动指定构建类型，默认使用 Debug
# 不要强行写死 Release，否则调试 / ASAN 很难用
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ==========================================================
# 编译选项
# ==========================================================

add_compile_options(-Wall -Wextra -Wpedantic)

option(NOVANET_ENABLE_ASAN "Enable AddressSanitizer" OFF)

if (NOVANET_ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

# ==========================================================
# 依赖
# ==========================================================

find_package(Threads REQUIRED)
find_package(Protobuf REQUIRED)

# ==========================================================
# 核心库：libnovanet.a
# ==========================================================

add_library(novanet STATIC
    # ======================================================
    # generated protobuf
    # ======================================================
    generated/proto/rpc_meta.pb.cc
    generated/proto/calculator.pb.cc
    # ======================================================
    # base
    # ======================================================
    src/base/Timestamp.cpp

    # 如果你已经实现了这些文件，再打开
    # src/base/Logger.cpp
    # src/base/ErrorCode.cpp

    # ======================================================
    # net
    # ======================================================
    src/net/InetAddress.cpp
    src/net/Socket.cpp
    src/net/SocketsOps.cpp

    src/net/Channel.cpp
    src/net/Poller.cpp
    src/net/EventLoop.cpp
    src/net/Acceptor.cpp

    src/net/Timer.cpp
    src/net/TimerQueue.cpp

    src/net/EventLoopThread.cpp
    src/net/EventLoopThreadPool.cpp
    src/net/TcpConnection.cpp
    src/net/TcpServer.cpp

    # ======================================================
    # rpc/protocol
    # ======================================================
    src/rpc/protocol/FrameType.cpp
    src/rpc/protocol/RpcHeader.cpp
    src/rpc/protocol/RpcMessage.cpp
    src/rpc/protocol/RpcCodec.cpp

    # ======================================================
    # rpc/core
    # 后面写到这些类时再打开
    # ======================================================
    src/rpc/core/RpcController.cpp
    src/rpc/core/ServiceRegistry.cpp
    src/rpc/core/MethodInvoker.cpp
    src/rpc/core/RpcDispatcher.cpp
    src/rpc/core/PendingCall.cpp
    # src/rpc/core/PendingCallManager.cpp
    src/rpc/core/RpcServer.cpp
    # src/rpc/core/RpcChannel.cpp

    # ======================================================
    # rpc/stream
    # 后面写到这些类时再打开
    # ======================================================
    # src/rpc/stream/StreamFrame.cpp
    # src/rpc/stream/StreamSession.cpp
    # src/rpc/stream/StreamManager.cpp
)

target_include_directories(novanet
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/generated/proto
)

target_link_libraries(novanet
    PUBLIC
        Threads::Threads
        protobuf::libprotobuf
)

# ==========================================================
# Phase 1 demo
# ==========================================================

add_executable(phase1_epoll_echo
    examples/phase1/epoll_echo_server.cpp
)

target_link_libraries(phase1_epoll_echo
    PRIVATE
        novanet
)

# ==========================================================
# Phase 2 demo
# ==========================================================

add_executable(phase2_reactor_echo
    examples/phase2/reactor_echo_server.cpp
)

target_link_libraries(phase2_reactor_echo
    PRIVATE
        novanet
)

# ==========================================================
# Phase 3 demo
# ==========================================================

add_executable(multireactor_echo_server
    examples/phase3/multireactor_echo_server.cpp
)

target_link_libraries(multireactor_echo_server
    PRIVATE
        novanet
)


# ==========================================================
# Tests
# ==========================================================

enable_testing()

add_executable(rpc_meta_smoke_test
    tests/rpc/RpcMetaSmokeTest.cpp
)

target_link_libraries(rpc_meta_smoke_test
    PRIVATE
        novanet
        protobuf::libprotobuf
)

add_test(
    NAME rpc_meta_smoke_test
    COMMAND rpc_meta_smoke_test
)

add_executable(rpc_codec_meta_test
    tests/rpc/RpcCodecMetaTest.cpp
)

target_link_libraries(rpc_codec_meta_test
    PRIVATE
        novanet
)

add_test(
    NAME rpc_codec_meta_test
    COMMAND rpc_codec_meta_test
)


add_executable(calculator_proto_smoke_test
    tests/rpc/CalculatorProtoSmokeTest.cpp
)

target_link_libraries(calculator_proto_smoke_test
    PRIVATE
        novanet
)

add_test(
    NAME calculator_proto_smoke_test
    COMMAND calculator_proto_smoke_test
)

add_executable(method_invoker_test
    tests/rpc/MethodInvokerTest.cpp
)

target_link_libraries(method_invoker_test
    PRIVATE
        novanet
)

add_test(
    NAME method_invoker_test
    COMMAND method_invoker_test
)


add_executable(rpc_dispatcher_test
    tests/rpc/RpcDispatcherTest.cpp
)

target_link_libraries(rpc_dispatcher_test
    PRIVATE
        novanet
)

add_test(
    NAME rpc_dispatcher_test
    COMMAND rpc_dispatcher_test
)


add_executable(rpc_dispatcher_unary_path_test
    tests/rpc/RpcDispatcherUnaryPathTest.cpp
)

target_link_libraries(rpc_dispatcher_unary_path_test
    PRIVATE
        novanet
)

add_test(
    NAME rpc_dispatcher_unary_path_test
    COMMAND rpc_dispatcher_unary_path_test
)


add_executable(rpc_server_integration_test
    tests/rpc/RpcServerIntegrationTest.cpp
)

target_link_libraries(rpc_server_integration_test
    PRIVATE
        novanet
        Threads::Threads
)

add_test(
    NAME rpc_server_integration_test
    COMMAND rpc_server_integration_test
)


add_executable(pending_calltest
    tests/rpc/PendingCallTest.cpp
)

target_link_libraries(pending_calltest
    PRIVATE
        novanet
        Threads::Threads
)

add_test(
    NAME pending_calltest
    COMMAND pending_calltest
)