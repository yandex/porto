#include "epoll.hpp"
#include "config.hpp"
#include "util/unix.hpp"
#include "util/log.hpp"

extern "C" {
#include <unistd.h>
}

static volatile sig_atomic_t signal_mask;
static void MultiHandler(int sig) {
    if (sig < 32) /* Ignore other boring signals */
        signal_mask |= (1 << sig);
}

static void DumpStack(int sig) {
    Crash();
    RaiseSignal(sig);
}

static TError EpollCreate(int &epfd) {
    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0)
        return TError(EError::Unknown, errno, "epoll_create1()");
    return TError::Success();
}

static TError EpollAdd(int &epfd, int fd) {
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLHUP;
    ev.data.fd = fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0)
        return TError(EError::Unknown, errno, "epoll_add(" + std::to_string(fd) + ")");
    return TError::Success();
}

static TError EpollRemove(int &epfd, int fd) {
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr) < 0)
        return TError(EError::Unknown, errno, "epoll_del(" + std::to_string(fd) + ")");
    return TError::Success();
}

TError TEpollLoop::InitializeSignals() {
    sigset_t mask;

    if (sigemptyset(&mask) < 0)
        return TError(EError::Unknown, "Can't initialize signal mask", errno);

    for (auto sig: HANDLE_SIGNALS) {
        if (RegisterSignal(sig, MultiHandler))
            return TError(EError::Unknown, "Can't register signal", errno);
    }

    for (auto sig: HANDLE_SIGNALS_WAIT) {
        if (RegisterSignal(sig, MultiHandler))
            return TError(EError::Unknown, "Can't register signal", errno);
        if (sigaddset(&mask, sig) < 0)
            return TError(EError::Unknown, "Can't add signal to mask", errno);
    }

    if (RegisterSignal(SIGSEGV, DumpStack))
            return TError(EError::Unknown, "Can't register SIGSEGV handler", errno);

    if (sigprocmask(SIG_SETMASK, &mask, NULL) < 0)
        return TError(EError::Unknown, "Can't set signal mask: ", errno);

    return TError::Success();
}

TError TEpollLoop::Create() {
    TError error = EpollCreate(EpollFd);
    if (error)
        return error;

    error = InitializeSignals();
    if (error) {
        Destroy();
        return error;
    }

    return TError::Success();
}

void TEpollLoop::Destroy() {
    close(EpollFd);
}

TError TEpollLoop::AddFd(int fd) {
    return EpollAdd(EpollFd, fd);
}

TError TEpollLoop::RemoveFd(int fd) {
    return EpollRemove(EpollFd, fd);
}

bool TEpollLoop::GetSignals(std::vector<int> &signals) {
    signals.clear();

    bool ret = false;
    for (int sig = ffs(signal_mask) - 1; sig > 0; sig = ffs(signal_mask) - 1) {
        signal_mask &= ~ (1 << sig);
        signals.push_back(sig);
        ret = true;
    }

    return ret;
}

TEpollLoop::~TEpollLoop() {
    delete[] Events;
}

TError TEpollLoop::GetEvents(std::vector<int> &signals,
                             std::vector<struct epoll_event> &evts,
                             int timeout) {
    evts.clear();

    if (MaxEvents != config().daemon().max_clients()) {
        delete[] Events;
        MaxEvents = config().daemon().max_clients();
        Events = new struct epoll_event[MaxEvents];
    }
    PORTO_ASSERT(Events);

    sigset_t mask;
    if (sigemptyset(&mask) < 0)
        return TError(EError::Unknown, "Can't initialize signal mask: ", errno);

    if (!GetSignals(signals)) {
        int nr = epoll_pwait(EpollFd, Events, MaxEvents, timeout, &mask);
        if (nr < 0) {
            if (errno != EINTR)
                return TError(EError::Unknown, "epoll() error: ", errno);
        }

        GetSignals(signals);

        for (int i = 0; i < nr; i++)
            evts.push_back(Events[i]);
    }

    return TError::Success();
}
