#pragma once

#include <map>
#include <memory>

#include "common.hpp"
#include "util/locks.hpp"

constexpr int EPOLL_EVENT_OOM = 1;

class TContainer;
class TEpollLoop;

class TEpollSource : public TNonCopyable {
public:
    int Fd;
    int Flags;
    std::weak_ptr<TContainer> Container;

    TEpollSource(int fd, int flags, std::weak_ptr<TContainer> container) :
        Fd(fd), Flags(flags), Container(container) {}
    TEpollSource(int fd) : Fd(fd), Flags(0), Container() {}
    TEpollSource() : Fd(-1), Flags(0), Container() {}
};

class TEpollLoop : public TLockable, public TNonCopyable {
    int EpollFd = -1;

    size_t MaxEvents = 0;
    struct epoll_event *Events = nullptr;

    std::vector<std::weak_ptr<TEpollSource>> Sources;

    TError ModifySourceEvents(int fd, uint32_t events) const;

public:
    TError Create();
    void Destroy();
    ~TEpollLoop();

    TError AddSource(std::shared_ptr<TEpollSource> source);
    void RemoveSource(int fd);
    std::shared_ptr<TEpollSource> GetSource(int fd);

    TError StartInput(int fd) const;
    TError StopInput(int fd) const;
    TError StartOutput(int fd) const;

    TError GetEvents(std::vector<struct epoll_event> &evts, int timeout);
};
