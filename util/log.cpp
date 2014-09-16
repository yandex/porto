#include <iostream>

#include "porto.hpp"
#include "log.hpp"
#include "util/unix.hpp"

extern "C" {
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
}

static std::ofstream logFile;
static std::ofstream kmsgFile;
static std::string logPath;
static unsigned int logMode;
static bool stdlog = false;

void TLogger::InitLog(const std::string &path, const unsigned int mode) {
    logPath = path;
    logMode = mode;
    logFile.close();
}

void TLogger::LogToStd() {
    stdlog = true;
}

void TLogger::OpenLog() {
    if (logFile.is_open()) {
        return;
    }

    if (access(DirName(logPath).c_str(), W_OK)) {
        if (!kmsgFile.is_open())
            kmsgFile.open("/dev/kmsg", std::ios_base::out);
        return;
    }

    struct stat st;
    bool needCreate = false;

    if (lstat(logPath.c_str(), &st) == 0) {
        if (st.st_mode != (logMode | S_IFREG)) {
            unlink(logPath.c_str());
            needCreate = true;
        }
    } else {
        needCreate = true;
    }

    if (needCreate)
        close(creat(logPath.c_str(), logMode));

    logFile.open(logPath, std::ios_base::app);

    if (logFile.is_open() && kmsgFile.is_open())
        kmsgFile.close();
}

void TLogger::CloseLog() {
    logFile.close();
    kmsgFile.close();
}

void TLogger::TruncateLog() {
    TLogger::CloseLog();
    (void)truncate(logPath.c_str(), 0);
}

static std::string GetTime() {
    char tmstr[256];
    time_t t;
    struct tm *tmp;
    t = time(NULL);
    tmp = localtime(&t);

    if (tmp && strftime(tmstr, sizeof(tmstr), "%c", tmp))
        return std::string(tmstr);

    return std::string();
}

std::basic_ostream<char> &TLogger::Log() {
    if (stdlog)
        return std::cerr << GetTime() << " " << program_invocation_short_name << ": ";

    OpenLog();

    if (logFile.is_open())
        return logFile << GetTime() << " ";
    else if (kmsgFile.is_open())
        return kmsgFile << " " << program_invocation_short_name << ": ";
    else
        return std::cerr << GetTime() << " " << program_invocation_short_name << ": ";
}

void TLogger::LogAction(const std::string &action, bool error, int errcode) {
    if (!error && LOG_VEBOSE)
        Log() << " Ok: " << action << std::endl;
    else if (error)
        Log() << " Error: " << action << ": " << strerror(errcode) << std::endl;
}

void TLogger::LogRequest(const std::string &message) {
    Log() << " -> " << message << std::endl;
}

void TLogger::LogResponse(const std::string &message) {
    Log() << " <- " << message << std::endl;
}
