#ifndef __LOG_HPP__
#define __LOG_HPP__

#include <iostream>
#include <string>

#include "error.hpp"

extern "C" {
#include "string.h"
#include "errno.h"
}

static bool verbose = true;

class TLogger {
public:
    static void LogAction(const std::string &action, bool error = false, int errcode = 0) {
        if (!error && verbose)
            std::cerr << "Ok: " << action << std::endl;
        else if (error)
            std::cerr << "Error: " << action << ": " << strerror(errcode) << std::endl;
    }

    static void LogError(const TError &e, const std::string &s = "") {
        std::cerr << "Error(" << e.GetError() << "): " << e.GetMsg() << " - " << s << std::endl;
    }

    static void LogRequest(const std::string &message) {
        std::cerr << "-> " << message << std::endl;
    }
    static void LogResponse(const std::string &message) {
        std::cerr << "<- " << message << std::endl;
    }
};

#endif /* __LOG_HPP__ */
