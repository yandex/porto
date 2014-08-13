#include "log.hpp"

extern "C" {
#include <string.h>
#include <errno.h>
}

bool verbose = true;

void TLogger::LogAction(const std::string &action, bool error, int errcode) {
    if (!error && verbose)
        std::cerr << "Ok: " << action << std::endl;
    else if (error)
        std::cerr << "Error: " << action << ": " << strerror(errcode) << std::endl;
}

void TLogger::LogError(const TError &e, const std::string &s) {
    if (e)
        std::cerr << "Error(" << e.GetError() << "): " << e.GetMsg() << " - " << s << std::endl;
}

void TLogger::LogRequest(const std::string &message) {
    std::cerr << "-> " << message << std::endl;
}

void TLogger::LogResponse(const std::string &message) {
    std::cerr << "<- " << message << std::endl;
}
