#include <string>
#include <csignal>

#include "log.hpp"
#include "unix.hpp"

extern "C" {
#include <unistd.h>
#include <errno.h>
}

int RetryBusy(int times, int timeo_ms, std::function<int()> handler) {
    int ret;

    if (!times)
        times = 1;

    while (times--) {
        ret = handler();
        if (errno != EBUSY)
            return ret;
        usleep(timeo_ms * 1000);
    }

    return ret;
}

int RetryFailed(int times, int timeo_ms, std::function<int()> handler) {
    int ret;

    if (!times)
        times = 1;

    while (times--) {
        ret = handler();
        if (ret == 0)
            return ret;
        usleep(timeo_ms * 1000);
    }

    return ret;
}

void SleepWhile(int timeo_ms, std::function<bool()> handler) {
    const int resolution = 10;
    int times = timeo_ms / resolution;

    if (!times)
        times = 0;

    (void)RetryFailed(times, resolution, [&]{ return handler() != true; });
}

int GetPid() {
    return getpid();
}

int RegisterSignal(int signum, void (*handler)(int)) {
    struct sigaction sa = { 0 };

    sa.sa_handler = handler;
    if (sigaction(signum, &sa, NULL) < 0)
        return -1;
    return 0;
}

void ResetAllSignalHandlers(void) {
    int sig;

    for (sig = 1; sig < _NSIG; sig++) {
        struct sigaction sa = { 0 };
        sa.sa_handler = SIG_DFL;
        sa.sa_flags = SA_RESTART;

        if (sig == SIGKILL || sig == SIGSTOP)
            continue;

        (void)sigaction(sig, &sa, NULL);
    }
}
