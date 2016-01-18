#pragma once

#include <map>
#include <memory>

#include "common.hpp"
#include "util/signal.hpp"
#include "util/locks.hpp"

constexpr int updateSignal = SIGHUP;
constexpr int rotateSignal = SIGUSR1;
constexpr int debugSignal = SIGUSR2;

static constexpr int HANDLE_SIGNALS[] = {SIGINT, SIGTERM,
                                         updateSignal, rotateSignal,
                                         debugSignal, SIGALRM, SIGPIPE};
static constexpr int HANDLE_SIGNALS_WAIT[] = {SIGCHLD};

constexpr int EPOLL_EVENT_OOM = 1;

class TContainer;
class TEpollLoop;

struct TEpollSource : public TNonCopyable {
    std::shared_ptr<TEpollLoop> EpollLoop;
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

    std::vector<std::weak_ptr<TEpollSource>> Sources;

    TError ModifySourceEvents(int fd, uint32_t events) const;

public:
    TError Create();
    void Destroy();
    ~TEpollLoop();

    TError AddSource(std::shared_ptr<TEpollSource> source);
    void RemoveSource(std::shared_ptr<TEpollSource> source);
    std::shared_ptr<TEpollSource> GetSource(int fd);

    TError StartInput(int fd) const;
    TError StopInput(int fd) const;
    TError StartOutput(int fd) const;

    TError GetEvents(std::vector<int> &signals,
                     std::vector<struct epoll_event> &evts,
                     int timeout);
};
