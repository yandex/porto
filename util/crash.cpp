#include <atomic>

#include "util/log.hpp"
#include "util/unix.hpp"

extern "C" {
#include <pthread.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <execinfo.h>
#include <stdlib.h>
}

void PrintTrace() {
    void *array[64];
    size_t size;
    char **strings;
    size_t i;

    size = backtrace(array, 64);
    strings = backtrace_symbols(array, size);

    L(LOG_NOTICE) << "Backtrace:" << std::endl;
    for (i = 0; i < size; i++)
        L(LOG_NOTICE) << strings[i] << std::endl;

    free (strings);
}

void Crash() {
    L(LOG_ERROR) << "Crashed" << std::endl;
    PrintTrace();
    exit(-1);
}
