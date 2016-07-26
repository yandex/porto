#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/prctl.h>

#include "version.hpp"

static pid_t target = -1;

static void forward(int sig) {
    kill(target, sig);
    signal(sig, SIG_DFL);
}

int main(int argc, char *argv[]) {
    int argn;

    for (argn = 1; argn < argc; argn++) {
        if (!strcmp(argv[argn], "-v") || !strcmp(argv[argn], "--version")) {
            printf("%s %s\n", PORTO_VERSION, PORTO_REVISION);
            return EXIT_SUCCESS;
        }

        if (!strcmp(argv[argn], "--wait")) {
            if (++argn == argc)
                return EXIT_FAILURE;
            target = atoi(argv[argn]);
            if (kill(target, 0))
                return EXIT_FAILURE;
        }
    }

    prctl(PR_SET_NAME, "portoinit");

    if (target >= 0) {
        signal(SIGINT, forward);
        signal(SIGQUIT, forward);
        signal(SIGTERM, forward);
    }

    while (1) {
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
