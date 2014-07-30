#include <iostream>

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
};
