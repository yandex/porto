#include <string>
#include <csignal>

#include "util/file.hpp"
#include "unix.hpp"

extern "C" {
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#include <libgen.h>
#include <sys/sysinfo.h>
#include <grp.h>
#include <pwd.h>
#include <sys/prctl.h>
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
}

std::string DirName(const std::string &str) {
    char *dup = strdup(str.c_str());
    if (!dup)
        throw std::bad_alloc();

    char *p = dirname(dup);
    std::string out(p);
    free(dup);

    return out;
}

size_t GetTotalMemory() {
    struct sysinfo si;
    if (sysinfo(&si) < 0)
        return 0;

    return si.totalram;
}

std::string GetDefaultUser() {
    std::string users[] = { "nobody" };

    for (auto &u : users)
        if (getpwnam(u.c_str()) != NULL)
            return u;

    return "daemon";
}

std::string GetDefaultGroup() {
    std::string groups[] = { "nobody", "nogroup" };

    for (auto &g : groups)
        if (getgrnam(g.c_str()) != NULL)
            return g;

    return "daemon";
}

int CreatePidFile(const std::string &path, const int mode) {
    TFile f(path, mode);

    return f.WriteStringNoAppend(std::to_string(getpid()));
}

void RemovePidFile(const std::string &path) {
    TFile f(path);
    (void)f.Remove();
}

void SetProcessName(const std::string &name) {
    prctl(PR_SET_NAME, (void *)name.c_str());
}
