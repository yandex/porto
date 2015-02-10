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

TError EpollCreate(int &epfd);
TError EpollAdd(int &epfd, int fd);

class TEpollLoop : public TNonCopyable {
public:
    TError Create();
    void Destroy();

    TError AddFd(int fd);
    TError GetEvents(std::vector<int> &signals,
                     std::vector<struct epoll_event> &events,
                     int timeout);

private:
    TError InitializeSignals();
    bool GetSignals(std::vector<int> &signals);

    int EpollFd;

    static const int MAX_EVENTS = 32;
    struct epoll_event Events[MAX_EVENTS];
};
