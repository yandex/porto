#pragma once

#include "common.hpp"
#include "util/signal.hpp"

extern "C" {
#include <sys/epoll.h>
}

constexpr int updateSignal = SIGHUP;
constexpr int rotateSignal = SIGUSR1;

static constexpr int HANDLE_SIGNALS[] = {SIGINT, SIGTERM,
                                         updateSignal, rotateSignal,
                                         SIGALRM};
static constexpr int HANDLE_SIGNALS_WAIT[] = {SIGCHLD};

class TEpollLoop : public TNonCopyable {
    TError InitializeSignals();
    bool GetSignals(std::vector<int> &signals);

    int EpollFd;

    size_t MaxEvents = 0;
    struct epoll_event *Events = nullptr;

public:
    TError Create();
    void Destroy();
    ~TEpollLoop();

    TError AddFd(int fd);
    TError RemoveFd(int fd);
    TError GetEvents(std::vector<int> &signals,
                     std::vector<struct epoll_event> &evts,
                     int timeout);
};
