#pragma once
#include <vector>
#include <unordered_map>
#include <sys/epoll.h>


namespace novanet::net{

class Channel;
class EventLoop;

class Poller{
public:
    using ChannelList = std::vector<Channel*>;
    explicit Poller(EventLoop* loop);
    ~Poller();

    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    void poll(int timeoutMs,ChannelList* activeChannels);

    void updateChannel(Channel* channel);

    void removeChannel(Channel* channel);

    bool hasChannel(Channel* channel) const;
private:
    void update(int operation,Channel* channel);

    void fillActiveChannels(int numEvents,ChannelList* activeChannels) const;

    using ChannelMap = std::unordered_map<int,Channel*>;
    using EventList = std::vector<struct epoll_event>;

    static const int kInitEventListSize = 16;

    EventLoop* ownerLoop_;
    int epollfd_;
    EventList events_;
    ChannelMap channels_;
};

}


