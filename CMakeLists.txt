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
option(NOVANET_BUILD_TESTS "Build NovaNet tests" ON)

if (NOVANET_ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()




# ==========================================================
# 依赖
# ==========================================================

find_package(Threads REQUIRED)
find_package(Protobuf REQUIRED)
find_package(CURL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(GTest REQUIRED)
enable_testing()
# ==========================================================
# Protobuf 自动生成
# ==========================================================

set(PROTO_SRC_DIR ${PROJECT_SOURCE_DIR}/proto)
set(PROTO_GEN_DIR ${PROJECT_SOURCE_DIR}/generated/proto)

file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

set(PROTO_FILES
    ${PROTO_SRC_DIR}/rpc_meta.proto
    ${PROTO_SRC_DIR}/calculator.proto
    ${PROTO_SRC_DIR}/chat.proto
)

set(PROTO_SRCS "")
set(PROTO_HDRS "")

foreach(PROTO_FILE ${PROTO_FILES})
    get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)

    set(PROTO_SRC ${PROTO_GEN_DIR}/${PROTO_NAME}.pb.cc)
    set(PROTO_HDR ${PROTO_GEN_DIR}/${PROTO_NAME}.pb.h)

    add_custom_command(
        OUTPUT
            ${PROTO_SRC}
            ${PROTO_HDR}
        COMMAND
            ${Protobuf_PROTOC_EXECUTABLE}
        ARGS
            --proto_path=${PROTO_SRC_DIR}
            --cpp_out=${PROTO_GEN_DIR}
            ${PROTO_FILE}
        DEPENDS
            ${PROTO_FILE}
        COMMENT
            "Generating ${PROTO_NAME}.pb.cc/.pb.h"
        VERBATIM
    )

    list(APPEND PROTO_SRCS ${PROTO_SRC})
    list(APPEND PROTO_HDRS ${PROTO_HDR})
endforeach()



# ==========================================================
# 核心库：libnovanet.a
# ==========================================================

add_library(novanet STATIC
   
    ${PROTO_SRCS}
    ${PROTO_HDRS}
    

    src/base/Timestamp.cpp

   
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
    src/net/TcpClient.cpp
    src/net/TcpServer.cpp

    
    src/rpc/protocol/FrameType.cpp
    src/rpc/protocol/RpcHeader.cpp
    src/rpc/protocol/RpcMessage.cpp
    src/rpc/protocol/RpcCodec.cpp


    src/rpc/core/RpcController.cpp
    src/rpc/core/ServiceRegistry.cpp
    src/rpc/core/MethodInvoker.cpp
    src/rpc/core/StreamMethodInvoker.cpp
    src/rpc/core/ChatGenerateStreamHandler.cpp
    src/rpc/core/RpcDispatcher.cpp
    src/rpc/core/AiExecutor.cpp
    src/rpc/core/FakeAiProvider.cpp
    src/rpc/core/GatewayAiProvider.cpp
    src/rpc/core/PendingCall.cpp
    src/rpc/core/PendingCallManager.cpp
    src/rpc/core/RpcClient.cpp
    src/rpc/core/RpcServer.cpp
    src/rpc/core/RpcChannel.cpp
    src/rpc/core/RpcServerStreamResponder.cpp

    # ======================================================
    # rpc/stream
    # ======================================================
    src/rpc/stream/StreamFrame.cpp
    src/rpc/stream/StreamSession.cpp
    src/rpc/stream/StreamManager.cpp
    # ======================================================
    # rpc/sdk
    # ======================================================
    src/rpc/sdk/Endpoint.cpp
    src/rpc/sdk/ClientContext.cpp
    src/rpc/sdk/ClientChannel.cpp
    src/rpc/sdk/CreateChannel.cpp
    src/rpc/sdk/CalculatorServiceStub.cpp
    src/rpc/sdk/ProtobufRpcChannelAdapter.cpp
    src/rpc/sdk/ClientReader.cpp
    src/rpc/sdk/ChatServiceStub.cpp
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
        CURL::libcurl
        nlohmann_json::nlohmann_json
)
# ==========================================================
# final main
# ==========================================================
add_executable(server_main
    examples/server_main.cpp
)

target_link_libraries(server_main
    novanet
    protobuf::libprotobuf
)

add_executable(client_main
    examples/client_main.cpp
)

target_link_libraries(client_main
    novanet
    protobuf::libprotobuf
)

add_executable(add_timeout_client
    examples/tests/add_timeout_client.cpp
)

target_link_libraries(add_timeout_client
    novanet
    protobuf::libprotobuf
)


add_executable(add_benchmark_client
    examples/tests/add_benchmark_client.cpp
)

target_link_libraries(add_benchmark_client
    novanet
    protobuf::libprotobuf
)



if (NOVANET_BUILD_TESTS)
    add_test(
        NAME client_demo_smoke_test
        COMMAND bash ${CMAKE_SOURCE_DIR}/tests/run_client_demo_smoke.sh
    )

    set_tests_properties(client_demo_smoke_test
        PROPERTIES
            TIMEOUT 20
            LABELS "integration;smoke"
    )
endif()

# # ==========================================================
# # Phase 1 demo
# # ==========================================================

# add_executable(phase1_epoll_echo
#     examples/phase1/epoll_echo_server.cpp
# )

# target_link_libraries(phase1_epoll_echo
#     PRIVATE
#         novanet
# )

# # ==========================================================
# # Phase 2 demo
# # ==========================================================

# add_executable(phase2_reactor_echo
#     examples/phase2/reactor_echo_server.cpp
# )

# target_link_libraries(phase2_reactor_echo
#     PRIVATE
#         novanet
# )

# # ==========================================================
# # Phase 3 demo
# # ==========================================================

# add_executable(multireactor_echo_server
#     examples/phase3/multireactor_echo_server.cpp
# )

# target_link_libraries(multireactor_echo_server
#     PRIVATE
#         novanet
# )

# # ==========================================================
# # Phase 4 RPC examples
# # ==========================================================

# add_executable(novanet_client
#     examples/phase4/novanet_client.cpp
# )

# target_link_libraries(novanet_client
#     PRIVATE
#         novanet
# )

# add_executable(novanet_server
#     examples/phase4/novanet_server.cpp
# )

# target_link_libraries(novanet_server
#     PRIVATE
#         novanet
# )


# add_executable(calculator_server
#     examples/phase4/calculator_server.cpp
# )

# target_link_libraries(calculator_server
#     PRIVATE
#         novanet
# )

# add_executable(calculator_client
#     examples/phase4/calculator_client.cpp
# )

# target_link_libraries(calculator_client
#     PRIVATE
#         novanet
# )

# add_executable(chat_server
#     examples/phase4/chat_server.cpp
# )

# target_link_libraries(chat_server
#     PRIVATE
#         novanet
# )

# add_executable(chat_client
#     examples/phase4/chat_client.cpp
# )

# target_link_libraries(chat_client
#     PRIVATE
#         novanet
# )

# add_executable(cancel_client
#     examples/phase4/cancel_client.cpp
# )

# target_link_libraries(cancel_client
#     PRIVATE
#         novanet
# )

# add_executable(backpressure_client
#     examples/phase4/backpressure_client.cpp
# )

# target_link_libraries(backpressure_client
#     PRIVATE
#         novanet
# )

# add_executable(novanet_server2
#     examples/phase4/novanet_server2.cpp
# )

# target_link_libraries(novanet_server2
#     PRIVATE
#         novanet
# )

# # ==========================================================
# # Tests
# # ==========================================================

# enable_testing()

# add_executable(rpc_meta_smoke_test
#     tests/rpc/RpcMetaSmokeTest.cpp
# )

# target_link_libraries(rpc_meta_smoke_test
#     PRIVATE
#         novanet
#         protobuf::libprotobuf
# )

# add_test(
#     NAME rpc_meta_smoke_test
#     COMMAND rpc_meta_smoke_test
# )

# add_executable(rpc_codec_meta_test
#     tests/rpc/RpcCodecMetaTest.cpp
# )

# target_link_libraries(rpc_codec_meta_test
#     PRIVATE
#         novanet
# )

# add_test(
#     NAME rpc_codec_meta_test
#     COMMAND rpc_codec_meta_test
# )


# add_executable(calculator_proto_smoke_test
#     tests/rpc/CalculatorProtoSmokeTest.cpp
# )

# target_link_libraries(calculator_proto_smoke_test
#     PRIVATE
#         novanet
# )

# add_test(
#     NAME calculator_proto_smoke_test
#     COMMAND calculator_proto_smoke_test
# )

# add_executable(method_invoker_test
#     tests/rpc/MethodInvokerTest.cpp
# )

# target_link_libraries(method_invoker_test
#     PRIVATE
#         novanet
# )

# add_test(
#     NAME method_invoker_test
#     COMMAND method_invoker_test
# )


# add_executable(rpc_dispatcher_test
#     tests/rpc/RpcDispatcherTest.cpp
# )

# target_link_libraries(rpc_dispatcher_test
#     PRIVATE
#         novanet
# )

# add_test(
#     NAME rpc_dispatcher_test
#     COMMAND rpc_dispatcher_test
# )


# add_executable(rpc_dispatcher_unary_path_test
#     tests/rpc/RpcDispatcherUnaryPathTest.cpp
# )

# target_link_libraries(rpc_dispatcher_unary_path_test
#     PRIVATE
#         novanet
# )

# add_test(
#     NAME rpc_dispatcher_unary_path_test
#     COMMAND rpc_dispatcher_unary_path_test
# )


# add_executable(rpc_server_integration_test
#     tests/rpc/RpcServerIntegrationTest.cpp
# )

# target_link_libraries(rpc_server_integration_test
#     PRIVATE
#         novanet
#         Threads::Threads
# )

# add_test(
#     NAME rpc_server_integration_test
#     COMMAND rpc_server_integration_test
# )


# add_executable(pending_calltest
#     tests/rpc/PendingCallTest.cpp
# )

# target_link_libraries(pending_calltest
#     PRIVATE
#         novanet
#         Threads::Threads
# )

# add_test(
#     NAME pending_calltest
#     COMMAND pending_calltest
# )


# add_executable(pending_call_managertest
#     tests/rpc/PendingCallManagerTest.cpp
# )
# target_link_libraries(pending_call_managertest
#     PRIVATE
#         novanet
#         Threads::Threads
# )
# add_test(
#     NAME pending_call_managertest
#     COMMAND pending_call_managertest
# )



# add_executable(StreamFrameTest
#     tests/rpc/stream/StreamFrameTest.cpp
# )
# target_link_libraries(StreamFrameTest
#     PRIVATE
#         novanet
# )
# add_test(NAME StreamFrameTest COMMAND StreamFrameTest)


# add_executable(StreamSessionTest
#     tests/rpc/stream/StreamSessionTest.cpp
# )

# target_link_libraries(StreamSessionTest
#     PRIVATE
#         novanet
# )

# add_test(
#     NAME StreamSessionTest
#     COMMAND StreamSessionTest
# )


# add_executable(stream_method_invoker_test
#     tests/rpc/StreamMethodInvokerTest.cpp
# )

# target_link_libraries(stream_method_invoker_test
#     PRIVATE
#         novanet
#         Threads::Threads
# )

# add_test(
#     NAME stream_method_invoker_test
#     COMMAND stream_method_invoker_test
# )



# add_executable(EndpointTest
#     tests/rpc/sdk/EndpointTest.cpp
# )

# target_link_libraries(EndpointTest
#     PRIVATE
#         novanet
#         Threads::Threads
# )

# add_test(
#     NAME EndpointTest
#     COMMAND EndpointTest
# )



# # ==========================================================
# # Unit tests
# # ==========================================================

# if(NOVANET_BUILD_TESTS)
#     add_executable(rpc_header_test
#         tests/unit/protocol/rpc_header_test.cpp
#     )
#     target_link_libraries(rpc_header_test
#         PRIVATE
#             novanet
#             GTest::gtest_main
#     )
#     add_test(NAME rpcheader_test COMMAND  rpc_header_test)
# endif()