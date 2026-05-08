#pragma once

#include <vector>
#include <memory>
#include <string>

namespace novanet::net{
class EventLoop;
class EventLoopThread;

/// @brief 事件循环线程池
/// @note 运行在 Main Reactor 线程。负责创建和管理所有的 Sub Reactor 线程，
///       并为新接入的 TcpConnection 轮询分配独立的 EventLoop。
class EventLoopThreadPool{
public:
    explicit EventLoopThreadPool(EventLoop* baseLoop,std::string nameAge = "EventLoopThreadPool");
    ~EventLoopThreadPool();

    EventLoopThreadPool(const EventLoopThreadPool&) = delete;
    EventLoopThreadPool& operator=(const EventLoopThreadPool&) = delete;

    void setThreadNum(int numThreads){
        numThreads_ = numThreads;
    }

    void start();

    /// @brief 轮询 (Round-Robin) 获取下一个可用的 Sub Loop
    [[nodiscard]] EventLoop* getNextLoop();

    /// @brief 获取所有的 Sub Loops (常用于广播、Hash分发等策略)
    [[nodiscard]] std::vector<EventLoop*> getAllLoops() const;


    [[nodiscard]] bool started() const noexcept{return started_;}
    [[nodiscard]] const std::string& name() const noexcept{ return name_;}
private:
    EventLoop* baseLoop_{nullptr};
    std::string name_;

    bool started_{false};
    int numThreads_{0};
    size_t next_{0}; //轮询分配的索引，使用 size_t 避免警告

    // 线程生命周期管理，unique_ptr 保证析构时自动 join/清理
    std::vector<std::unique_ptr<EventLoopThread>> threads_{};
    
    //收集所有子线程里跑起来的 Loop指针
    std::vector<EventLoop*> loops_{};

};

} //namespace novanet::net