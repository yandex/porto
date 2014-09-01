#include "porto.hpp"
#include "log.hpp"

extern "C" {
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
}

static std::ofstream file;

void TLogger::OpenLog(const std::string &path, const unsigned int mode) {
    if (file.is_open())
        file.close();

    struct stat st;
    bool needCreate = false;

    if (lstat(path.c_str(), &st) == 0) {
        if (st.st_mode != (mode | S_IFREG)) {
            unlink(path.c_str());
            needCreate = true;
        }
    } else {
        needCreate = true;
    }

    if (needCreate)
        close(creat(path.c_str(), mode));

    file.open(path, std::ios_base::app);
}

void TLogger::CloseLog() {
    file.close();
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
    return file << GetTime() << " ";
}

void TLogger::Log(const std::string &action) {
    if (!LOG_VEBOSE)
        return;
    
    Log() << " " << action << std::endl;
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
