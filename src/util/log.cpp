#include <iostream>
#include <iomanip>
#include <sstream>

#include "statistics.hpp"
#include "log.hpp"
#include "util/unix.hpp"
#include "util/file.hpp"
#include "config.hpp"
#include "common.hpp"

extern "C" {
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
}

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
    bool DieOnNl = false;
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

static __thread struct tm logForkTm;
static __thread struct timeval logForkTimeval;

void TLogger::DisableLocaltime() {
    gettimeofday(&logForkTimeval, NULL);
    localtime_r(&logForkTimeval.tv_sec, &logForkTm);
}

void TLogger::EnableLocaltime() {
    memset(&logForkTm, 0, sizeof(logForkTm));
    memset(&logForkTimeval, 0, sizeof(logForkTimeval));
}

void TLogger::ClearBuffer() {
    PrepareLog();
    logBuf->ClearBuffer();
}

void TLogger::OpenLog(bool std, const TPath &path, const unsigned int mode) {
    PrepareLog();
    if (std) {
        // because in task.cpp we expect that nothing should be in 0-2 fd,
        // we need to duplicate our std log somewhere else
        int logFd = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC);
        PORTO_ASSERT(logFd >= 0);
        logBuf->SetFd(logFd);
    } else {
        logBuf->Open(path, mode);
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

static long tvdiff(struct timeval *start, struct timeval *stop) {
    constexpr auto NSEC_PER_SEC = 1000000000L;
    auto secdiff = stop->tv_sec - start->tv_sec;
    if (secdiff == 1)
        return NSEC_PER_SEC - start->tv_usec + stop->tv_usec >= NSEC_PER_SEC;
    else
        return secdiff;
}

std::string TLogger::GetTime() {
    char buf[256];
    struct timeval tv;
    gettimeofday(&tv, NULL);

    std::stringstream ss;
    if (logForkTimeval.tv_sec) {
        strftime(buf, sizeof(buf), "%F %T", &logForkTm);
        ss << buf;
        auto offset = tvdiff(&logForkTimeval, &tv);
        if (offset)
            ss << "+" << offset;
    } else {
        struct tm *tmp, result;
        if ((tmp = localtime_r(&tv.tv_sec, &result)) != nullptr) {
            strftime(buf, sizeof(buf), "%F %T", tmp);
            ss << buf;
        }

        if (config().log().verbose())
            ss << "." << std::setw(6) << std::setfill('0') << tv.tv_usec;
    }

    return ss.str();
}

TLogBuf::TLogBuf(const size_t size) {
    Data.reserve(size);
    char *base = static_cast<char *>(Data.data());
    setp(base, base + Data.capacity() - 1);
}

void TLogBuf::Open(const TPath &path, const unsigned int mode) {
    if (!path.DirName().CanWrite(TCred::Current())) {
        logBufFd = open("/dev/kmsg", O_WRONLY | O_APPEND | O_CLOEXEC);
        if (logBufFd < 0)
            logBufFd = STDERR_FILENO;

        return;
    }

    if (path.GetMode() != mode)
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
    if (n && DieOnNl)
        abort();
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

std::basic_ostream<char> &TLogger::Log(ELogLevel level) {
    PrepareLog();

    static const std::string prefix[] = { "    ",
                                          "WRN ",
                                          "ERR ",
                                          "EVT ",
                                          "ACT ",
                                          "REQ ",
                                          "RSP ",
                                          "SYS " };
    std::string name = GetProcessName();

    if (Statistics) {
        if (level == LOG_WARN)
            Statistics->Warns++;
        else if (level == LOG_ERROR)
            Statistics->Errors++;

        if (level == LOG_ERROR && config().daemon().debug())
            logBuf->DieOnNl = true;
    }

    return (*logStream) << GetTime() << " " << name << "[" << GetTid() << "]: " << prefix[level];
}

void porto_assert(const char *msg, size_t line, const char *file) {
    L_ERR() << "Assertion failed: " << msg << " at " << file << ":" << line << std::endl;
    Crash();
}

void porto_runtime_error(const std::string &msg, size_t line, const char *file) {
    L_ERR() << "Runtime error: " << msg << " at " << file << ":" << line << std::endl;
    Crash();
}
