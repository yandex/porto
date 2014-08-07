#ifndef __LOG_HPP__
#define __LOG_HPP__

#include <iostream>

#include "error.hpp"

#include "string.h"
#include "errno.h"

using namespace std;

static bool verbose = true;

class TLogger {
public:
    static void LogAction(string action, bool error = false, int errcode = 0) {
        if (!error && verbose)
            cerr << "Ok: " << action << endl;
        else if (error)
            cerr << "Error: " << action << ": " << strerror(errcode) << endl;
    }

    static void LogError(const TError &e, const string &s = "") {
        cerr << "Error(" << e.GetError() << "): " << e.GetMsg() << " - " << s << endl;
    }

    static void LogRequest(string message) {
        cerr << "-> " << message << endl;
    }
    static void LogResponse(string message) {
        cerr << "<- " << message << endl;
    }
};

#endif /* __LOG_HPP__ */
