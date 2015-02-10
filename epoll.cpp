#include "epoll.hpp"
#include "util/unix.hpp"

extern "C" {
#include <unistd.h>
}

static volatile sig_atomic_t signal_mask;
static void MultiHandler(int sig) {
    if (sig < 32) /* Ignore other boring signals */
        signal_mask |= (1 << sig);
}

TError EpollCreate(int &epfd) {
    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0)
        return TError(EError::Unknown, errno, "epoll_create1()");
    return TError::Success();
}

TError EpollAdd(int &epfd, int fd) {
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLHUP;
    ev.data.fd = fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0)
        return TError(EError::Unknown, errno, "epoll_add(" + std::to_string(fd) + ")");
    return TError::Success();
}

TError TEpollLoop::InitializeSignals() {
    sigset_t mask;

    if (sigemptyset(&mask) < 0)
        return TError(EError::Unknown, "Can't initialize signal mask: ", errno);

    for (auto sig: HANDLE_SIGNALS) {
        if (RegisterSignal(sig, MultiHandler))
            return TError(EError::Unknown, "Can't register signal: ", errno);
    }

    for (auto sig: HANDLE_SIGNALS_WAIT) {
        if (RegisterSignal(sig, MultiHandler))
            return TError(EError::Unknown, "Can't register signal: ", errno);
        if (sigaddset(&mask, sig) < 0)
            return TError(EError::Unknown, "Can't add signal to mask: ", errno);
    }

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

TError TEpollLoop::GetEvents(std::vector<int> &signals,
                             std::vector<struct epoll_event> &events,
                             int timeout) {
    events.clear();

    sigset_t mask;

    if (sigemptyset(&mask) < 0)
        return TError(EError::Unknown, "Can't initialize signal mask: ", errno);

    if (!GetSignals(signals)) {
        int nr = epoll_pwait(EpollFd, Events, MAX_EVENTS, timeout, &mask);
        if (nr < 0) {
            if (errno != EINTR)
                return TError(EError::Unknown, "epoll() error: ", errno);
        }

        GetSignals(signals);

        for (int i = 0; i < nr; i++)
            events.push_back(Events[i]);
    }

    return TError::Success();
}
