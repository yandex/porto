#include "log.hpp"
#include "util/unix.hpp"
#include "util/signal.hpp"
#include "common.hpp"

extern "C" {
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <execinfo.h>
#include <cxxabi.h>
}

bool StdLog = false;
bool Verbose = false;
bool Debug = false;

TStatistics *Statistics = nullptr;

void InitStatistics() {
    TError error;
    TFile file;

    error = file.Create(PORTOD_STAT_FILE, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (!error)
        error = file.Truncate(sizeof(TStatistics));
    if (error) {
        L_ERR("Cannot init {} {}", PORTOD_STAT_FILE, error);
        file.Close();
    }

    Statistics = (TStatistics *)mmap(nullptr, sizeof(TStatistics),
                                     PROT_READ | PROT_WRITE,
                                     MAP_SHARED | (file ? 0 : MAP_ANONYMOUS),
                                     file.Fd, 0);
    PORTO_ASSERT(Statistics != nullptr);
}

TFile LogFile(STDOUT_FILENO);

void OpenLog(const TPath &path) {
    int fd;

    if (Statistics)
        Statistics->LogOpen++;

    if (StdLog) {
        fd = STDOUT_FILENO;
    } else {
        struct stat st;
        fd = open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC |
                                O_NOFOLLOW | O_NOCTTY, 0644);
        if (fd >= 0 && !fstat(fd, &st) && (st.st_mode & 0777) != 0644)
            fchmod(fd, 0644);
    }

    if (fd >= 0 && fd < 3)
        fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);

    if (fd >= 0) {
        if (LogFile.Fd != STDOUT_FILENO)
            LogFile.Close();
        LogFile.SetFd = fd;
    }

    /* redirect stdout and stderr into log */
    if (fd >= 0 && !StdLog) {
        dup3(fd, STDOUT_FILENO, O_CLOEXEC);
        dup3(fd, STDERR_FILENO, O_CLOEXEC);
    }
}

void WriteLog(const char *prefix, const std::string &log_msg) {
    std::string msg = fmt::format("{} {}[{}]: {} {}\n",
            FormatTime(time(nullptr)), GetTaskName(), GetTid(), prefix, log_msg);

    if (Statistics) {
        Statistics->LogLines++;
        Statistics->LogBytes += msg.size();
    }

    if (!LogFile)
        return;

    TError error = LogFile.WriteAll(msg);
    if (error && Statistics) {
        if (error.Errno != ENOSPC &&
                error.Errno != EDQUOT &&
                error.Errno != EROFS &&
                error.Errno != EIO &&
                error.Errno != EUCLEAN)
            Statistics->Warns++;
        Statistics->LogLinesLost++;
        Statistics->LogBytesLost += msg.size();
    }
}

void porto_assert(const char *msg, const char *file, size_t line) {
    L_ERR("Assertion failed: {} at {}:{}", msg, file, line);
    Crash();
}

void FatalError(const std::string &text, TError &error) {
    L_ERR("{}: {}", text, error);
    _exit(EXIT_FAILURE);
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
            L_STK("{}", symbollist[i]);
        }
    }

    free(funcname);
    free(symbollist);
}
