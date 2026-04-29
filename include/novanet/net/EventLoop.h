#pragma once
#include <vector>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace novanet::net{

class Channel;
class Poller;


class EventLoop{
public:
    using Functor = std::function<void()>;

    EventLoop();
    ~EventLoop();


    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void loop();

    void quit();

    void runInLoop(Functor cb);


    void queueInLoop(Functor cb);

    void updateChannel(Channel* channel);

    void removeChannel(Channel* channel);

    bool hasChannel(Channel* channel);

    bool isInLoopThread() const;

    void assertInLoopThread() const;

private:
    
    void doPendingFunctors();

private:
    using ChannelList = std::vector<Channel*>;

    std::atomic<bool> looping_ {false};

    std::atomic<bool> quit_ {false};
    
    std::atomic<bool> callingPendingFunctors_{false};

    const std::thread::id threadId_ {std::this_thread::get_id()};

    std::unique_ptr<Poller> poller_;

    ChannelList activeChannels_;

    Channel* currentActiveChannel_{nullptr};
    
    std::mutex mutex_;

    std::vector<Functor> pendingFunctors_;
};

}