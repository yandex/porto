#include <string>
#include <csignal>

#include "util/file.hpp"
#include "util/string.hpp"
#include "util/cred.hpp"
#include "unix.hpp"

extern "C" {
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#include <libgen.h>
#include <sys/sysinfo.h>
#include <sys/prctl.h>
#include <poll.h>
#include <linux/capability.h>
#include <sys/syscall.h>
#include <sys/epoll.h>
}

int RetryBusy(int times, int timeoMs, std::function<int()> handler) {
    int ret = 0;

    if (!times)
        times = 1;

    while (times-- > 0) {
        ret = handler();
        if (errno != EBUSY)
            return ret;
        if (usleep(timeoMs * 1000) < 0)
            return -1;
    }

    return ret;
}

int RetryFailed(int times, int timeoMs, std::function<int()> handler) {
    int ret = 0;

    if (!times)
        times = 1;

    while (times-- > 0) {
        ret = handler();

        if (ret == 0)
            return ret;
        if (usleep(timeoMs * 1000) < 0)
            return -1;
    }

    return ret;
}

int SleepWhile(int timeoMs, std::function<int()> handler) {
    const int resolution = 5;
    int times = timeoMs / resolution;

    if (!times)
        times = 0;

    return RetryFailed(times, resolution, handler);
}

int GetPid() {
    return getpid();
}

size_t GetCurrentTimeMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int RegisterSignal(int signum, void (*handler)(int)) {
    struct sigaction sa = {};

    sa.sa_handler = handler;
    return sigaction(signum, &sa, NULL);
}

int RegisterSignal(int signum, void (*handler)(int sig, siginfo_t *si, void *unused)) {
    struct sigaction sa = {};

    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;

    return sigaction(signum, &sa, NULL);
}

void ResetAllSignalHandlers(void) {
    int sig;

    for (sig = 1; sig < _NSIG; sig++) {
        struct sigaction sa = {};
        sa.sa_handler = SIG_DFL;
        sa.sa_flags = SA_RESTART;

        if (sig == SIGKILL || sig == SIGSTOP)
            continue;

        (void)sigaction(sig, &sa, NULL);
    }

    sigset_t mask;
    if (sigemptyset(&mask) < 0)
        return;

    (void)sigprocmask(SIG_SETMASK, &mask, NULL);
}

size_t GetTotalMemory() {
    struct sysinfo si;
    if (sysinfo(&si) < 0)
        return 0;

    return si.totalram;
}

int CreatePidFile(const std::string &path, const int mode) {
    TFile f(path, mode);

    return f.WriteStringNoAppend(std::to_string(getpid()));
}

void RemovePidFile(const std::string &path) {
    TFile f(path);
    if (f.Exists())
        (void)f.Remove();
}

void SetProcessName(const std::string &name) {
    prctl(PR_SET_NAME, (void *)name.c_str());
}

void SetDieOnParentExit() {
    prctl(PR_SET_PDEATHSIG, SIGHUP);
}

std::string GetProcessName() {
    char name[17];

    if (prctl(PR_GET_NAME, (void *)name) < 0)
        strncpy(name, program_invocation_short_name, sizeof(name));

    return name;
}

TError GetTaskCgroups(const int pid, std::map<std::string, std::string> &cgmap) {
    TFile f("/proc/" + std::to_string(pid) + "/cgroup");
    std::vector<std::string> lines;
    TError error = f.AsLines(lines);
    if (error)
        return error;

    std::vector<std::string> tokens;
    for (auto l : lines) {
        tokens.clear();
        error = SplitString(l, ':', tokens, 3);
        if (error)
            return error;
        cgmap[tokens[1]] = tokens[2];
    }

    return TError::Success();
}

int BlockAllSignals() {
    sigset_t mask;

    if (sigfillset(&mask) < 0)
        return -1;

    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0)
        return -1;

    return 0;
}

std::string GetHostName() {
    char buf[256];
    int ret = gethostname(buf, sizeof(buf));
    if (ret < 0)
        return "";
    buf[sizeof(buf) - 1] = '\0';
    return buf;
}

TError SetHostName(const std::string &name) {
    int ret = sethostname(name.c_str(), name.length());
    if (ret < 0)
        return TError(EError::Unknown, errno, "sethostname(" + name + ")");

    return TError::Success();
}

bool FdHasEvent(int fd) {
    struct pollfd pfd = {};
    pfd.fd = fd;
    pfd.events = POLLIN;

    (void)poll(&pfd, 1, 0);
    return pfd.revents != 0;
}

TError DropBoundedCap(int cap) {
    if (prctl(PR_CAPBSET_DROP, cap, 0, 0, 0) < 0)
        return TError(EError::Unknown, errno,
                      "prctl(PR_CAPBSET_DROP, " + std::to_string(cap) + ")");

    return TError::Success();
}

TError SetCap(uint64_t effective, uint64_t permitted, uint64_t inheritable) {
    cap_user_header_t hdrp = (cap_user_header_t)malloc(sizeof(*hdrp));
    cap_user_data_t datap = (cap_user_data_t)malloc(sizeof(*datap) * 2);
    if (!hdrp || !datap)
        throw std::bad_alloc();

    hdrp->version = _LINUX_CAPABILITY_VERSION_3;
    hdrp->pid = getpid();
    datap[0].effective = (uint32_t)effective;
    datap[1].effective = (uint32_t)(effective >> 32);
    datap[0].permitted = (uint32_t)permitted;
    datap[1].permitted = (uint32_t)(permitted >> 32);
    datap[0].inheritable = (uint32_t)inheritable;
    datap[1].inheritable = (uint32_t)(inheritable >> 32);

    if (syscall(SYS_capset, hdrp, datap) < 0) {
        int err = errno;
        free(hdrp);
        free(datap);
        return TError(EError::Unknown, err, "capset(" +
                      std::to_string(effective) + ", " +
                      std::to_string(permitted) + ", " +
                      std::to_string(inheritable) + ")");
    }

    free(hdrp);
    free(datap);

    return TError::Success();
}

TScopedFd::TScopedFd(int fd) : Fd(fd) {
}

TScopedFd::~TScopedFd() {
    if (Fd >= 0)
        close(Fd);
}

int TScopedFd::GetFd() {
    return Fd;
}

TScopedFd &TScopedFd::operator=(int fd) {
    if (Fd >= 0)
        close(Fd);

    Fd = fd;

    return *this;
}

TError SetOomScoreAdj(int value) {
    TFile f("/proc/self/oom_score_adj");
    return f.WriteStringNoAppend(std::to_string(value));
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

int64_t GetBootTime() {
    std::vector<std::string> lines;
    TFile f("/proc/stat");
    if (f.AsLines(lines))
        return 0;

    for (auto &line : lines) {
        std::vector<std::string> cols;
        if (SplitString(line, ' ', cols))
            return 0;

        if (cols[0] == "btime") {
            int64_t val;
            if (StringToInt64(cols[1], val))
                return 0;
            return val;
        }
    }

    return 0;
}
