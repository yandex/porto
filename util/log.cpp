#include <iostream>
#include <sstream>

#include "log.hpp"
#include "util/unix.hpp"
#include "util/file.hpp"

extern "C" {
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
}

static TLogBuf logBuf(1024);
static std::ostream logStream(&logBuf);

void TLogger::OpenLog(bool std, const TPath &path, const unsigned int mode) {
    if (std) {
        // because in task.cpp we expect that nothing should be in 0-2 fd,
        // we need to duplicate our std log somewhere else
        logBuf.SetFd(dup(STDOUT_FILENO));
    } else {
        logBuf.Open(path, mode);
    }
}

void TLogger::DisableLog() {
    TLogger::CloseLog();
    logBuf.SetFd(-1);
}

int TLogger::GetFd() {
    return logBuf.GetFd();
}

void TLogger::CloseLog() {
    int fd = logBuf.GetFd();
    if (fd > 2)
        close(fd);
    logBuf.SetFd(STDOUT_FILENO);
}

static std::string GetTime() {
    char tmstr[256];
    time_t t;
    struct tm *tmp;
    t = time(NULL);
    tmp = localtime(&t);

    if (tmp && strftime(tmstr, sizeof(tmstr), "%F %T", tmp))
        return std::string(tmstr);

    return std::string();
}

TLogBuf::TLogBuf(const size_t size) {
    Data.reserve(size);
    char *base = static_cast<char *>(Data.data());
    setp(base, base + Data.capacity() - 1);
}

void TLogBuf::Open(const TPath &path, const unsigned int mode) {
    if (!path.DirName().AccessOk(EFileAccess::Write)) {
        Fd = open("/dev/kmsg", O_WRONLY | O_APPEND | O_CLOEXEC);
        if (Fd < 0)
            Fd = STDERR_FILENO;

        return;
    }

    bool needCreate = false;

    if (path.Exists()) {
        if (path.GetType() != EFileType::Regular ||
            path.GetMode() != mode) {

            TFile f(path);
            (void)f.Remove();
            needCreate = true;
        }
    } else {
        needCreate = true;
    }

    if (needCreate) {
        TFile f(path, mode);
        (void)f.Touch();
    }

    Fd = open(path.ToString().c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
    if (Fd < 0)
        Fd = STDERR_FILENO;
}

int TLogBuf::sync() {
    std::ptrdiff_t n = pptr() - pbase();
    pbump(-n);

    int ret = write(Fd, pbase(), n);
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
    static const std::string prefix[] = { "", "Warning! ", "Error! " };
    std::string name = GetProcessName();

#ifdef PORTOD
    if (level == LOG_WARN)
        Statistics->Warns++;
    else if (level == LOG_ERROR)
        Statistics->Errors++;
#endif

    return logStream << GetTime() << " " << name << ": " << prefix[level];
}
