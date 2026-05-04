cmake_minimum_required(VERSION 3.16)

# 项目名字 NovaNet 语言c++
project(NovaNet LANGUAGES CXX)

# 用 C++17
set(CMAKE_CXX_STANDARD 17)
# 不允许偷偷降级
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 不用 GNU 那些乱七八糟的扩展，尽量标准一点
set(CMAKE_CXX_EXTENSIONS OFF)

# 导出 compile_commands.json 给 clangd 补全用
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# # 1. 设置构建类型为 RelWithDebInfo (带调试符号的 Release，配合 ASAN 绝佳)

# 【核心修改点 1】：为了 Soak Test 找 Bug，必须切回 RelWithDebInfo
#set(CMAKE_BUILD_TYPE RelWithDebInfo)


# 把 RelWithDebInfo 改为 Release，榨干性能
set(CMAKE_BUILD_TYPE Release)

# 打开更多警告
add_compile_options(-Wall -Wextra -Wpedantic)

# 【核心修改点 2】：开启 ASAN 内存泄漏/越界检查神器
#add_compile_options(-fsanitize=address)
#add_link_options(-fsanitize=address) # 使用更现代的 add_link_options 代替 link_libraries(asan)


# 引入系统的多线程库 (EventLoop 中用到了 std::mutex 和 std::thread::id)
find_package(Threads REQUIRED)

# ==========================================================
# 核心网络库: libnovanet.a
# ==========================================================
add_library(novanet 
    # --- Phase 1 底层组件 ---
    src/net/InetAddress.cpp
    src/net/Socket.cpp
    src/net/SocketsOps.cpp
    
    # --- Phase 2 Reactor 核心组件 (在这里新增) ---
    src/net/Channel.cpp
    src/net/Poller.cpp
    src/net/EventLoop.cpp
    src/net/Acceptor.cpp
)

# 让编译器知道头文件在 include/ 下面 (现代 CMake 标准写法)
target_include_directories(novanet
    PUBLIC ${PROJECT_SOURCE_DIR}/include
)

# ==========================================================
# 可执行文件 1：Phase 1 的老 Demo
# ==========================================================
add_executable(phase1_epoll_echo examples/phase1/epoll_echo_server.cpp)
target_compile_options(phase1_epoll_echo PRIVATE -fno-omit-frame-pointer)
target_link_libraries(phase1_epoll_echo novanet Threads::Threads)

# ==========================================================
# 可执行文件 2：Phase 2 的单线程 Reactor Echo Server
# ==========================================================
add_executable(phase2_reactor_echo examples/phase2/reactor_echo_server.cpp)
# 同样加上栈帧指针参数，保证 ASAN 报错时能看到完整的函数调用栈
target_compile_options(phase2_reactor_echo PRIVATE -fno-omit-frame-pointer)
# 链接网络库和系统线程库
target_link_libraries(phase2_reactor_echo novanet Threads::Threads)