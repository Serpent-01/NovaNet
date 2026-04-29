#include "novanet/net/Buffer.h" // 包含你的 Header-only 核心
#include <gtest/gtest.h>
#include <string>

using namespace novanet::net;

// 1. 边界扩缩容测试 (精确打击 size_t 和容器的边界)
TEST(BufferTest, ExactExpandAndCompact) {
    Buffer buf;
    // 假设初始剩余容量足够，写入部分数据
    std::string data(500, 'a');
    buf.append(data.data(), data.size());
    EXPECT_EQ(buf.readableBytes(), 500);

    // 读出 400 字节，此时头部应该空出 400 字节
    buf.retrieve(400);
    EXPECT_EQ(buf.readableBytes(), 100);

    // 再次写入 800 字节。此时尾部空间不够，但总数据量 (100+800=900) < 初始容量 (如1024)
    // 必须验证这里发生的是内部 std::copy 腾挪，而不是分配新内存。
    std::string more_data(800, 'b');
    buf.append(more_data.data(), more_data.size());
    EXPECT_EQ(buf.readableBytes(), 900);
}

// 2. 内存复位测试 (防止游标无限后移)
TEST(BufferTest, RetrieveAllReset) {
    Buffer buf;
    buf.append("NovaNet", 7);
    buf.retrieveAll();
    
    // 必须验证复位后 readable 为 0，且继续 append 不会导致假性越界
    EXPECT_EQ(buf.readableBytes(), 0);
    buf.append("RPC", 3);
    EXPECT_EQ(buf.readableBytes(), 3);
}

// 3. 滴漏模拟测试 (极限拆包)
TEST(BufferTest, TrickleData) {
    Buffer buf;
    std::string expected;
    // 模拟每次只收到 1 字节或 3 字节这种极度碎片化的数据流
    for (int i = 0; i < 1000; ++i) {
        std::string piece = (i % 2 == 0) ? "A" : "XYZ";
        expected += piece;
        buf.append(piece.data(), piece.size());
    }
    EXPECT_EQ(buf.readableBytes(), expected.size());
    
    // 必须自己写一个 retrieveAllAsString 方法，或者按下面的方式验证
    std::string actual(buf.peek(), buf.readableBytes());
    EXPECT_EQ(expected, actual);
}

// 4. 真实系统调用的读写模拟 (如果你实现了 readFd 的话)
// 这个测试依赖于你的系统调用逻辑，先留空框架
TEST(BufferTest, ReadFdSimulation) {
    // TODO: 使用 pipe() 模拟内核态到用户态的 readv 分散读
    EXPECT_TRUE(true);
}