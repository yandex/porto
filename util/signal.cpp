#include "signal.hpp"

extern "C" {
#include <stdlib.h>
}

static int UnblockSignal(int sig) {
    sigset_t mask;
    auto ret = sigfillset(&mask);
    if (ret < 0)
        return ret;
    ret = sigaddset(&mask, sig);
    if (ret < 0)
        return ret;
    return sigprocmask(SIG_UNBLOCK, &mask, NULL);
}

int RegisterSignal(int signum, void (*handler)(int)) {
    struct sigaction sa = {};

    auto ret = UnblockSignal(signum);
    if (ret < 0)
        return ret;

    sa.sa_handler = handler;
    return sigaction(signum, &sa, NULL);
}

int RegisterSignal(int signum, void (*handler)(int sig, siginfo_t *si, void *unused)) {
    struct sigaction sa = {};

    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;

    auto ret = UnblockSignal(signum);
    if (ret < 0)
        return ret;

    return sigaction(signum, &sa, NULL);
}

void ResetSignalHandler(int signum) {
    if (signum == SIGKILL || signum == SIGSTOP)
        return;

    struct sigaction sa = {};
    sa.sa_handler = SIG_DFL;
    sa.sa_flags = SA_RESTART;

    (void)sigaction(signum, &sa, NULL);
}

void ResetAllSignalHandlers(void) {
    int sig;

    for (sig = 1; sig < _NSIG; sig++)
        ResetSignalHandler(sig);

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

void BlockAllSignals() {
    sigset_t mask;
    if (sigfillset(&mask) < 0)
        return;
    (void)sigprocmask(SIG_SETMASK, &mask, NULL);
}
