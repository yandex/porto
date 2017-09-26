#include "statistics.hpp"
#include "log.hpp"
#include "util/unix.hpp"
#include "util/signal.hpp"
#include "common.hpp"

extern "C" {
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
}

bool StdLog = false;
bool Verbose = false;
bool Debug = false;

TStatistics *Statistics = nullptr;

TFile LogFile(STDOUT_FILENO);

void OpenLog(const TPath &path) {
    int fd;

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

    if (fd > 2) {
        if (LogFile.Fd != STDOUT_FILENO)
            LogFile.Close();
        LogFile.SetFd = fd;

        /* redirect stdout and stderr into log */
        dup3(fd, STDOUT_FILENO, O_CLOEXEC);
        dup3(fd, STDERR_FILENO, O_CLOEXEC);
    }
}

void WriteLog(std::string log_msg, ELogLevel level) {

    if (Statistics) {
        if (level == LOG_WARN)
            Statistics->Warns++;
        else if (level == LOG_ERROR)
            Statistics->Errors++;
    }

    static const std::string prefix[] = { "    ",
                                          "WRN ",
                                          "ERR ",
                                          "EVT ",
                                          "ACT ",
                                          "REQ ",
                                          "RSP ",
                                          "SYS ",
                                          "STK ", };

    std::string msg = FormatTime(time(nullptr)) + " " +
        GetTaskName() + "[" + std::to_string(GetTid()) + "]: " +
        prefix[level] + log_msg +
        (log_msg.back() == '\n' ? "" : "\n");

    if (Statistics) {
        Statistics->LogLines++;
        Statistics->LogBytes += msg.size();
    }

    if (LogFile)
        LogFile.WriteAll(msg);

    if (level == LOG_ERROR && Verbose)
        Stacktrace();
}

void porto_assert(const char *msg, const char *file, size_t line) {
    L_ERR("Assertion failed: {} at {}:{}", msg, file, line);
    Crash();
}
