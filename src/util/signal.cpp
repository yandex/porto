#include "signal.hpp"

extern "C" {
#include <stdlib.h>
}

#include "util/log.hpp"

extern "C" {
#include <pthread.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <execinfo.h>
#include <stdlib.h>
#include <cxxabi.h>
#include <string.h>
}

// https://panthema.net/2008/0901-stacktrace-demangled/
// stacktrace.h (c) 2008, Timo Bingmann from http://idlebox.net/
// published under the WTFPL v2.0

void Stacktrace() {
    L_STK("Stacktrace:");

    // storage array for stack trace address data
    void* addrlist[64];

    // retrieve current stack addresses
    int addrlen = backtrace(addrlist, sizeof(addrlist) / sizeof(void*));

    if (addrlen == 0) {
        L_STK("  <empty, possibly corrupt>\n");
        return;
    }

    // resolve addresses into strings containing "filename(function+address)",
    // this array must be free()-ed
    char** symbollist = backtrace_symbols(addrlist, addrlen);

    // allocate string which will be filled with the demangled function name
    size_t funcnamesize = 256;
    char* funcname = (char*)malloc(funcnamesize);

    // iterate over the returned symbol lines. skip the first, it is the
    // address of this function.
    for (int i = 1; i < addrlen; i++) {
        char *begin_name = 0, *begin_offset = 0, *end_offset = 0;
        char *begin_addr = 0;

        // find parentheses and +address offset surrounding the mangled name:
        // ./module(function+0x15c) [0x8048a6d]
        for (char *p = symbollist[i]; *p; ++p) {
            if (*p == '(')
                begin_name = p;
            else if (*p == '+')
                begin_offset = p;
            else if (*p == ')' && begin_offset)
                end_offset = p;
            else if (*p == '[') {
                begin_addr = p;
                break;
            }
        }

        if (begin_name && begin_offset && end_offset && begin_name < begin_offset) {
            *begin_name++ = '\0';
            *begin_offset++ = '\0';
            *end_offset = '\0';

            // mangled name is now in [begin_name, begin_offset) and caller
            // offset in [begin_offset, end_offset). now apply
            // __cxa_demangle():

            int status;
            char* ret = abi::__cxa_demangle(begin_name, funcname, &funcnamesize, &status);
            if (status == 0) {
                funcname = ret; // use possibly realloc()-ed string
                L_STK("{}: {} {}", symbollist[i], funcname, begin_addr);
            } else {
                // demangling failed. Output function name as a C function with no arguments.
                L_STK("{}: {}()+{} {}", symbollist[i], begin_name, begin_offset, begin_addr);
            }
        } else {
            // couldn't parse the line? print the whole line.
            L_STK(symbollist[i]);
        }
    }

    free(funcname);
    free(symbollist);
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
