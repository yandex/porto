#include <string>
#include <iostream>

#include "common.hpp"
#include "version.hpp"
#include "util/signal.hpp"

extern "C" {
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
}

static void ForwardSignal(int signum) {
    kill(-1, signum);
    ResetSignalHandler(signum);
}

int main(int argc, char *argv[]) {
    int argn;

    for (argn = 1; argn < argc; argn++) {
        std::string arg(argv[argn]);

        if (arg == "-v" || arg == "--version") {
            std::cout << GIT_TAG << " " << GIT_REVISION <<std::endl;
            return EXIT_SUCCESS;
        }
    }

    RegisterSignal(SIGTERM, ForwardSignal);
    RegisterSignal(SIGINT, ForwardSignal);
    RegisterSignal(SIGQUIT, ForwardSignal);

    while (true) {
        int ret = wait(NULL);
        if (ret < 0 && errno == ECHILD)
            sleep(5 * 60);
    }

    return EXIT_SUCCESS;
}
