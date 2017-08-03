#include <iostream>

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

bool Verbose;
bool Debug;

TStatistics *Statistics = nullptr;

//FIXME KILL THIS SHIT

static int logBufFd = STDERR_FILENO;
class TLogBuf : public std::streambuf {
    std::vector<char> Data;
public:
    TLogBuf(const size_t size);
    void Open(const TPath &path, const unsigned int mode);
    int GetFd() { return logBufFd; }
    void SetFd(int fd) { logBufFd = fd; }
    void ClearBuffer() {
        std::ptrdiff_t n = pptr() - pbase();
        pbump(-n);
    }
protected:
    int sync() override;
    int_type overflow(int_type ch) override;
};

static __thread TLogBuf *logBuf;
static __thread std::ostream *logStream;

static inline void PrepareLog() {
    if (!logBuf) {
        logBuf = new TLogBuf(1024);
        logStream = new std::ostream(logBuf);
    }
}

void TLogger::OpenLog(bool std, const TPath &path, const unsigned int mode) {
    PrepareLog();
    if (std) {
        // because in task.cpp we expect that nothing should be in 0-2 fd,
        // we need to duplicate our std log somewhere else
        int logFd = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
        PORTO_ASSERT(logFd > 2);
        logBuf->SetFd(logFd);
    } else {
        logBuf->Open(path, mode);
        /* redirect stdout and stderr into log */
        dup3(logBuf->GetFd(), STDOUT_FILENO, O_CLOEXEC);
        dup3(logBuf->GetFd(), STDERR_FILENO, O_CLOEXEC);
    }
}

void TLogger::DisableLog() {
    PrepareLog();
    TLogger::CloseLog();
    logBuf->SetFd(-1);
}

int TLogger::GetFd() {
    PrepareLog();
    return logBuf->GetFd();
}

void TLogger::CloseLog() {
    PrepareLog();
    int fd = logBuf->GetFd();
    if (fd > 2)
        close(fd);
    logBuf->SetFd(STDOUT_FILENO);
    logBuf->ClearBuffer();
}

TLogBuf::TLogBuf(const size_t size) {
    Data.reserve(size);
    char *base = static_cast<char *>(Data.data());
    setp(base, base + Data.capacity() - 1);
}

void TLogBuf::Open(const TPath &path, const unsigned int mode) {
    struct stat st;
    if (!path.StatFollow(st) && (st.st_mode & 0777) != mode)
        (void)path.Chmod(mode);

    logBufFd = open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC |
                                  O_NOFOLLOW | O_NOCTTY, mode);
    if (logBufFd < 0)
        logBufFd = STDERR_FILENO;
}

int TLogBuf::sync() {
    std::ptrdiff_t n = pptr() - pbase();
    pbump(-n);

    int ret = write(logBufFd, pbase(), n);
    return (ret == n) ? 0 : -1;
}

TLogBuf::int_type TLogBuf::overflow(int_type ch) {
    if (ch != traits_type::eof()) {
        if (sync() < 0)
            return traits_type::eof();

        PORTO_ASSERT(std::less_equal<char *>()(pptr(), epptr()));
        *pptr() = ch;
        pbump(1);

        return ch;
    }

    return traits_type::eof();
}

void TLogger::Log(std::string log_msg, ELogLevel level) {
    PrepareLog();

    static const std::string prefix[] = { "    ",
                                          "WRN ",
                                          "ERR ",
                                          "EVT ",
                                          "ACT ",
                                          "REQ ",
                                          "RSP ",
                                          "SYS ",
                                          "STK ", };
    std::string name = GetTaskName();

    if (Statistics) {
        if (level == LOG_WARN)
            Statistics->Warns++;
        else if (level == LOG_ERROR)
            Statistics->Errors++;
    }

    if (level == LOG_ERROR && Verbose)
        Stacktrace();

    std::string msg = FormatTime(time(nullptr)) + " " +
        name + "[" + std::to_string(GetTid()) + "]: " +
        prefix[level] + log_msg +
        (log_msg.back() == '\n' ? "" : "\n");

    (*logStream) << msg;
    logStream->flush();
}

void porto_assert(const char *msg, size_t line, const char *file) {
    L_ERR("Assertion failed: {} at {}:{}", msg, file, line);
    Crash();
}

void porto_runtime_error(const std::string &msg, size_t line, const char *file) {
    L_ERR("Runtime error: {} at {}:{}", msg, file, line);
    Crash();
}
