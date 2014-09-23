#include "util/log.hpp"
#include "util/unix.hpp"

#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/prctl.h>

void Crash()
{
    TLogger::Log() << "";
    exit(-1);
}

void SigsegvHandler(int sig, siginfo_t *si, void *unused)
{
    TLogger::Log() << "SIGSEGV at %p" << si->si_addr;
    Crash();
}

#define WATCHDOG_MAX_FAILS 5
static unsigned long watchdog_counter;

void WatchdogStrobe()
{
    watchdog_counter++;
}

void* WatchdogCheck(void *arg)
{
    static unsigned long prev_value = 0;
    static unsigned long fails = 0;

    prctl(PR_SET_NAME, "watchdog");

    while (1) {
        if (watchdog_counter > prev_value)
            fails = 0;
        else
            fails++;

        prev_value = watchdog_counter;

        if (fails > WATCHDOG_MAX_FAILS) {
            TLogger::Log() << "Watchdog stalled" << std::endl;
            Crash();
        }

        sleep(1);
    }

    return NULL;
}

void WatchdogStart()
{
    pthread_t thread;
    pthread_create(&thread, NULL, WatchdogCheck, NULL);
}
