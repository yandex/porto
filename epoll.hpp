#pragma once

#include <map>
#include <mutex>
#include <memory>

#include "common.hpp"
#include "util/signal.hpp"

extern "C" {
#include <sys/epoll.h>
}

constexpr int updateSignal = SIGHUP;
constexpr int rotateSignal = SIGUSR1;
constexpr int debugSignal = SIGUSR2;

static constexpr int HANDLE_SIGNALS[] = {SIGINT, SIGTERM,
                                         updateSignal, rotateSignal,
                                         debugSignal, SIGALRM};
static constexpr int HANDLE_SIGNALS_WAIT[] = {SIGCHLD};


constexpr int EPOLL_EVENT_OOM = 1;

class TContainer;

struct TEpollSource : public TNonCopyable {
    int Fd;
    int Flags;
    std::weak_ptr<TContainer> Container;

    TEpollSource(int fd, int flags, std::weak_ptr<TContainer> container) : Fd(fd), Flags(flags), Container(container) {}
    TEpollSource(int fd) : Fd(fd), Flags(0), Container() {}
};

class TEpollLoop : public TNonCopyable {
    TError InitializeSignals();
    bool GetSignals(std::vector<int> &signals);

    int EpollFd;

    size_t MaxEvents = 0;
    struct epoll_event *Events = nullptr;

    std::map<void *, std::shared_ptr<TEpollSource>> Sources;
    std::mutex Lock;

    TError RemoveFd(int fd);

public:
    TError Create();
    void Destroy();
    ~TEpollLoop();


    TError AddSource(std::shared_ptr<TEpollSource> source);
    void RemoveSource(std::shared_ptr<TEpollSource> source);
    std::shared_ptr<TEpollSource> GetSource(void *ptr);
    TError GetEvents(std::vector<int> &signals,
                     std::vector<struct epoll_event> &evts,
                     int timeout);
};
