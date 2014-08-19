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

    (void)unlink(path.c_str());
    (void)creat(path.c_str(), mode);

    file.open(path);
}

void TLogger::CloseLog() {
    file.close();
}

void TLogger::Log(const std::string &action) {
    if (verbose) {
        std::cerr << action << std::endl;

        if (file.is_open())
            file << action << std::endl;
    }
}

void TLogger::LogAction(const std::string &action, bool error, int errcode) {
    if (!error && verbose) {
        std::cerr << "Ok: " << action << std::endl;
        if (file.is_open())
            file << "Ok: " << action << std::endl;
    } else if (error) {
        std::cerr << "Error: " << action << ": " << strerror(errcode) << std::endl;
        if (file.is_open())
            file << "Error: " << action << ": " << strerror(errcode) << std::endl;
    }
}

void TLogger::LogError(const TError &e, const std::string &s) {
    if (!e)
        return;

    std::cerr << "Error(" << rpc::EError_Name(e.GetError()) << "): " << s << ": " << e.GetMsg() << std::endl;
    if (file.is_open())
        file << "Error(" << rpc::EError_Name(e.GetError()) << "): " << s << ": " << e.GetMsg() << std::endl;
}

void TLogger::LogRequest(const std::string &message) {
    std::cerr << "-> " << message << std::endl;
    if (file.is_open())
        file << "-> " << message << std::endl;
}

void TLogger::LogResponse(const std::string &message) {
    std::cerr << "<- " << message << std::endl;
    if (file.is_open())
        file << "<- " << message << std::endl;
}
