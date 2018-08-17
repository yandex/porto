#define _GNU_SOURCE
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>

#include "version.hpp"

static pid_t target = -1;
int seize = 0;

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

        if (!strcmp(argv[argn], "--seize"))
            seize = 1;
        else if (strcmp(argv[argn], "--wait"))
            continue;

        if (++argn == argc)
            return EXIT_FAILURE;
        target = atoi(argv[argn]);
        if (kill(target, 0))
            return EXIT_FAILURE;
    }

    prctl(PR_SET_DUMPABLE, 0);
    prctl(PR_SET_NAME, "portoinit");

    if (target >= 0) {
        signal(SIGINT, forward);
        signal(SIGQUIT, forward);
        signal(SIGTERM, forward);
    } else {
        signal(SIGCHLD, SIG_IGN);
    }

    if (seize) {
        if (ptrace(PTRACE_SEIZE, target, 0, PTRACE_O_TRACEEXIT))
            return EXIT_FAILURE;
    }

    while (1) {
        int status;
        pid_t pid = wait(&status);
        if (target < 0) {
            if (pid < 0 && errno == ECHILD)
                pause();
        } else {
            if (pid < 0) {
                if (errno == ECHILD)
                    return 0;
                if (errno == EINTR)
                    continue;
                return EXIT_FAILURE;
            } else if (pid == target) {
                if (WIFSTOPPED(status)) {
                    if (status >> 8 == (SIGTRAP | (PTRACE_EVENT_EXIT << 8))) {
                        unsigned long msg;
                        if (ptrace(PTRACE_GETEVENTMSG, target, NULL, &msg))
                            return EXIT_FAILURE;
                        status = msg;
                    } else {
                        ptrace(PTRACE_CONT, target, 0, WSTOPSIG(status));
                        continue;
                    }
                }
                if (WIFEXITED(status))
                    return WEXITSTATUS(status);
                if (WIFSIGNALED(status)) {
                    if (WCOREDUMP(status))
                        return 128 + SIGRTMIN + WTERMSIG(status);
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
