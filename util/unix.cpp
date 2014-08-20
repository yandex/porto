#include <string>

#include "log.hpp"
#include "unix.hpp"

extern "C" {
#include <signal.h>
#include <unistd.h>
#include <errno.h>
}

int RetryBusy(int times, int timeo, std::function<int()> handler) {
    int ret = 0;

    if (!times)
        times = 1;

    while (times--) {
        ret = handler();
        if (errno != EBUSY)
            return ret;
        usleep(timeo);
    }

    return ret;
}

int RetryFailed(int times, int timeo, std::function<int()> handler) {
    int ret;

    if (!times)
        times = 1;

    while (times--) {
        ret = handler();
        if (ret >= 0)
            return ret;
        usleep(timeo);
    }

    return ret;
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
