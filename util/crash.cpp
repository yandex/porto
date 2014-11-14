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
