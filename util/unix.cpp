#include <string>
#include <csignal>

#include "unix.hpp"

extern "C" {
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <string.h>
#include <libgen.h>
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

size_t GetCurrentTime() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec;
}

int RegisterSignal(int signum, void (*handler)(int)) {
    struct sigaction sa = {};

    sa.sa_handler = handler;
    if (sigaction(signum, &sa, NULL) < 0)
        return -1;
    return 0;
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
