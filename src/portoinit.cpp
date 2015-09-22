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
#include <sys/prctl.h>
}

pid_t target = -1;

static void ForwardSignal(int signum) {
    kill(target, signum);
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

        if (arg == "--wait") {
            argn++;
            if (argn == argc)
                return EXIT_FAILURE;
            target = atoi(argv[argn]);
            if (kill(target, 0))
                return EXIT_FAILURE;
        }
    }

    prctl(PR_SET_NAME, "portoinit");
    RegisterSignal(SIGTERM, ForwardSignal);
    RegisterSignal(SIGINT, ForwardSignal);
    RegisterSignal(SIGQUIT, ForwardSignal);

    while (true) {
        int status;
        pid_t pid = wait(&status);
        if (target < 0) {
            if (pid < 0 && errno == ECHILD)
                sleep(5 * 60);
        } else {
            if (pid < 0) {
                if (errno == ECHILD)
                    return 0;
                if (errno == EINTR)
                    continue;
                return EXIT_FAILURE;
            } else if (pid == target) {
                if (WIFEXITED(status))
                    return WEXITSTATUS(status);
                if (WIFSIGNALED(status)) {
                    signal(WTERMSIG(status), SIG_DFL);
                    kill(getpid(), WTERMSIG(status));
                    return 128 + WTERMSIG(status);
                }
                return EXIT_FAILURE;
            }
        }
    }

    return EXIT_SUCCESS;
}
