#include <fstream>
#include "log.hpp"

extern "C" {
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
}

bool verbose = true;

static std::ofstream file;

void TLogger::OpenLog(const std::string &path, const int mode) {
    if (file.is_open())
        file.close();

    (void)creat(path.c_str(), mode);
    if (truncate(path.c_str(), 0)) {}

    file.open(path);
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

void TLogger::Log(const std::string &action) {
    if (verbose) {
        if (file.is_open())
            file << GetTime() << " " << action << std::endl;
        else
            std::cerr << GetTime() << " " << action << std::endl;
    }
}

void TLogger::LogAction(const std::string &action, bool error, int errcode) {
    if (!error && verbose) {
        if (file.is_open())
            file << GetTime() << " Ok: " << action << std::endl;
        else
            std::cerr << GetTime() << " Ok: " << action << std::endl;
    } else if (error) {
        if (file.is_open())
            file << GetTime() << " Error: " << action << ": " << strerror(errcode) << std::endl;
        else
            std::cerr << GetTime() << " Error: " << action << ": " << strerror(errcode) << std::endl;
    }
}

void TLogger::LogError(const TError &e, const std::string &s) {
    if (!e)
        return;

    if (file.is_open())
        file << GetTime() << " Error(" << rpc::EError_Name(e.GetError()) << "): " << s << ": " << e.GetMsg() << std::endl;
    else
        std::cerr << GetTime() << " Error(" << rpc::EError_Name(e.GetError()) << "): " << s << ": " << e.GetMsg() << std::endl;
}

void TLogger::LogRequest(const std::string &message) {
    if (file.is_open())
        file << GetTime() << " -> " << message << std::endl;
    else
        std::cerr << GetTime() << " -> " << message << std::endl;
}

void TLogger::LogResponse(const std::string &message) {
    if (file.is_open())
        file << GetTime() << " <- " << message << std::endl;
    else
        std::cerr << GetTime() << " <- " << message << std::endl;
}
