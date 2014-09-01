#include <fstream>

#include "porto.hpp"
#include "log.hpp"

extern "C" {
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
}

static std::ofstream File;

void TLogger::OpenLog(const std::string &path, const unsigned int mode) {
    if (File.is_open())
        File.close();

    struct stat st;
    bool need_create = false;

    if (lstat(path.c_str(), &st) == 0) {
        if (st.st_mode != (mode | S_IFREG)) {
            unlink(path.c_str());
            need_create = true;
        }
    } else {
        need_create = true;
    }

    if (need_create)
        close(creat(path.c_str(), mode));

    File.open(path, std::ios_base::app);
}

void TLogger::CloseLog() {
    File.close();
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

void TLogger::Log(const std::string &action) {
    if (!LOG_VEBOSE)
        return;
    
    File << GetTime() << " " << action << std::endl;
}

void TLogger::LogAction(const std::string &action, bool error, int errcode) {
    if (!error && LOG_VEBOSE)
        File << GetTime() << " Ok: " << action << std::endl;
    else if (error)
        File << GetTime() << " Error: " << action << ": " << strerror(errcode) << std::endl;
}

void TLogger::LogError(const TError &e, const std::string &s) {
    if (!e)
        return;

    File << GetTime() << " Error(" << rpc::EError_Name(e.GetError()) << "): " << s << ": " << e.GetMsg() << std::endl;
}

void TLogger::LogRequest(const std::string &message) {
    File << GetTime() << " -> " << message << std::endl;
}

void TLogger::LogResponse(const std::string &message) {
    File << GetTime() << " <- " << message << std::endl;
}
