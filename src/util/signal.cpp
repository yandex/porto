#include "signal.hpp"
#include "util/log.hpp"

extern "C" {
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <string.h>
}

void Crash() {
    L_ERR("Crashed");
    Stacktrace();

    /* that's all */
    Signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
    _exit(128);
}

void FatalSignal(int sig) {
    Statistics->Fatals++;

    /* don't hang */
    alarm(5);

    L_ERR("Fatal signal: {}", std::string(strsignal(sig)));
    Stacktrace();

    /* ok, die */
    Signal(sig, SIG_DFL);
    raise(sig);
    _exit(128 + sig);
}

void CatchFatalSignals() {
    Signal(SIGILL, FatalSignal);
    Signal(SIGABRT, FatalSignal);
    Signal(SIGFPE, FatalSignal);
    Signal(SIGSEGV, FatalSignal);
    Signal(SIGBUS, FatalSignal);

    Signal(SIGPIPE, SIG_IGN);
}

void ResetBlockedSignals() {
    sigset_t sigMask;

    sigemptyset(&sigMask);
    if (sigprocmask(SIG_SETMASK, &sigMask, NULL)) {
        L_ERR("Cannot unblock signals");
        Crash();
    }
}

void ResetIgnoredSignals() {
    Signal(SIGPIPE, SIG_DFL);
}

void Signal(int signum, void (*handler)(int)) {
    struct sigaction sa = {};

    sa.sa_handler = handler;
    if (sigaction(signum, &sa, NULL)) {
        L_ERR("Cannot set signal action");
        Crash();
    }
}

int SignalFd() {
    sigset_t sigMask;

    sigemptyset(&sigMask);
    sigaddset(&sigMask, SIGHUP);
    sigaddset(&sigMask, SIGINT);
    sigaddset(&sigMask, SIGTERM);
    sigaddset(&sigMask, SIGUSR1);
    sigaddset(&sigMask, SIGUSR2);
    sigaddset(&sigMask, SIGCHLD);

    if (sigprocmask(SIG_BLOCK, &sigMask, NULL)) {
        L_ERR("Cannot block signals");
        Crash();
    }

    int sigFd = signalfd(-1, &sigMask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sigFd < 0) {
        L_ERR("Cannot create signalfd");
        Crash();
    }

    return sigFd;
}
