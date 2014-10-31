#include <atomic>

#include "porto.hpp"
#include "util/log.hpp"
#include "util/unix.hpp"

extern "C" {
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <execinfo.h>
#include <stdlib.h>
}

static void PrintTrace() {
    void *array[20];
    size_t size;
    char **strings;
    size_t i;

    size = backtrace (array, 20);
    strings = backtrace_symbols (array, size);

    TLogger::Log() << "Backtrace:" << std::endl;
    for (i = 0; i < size; i++)
        TLogger::Log() << strings[i] << std::endl;

    free (strings);
}

void Crash() {
    TLogger::Log() << "Crashed" << std::endl;
    PrintTrace();
    exit(-1);
}

/*
static void SigsegvHandler(int sig, siginfo_t *si, void *unused) {
    TLogger::Log() << "SIGSEGV at %p" << si->si_addr;
    Crash();
}
*/

static std::atomic<unsigned long> watchdogCounter;
static unsigned int maxFails, delayS;

void WatchdogStrobe() {
    watchdogCounter++;
}

static void* WatchdogCheck(void *arg) {
    unsigned long prevValue = 0;
    unsigned long fails = 0;

    prctl(PR_SET_NAME, "portod-watchdog");

    if (BlockAllSignals())
        TLogger::Log() << "Error: can't block all signals" << std::endl;

    while (1) {
        if (watchdogCounter > prevValue)
            fails = 0;
        else
            fails++;

        prevValue = watchdogCounter;

        if (fails > maxFails) {
            TLogger::Log() << "Watchdog stalled" << std::endl;
            Crash();
        }

        sleep(delayS);
    }

    return NULL;
}

void WatchdogStart(int maxFailsArg, int delaySArg) {
//    (void)RegisterSignal(SIGSEGV, SigsegvHandler);

    maxFails = maxFailsArg;
    delayS = delaySArg;
    pthread_t thread;
    pthread_create(&thread, NULL, WatchdogCheck, NULL);
}
