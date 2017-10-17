#include "epoll.hpp"
#include "statistics.hpp"
#include "config.hpp"
#include "util/unix.hpp"
#include "util/log.hpp"

extern "C" {
#include <unistd.h>
#include <sys/epoll.h>
}

static TError EpollCreate(int &epfd) {
    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0)
        return TError(EError::Unknown, errno, "epoll_create1()");
    return TError::Success();
}

TError TEpollLoop::Create() {
    TError error = EpollCreate(EpollFd);
    if (error)
        return error;

    Statistics->EpollSources = 0;

    return TError::Success();
}

void TEpollLoop::Destroy() {
    auto lock = ScopedLock();
    Sources.clear();
    close(EpollFd);
    EpollFd = -1;
}

TEpollLoop::~TEpollLoop() {
    delete[] Events;
}

TError TEpollLoop::GetEvents(std::vector<struct epoll_event> &evts, int timeout) {
    evts.clear();

    if (MaxEvents != config().daemon().max_clients() + NR_SUPERUSER_CLIENTS) {
        delete[] Events;
        MaxEvents = config().daemon().max_clients() + NR_SUPERUSER_CLIENTS;
        Events = new struct epoll_event[MaxEvents];
    }
    PORTO_ASSERT(Events);

    int nr = epoll_wait(EpollFd, Events, MaxEvents, timeout);
    if (nr < 0) {
        if (errno != EINTR)
            return TError(EError::Unknown, "epoll() error: ", errno);
    }

    for (int i = 0; i < nr; i++)
        evts.push_back(Events[i]);

    return TError::Success();
}

TError TEpollLoop::AddSource(std::shared_ptr<TEpollSource> source) {
    int fd = source->Fd;
    auto lock = ScopedLock();

    if ((int)Sources.size() <= fd)
        Sources.resize(fd + 256);

    if (!Sources[fd].expired()) {
        L_ERR("Duplicate epoll fd {}", fd);
        return TError(EError::Unknown, "dublicate epoll fd");
    }

    Sources[fd] = source;
    Statistics->EpollSources++;

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLHUP;
    ev.data.fd = fd;
    if (epoll_ctl(EpollFd, EPOLL_CTL_ADD, fd, &ev) < 0)
        return TError(EError::Unknown, errno, "epoll_add(" + std::to_string(fd) + ")");

    return TError::Success();
}

void TEpollLoop::RemoveSource(int fd) {
    auto lock = ScopedLock();

    if (fd < (int)Sources.size() && !Sources[fd].expired()) {
        Sources[fd].reset();
        Statistics->EpollSources--;
    } else
        L_ERR("Invalid epoll fd {}", fd);

    if (epoll_ctl(EpollFd, EPOLL_CTL_DEL, fd, nullptr) < 0)
        L_ERR("Cannot remove epoll {} : {}", fd,
              TError(EError::Unknown, errno, "epoll_ctl"));
}

TError TEpollLoop::ModifySourceEvents(int fd, uint32_t events) const {
    struct epoll_event ev;
    ev.events = events;
    ev.data.fd = fd;
    if (epoll_ctl(EpollFd, EPOLL_CTL_MOD, fd, &ev) < 0)
        return TError(EError::Unknown, errno, "epoll_mod(" + std::to_string(fd) + ")");
    return TError::Success();
}

TError TEpollLoop::StartInput(int fd) const {
    return ModifySourceEvents(fd, EPOLLIN);
}

TError TEpollLoop::StopInput(int fd) const {
    return ModifySourceEvents(fd, 0);
}

TError TEpollLoop::StartOutput(int fd) const {
    return ModifySourceEvents(fd, EPOLLOUT);
}

std::shared_ptr<TEpollSource> TEpollLoop::GetSource(int fd) {
    auto lock = ScopedLock();

    if (fd >= (int)Sources.size())
        return nullptr;

    return Sources[fd].lock();
}
