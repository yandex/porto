#include <string>
#include <csignal>

#include "util/file.hpp"
#include "util/string.hpp"
#include "util/cred.hpp"
#include "util/path.hpp"
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
#include <sys/signalfd.h>
#include <fcntl.h>
#include <sys/wait.h>
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

void RaiseSignal(int signum) {
    ResetAllSignalHandlers();

    raise(signum);
    exit(-signum);
}

static volatile sig_atomic_t signal_mask;
static void MultiHandler(int sig) {
    if (sig < 32) /* Ignore other boring signals */
        signal_mask |= (1 << sig);
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

    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0)
        return TError(EError::Unknown, "Can't set signal mask: ", errno);

    return TError::Success();
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
    (void)prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0);
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

void CloseAllFds() {
    for (int i = 1; i < getdtablesize(); i++)
        close(i);
}

// TEpollLoop

TError TEpollLoop::Create() {
    TError error = EpollCreate(EpollFd);
    if (error)
        return error;

    error = InitializeSignals();
    if (error) {
        close(EpollFd);
        return error;
    }

    return TError::Success();
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

TError AllocLoop(const TPath &path, size_t size) {
    TError error;
    TScopedFd fd;
    uint8_t ch = 0;
    int status;

    fd = open(path.ToString().c_str(), O_WRONLY | O_CREAT, 0755);
    if (fd.GetFd() < 0)
        return TError(EError::Unknown, errno, "open(" + path.ToString() + ")");

    int ret = lseek(fd.GetFd(), size - 1, SEEK_SET);
    if (ret < 0) {
        error = TError(EError::Unknown, errno, "lseek(" + path.ToString() + ")");
        goto remove_file;
    }

    ret = write(fd.GetFd(), &ch, sizeof(ch));
    if (ret < 0) {
        error = TError(EError::Unknown, errno, "write(" + path.ToString() + ")");
        goto remove_file;
    }

    fd = -1;

    error = Run({ "mkfs.ext4", "-F", "-F", path.ToString() }, status);
    if (error)
        goto remove_file;

    if (status) {
        error = TError(EError::Unknown, error.GetErrno(), "mkfs: " + error.GetMsg());
        goto remove_file;
    }

    return TError::Success();

remove_file:
    TFile f(path);
    (void)f.Remove();

    return error;
}

TError Run(const std::vector<std::string> &command, int &status) {
    int pid = fork();
    if (pid < 0) {
        return TError(EError::Unknown, errno, "fork()");
    } else if (pid > 0) {
        int ret;
retry:
        ret = waitpid(pid, &status, 0);
        if (ret < 0) {
            if (errno == EINTR)
                goto retry;
            return TError(EError::Unknown, errno, "waitpid(" + std::to_string(pid) + ")");
        }
    } else {
        SetDieOnParentExit();

        char **p = (char **)malloc(sizeof(*p) * command.size() + 1);
        for (size_t i = 0; i < command.size(); i++)
            p[i] = strdup(command[i].c_str());
        p[command.size()] = nullptr;

        execvp(command[0].c_str(), p);
        _exit(EXIT_FAILURE);
    }

    return TError::Success();
}

TError Popen(const std::string &cmd, std::vector<std::string> &lines) {
    FILE *f = popen(cmd.c_str(), "r");
    if (f == nullptr)
        return TError(EError::Unknown, errno, "Can't execute " + cmd);

    char *line = nullptr;
    size_t n;

    while (getline(&line, &n, f) >= 0)
        lines.push_back(line);

    fclose(f);

    return TError::Success();
}
