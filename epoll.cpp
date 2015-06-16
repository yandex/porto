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

    if (sig == debugSignal)
        PrintTrace();
}

static void DumpStackAndDie(int sig) {
    L_EVT() << "Received fatal signal " << strsignal(sig) << std::endl;
    PrintTrace();
    RaiseSignal(sig);
}

static void DumpStack(int sig) {
    L_EVT() << "Received " << strsignal(sig) << std::endl;
    PrintTrace();
}

static TError EpollCreate(int &epfd) {
    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0)
        return TError(EError::Unknown, errno, "epoll_create1()");
    return TError::Success();
}

TError TEpollLoop::InitializeSignals() {
    sigset_t mask;

    if (sigemptyset(&mask) < 0)
        return TError(EError::Unknown, errno, "Can't initialize signal mask");

    for (auto sig: HANDLE_SIGNALS) {
        if (RegisterSignal(sig, MultiHandler))
            return TError(EError::Unknown, errno, "Can't register signal");
    }

    for (auto sig: HANDLE_SIGNALS_WAIT) {
        if (RegisterSignal(sig, MultiHandler))
            return TError(EError::Unknown, errno, "Can't register signal");
        if (sigaddset(&mask, sig) < 0)
            return TError(EError::Unknown, errno, "Can't add signal to mask");
    }

    if (RegisterSignal(SIGSEGV, DumpStackAndDie))
            return TError(EError::Unknown, errno, "Can't register SIGSEGV handler");

    if (RegisterSignal(SIGPIPE, DumpStack))
            return TError(EError::Unknown, errno, "Can't register SIGPIPE handler");

    if (sigprocmask(SIG_SETMASK, &mask, NULL) < 0)
        return TError(EError::Unknown, errno, "Can't set signal mask: ");

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
    std::lock_guard<std::mutex> lock(Lock);
    Sources.clear();
    close(EpollFd);
    EpollFd = -1;
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

TError TEpollLoop::AddSource(std::shared_ptr<TEpollSource> source) {
    std::lock_guard<std::mutex> lock(Lock);

    void *ptr = static_cast<void *>(source.get());
    Sources[ptr] = source;
    Statistics->EpollSources = Sources.size();

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLHUP;
    ev.data.ptr = ptr;
    if (epoll_ctl(EpollFd, EPOLL_CTL_ADD, source->Fd, &ev) < 0)
        return TError(EError::Unknown, errno, "epoll_add(" + std::to_string(source->Fd) + ")");
    return TError::Success();
}

TError TEpollLoop::RemoveFd(int fd) {
    if (epoll_ctl(EpollFd, EPOLL_CTL_DEL, fd, nullptr) < 0)
        return TError(EError::Unknown, errno, "epoll_del(" + std::to_string(fd) + ")");
    return TError::Success();
}

void TEpollLoop::RemoveSource(std::shared_ptr<TEpollSource> source) {
    std::lock_guard<std::mutex> lock(Lock);

    void *ptr = static_cast<void *>(source.get());
    if (Sources.find(ptr) == Sources.end())
        return;

    Sources.erase(ptr);
    Statistics->EpollSources = Sources.size();

    TError error = RemoveFd(source->Fd);
    if (error)
        L() << "Can't remove fd " << source->Fd << " from epoll: " << error << std::endl;
}

std::shared_ptr<TEpollSource> TEpollLoop::GetSource(void *ptr) {
    std::lock_guard<std::mutex> lock(Lock);

    if (Sources.find(ptr) == Sources.end())
        return nullptr;

    return Sources.at(ptr).lock();
}
