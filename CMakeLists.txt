cmake_minimum_required(VERSION 3.16)

# 项目名字 NovaNet 语言c++
project(NovaNet LANGUAGES CXX)
# 用 C++17
set(CMAKE_CXX_STANDARD 17)
# 不允许偷偷降级
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 不用 GNU 那些乱七八糟的扩展，尽量标准一点
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)


# 打开更多警告
add_compile_options(-Wall -Wextra -Wpedantic)
# 1. 先定义库，并把你的 .cpp 文件全部塞进去
# 注意：一定要把 src 下的那些文件写出来，否则库是空的
add_library(novanet 
    src/net/InetAddress.cpp
    src/net/Socket.cpp
    src/net/SocketsOps.cpp
)

# 让编译器知道头文件在 include/ 下面
target_include_directories(novanet
    PUBLIC ${PROJECT_SOURCE_DIR}/include
)
# 告诉编译器，头文件去 include 目录下找
include_directories(include)


# 1. 设置构建类型（放在 add_executable 之前之后都可以，全局生效）
set(CMAKE_BUILD_TYPE RelWithDebInfo)

# 2. 先生成可执行程序，名字叫 phase1_epoll_echo
# 注意：一定要先 add_executable！

add_compile_options(-fsanitize=address)
link_libraries(asan)

add_executable(phase1_epoll_echo examples/phase1/epoll_echo_server.cpp)

# 3. 现在针对这个已经存在的目标，添加栈帧指针参数
# 名字必须和 add_executable 里的完全一致
target_compile_options(phase1_epoll_echo PRIVATE -fno-omit-frame-pointer)

# 4. 链接库
target_link_libraries(phase1_epoll_echo novanet)
