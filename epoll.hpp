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
class TEpollLoop;

struct TEpollSource : public TNonCopyable {
    std::weak_ptr<TEpollLoop> EpollLoop;
    int Fd;
    int Flags;
    std::weak_ptr<TContainer> Container;

    TEpollSource(std::shared_ptr<TEpollLoop> loop, int fd, int flags,
                 std::weak_ptr<TContainer> container) : EpollLoop(loop), Fd(fd),
                                                        Flags(flags),
                                                        Container(container) {}
    TEpollSource(std::shared_ptr<TEpollLoop> loop, int fd) : EpollLoop(loop),
                                                             Fd(fd), Flags(0),
                                                             Container() {}
};

class TEpollLoop : public TLockable, public TNonCopyable {
    TError InitializeSignals();
    bool GetSignals(std::vector<int> &signals);

    int EpollFd;

    size_t MaxEvents = 0;
    struct epoll_event *Events = nullptr;

    std::map<void *, std::weak_ptr<TEpollSource>> Sources;

    TError RemoveFd(int fd);
    TError ModifySourceEvents(std::shared_ptr<TEpollSource> source, bool in);

public:
    TError Create();
    void Destroy();
    ~TEpollLoop();

    TError AddSource(std::shared_ptr<TEpollSource> source);
    void RemoveSource(std::shared_ptr<TEpollSource> source);
    std::shared_ptr<TEpollSource> GetSource(void *ptr);
    TError EnableSource(std::shared_ptr<TEpollSource> source);
    TError DisableSource(std::shared_ptr<TEpollSource> source);
    TError GetEvents(std::vector<int> &signals,
                     std::vector<struct epoll_event> &evts,
                     int timeout);
};
