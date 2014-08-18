#include <iostream>
#include <vector>

extern "C" {
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/prctl.h>
}

using namespace std;

const string PREFIX = "loop: ";

static void SendPidStatus(int fd, int pid, int status) {
    cerr << PREFIX << pid << " finished with " << status << endl;

    if (write(fd, &pid, sizeof(pid)) < 0)
        cerr << PREFIX << "write(pid): " << strerror(errno);
    if (write(fd, &status, sizeof(status)) < 0)
        cerr << PREFIX << "write(status): " << strerror(errno);
}

static pid_t portod_pid;
static volatile sig_atomic_t Done = false;

static void DoExitAndCleanup(int signum)
{
    Done = true;
    (void)kill(portod_pid, SIGINT);
}

static int SpawnPortod() {
    int pfd[2];
    int ret = EXIT_FAILURE;
    if (pipe(pfd) < 0) {
        cerr << PREFIX << "pipe(): " << strerror(errno);
        return EXIT_FAILURE;
    }

    portod_pid = fork();
    if (portod_pid < 0) {
        cerr << PREFIX << "fork(): " << strerror(errno);
        ret = EXIT_FAILURE;
        goto exit;
    } else if (portod_pid == 0) {
        close(pfd[1]);

        ret = execlp("portod", "portod", nullptr);
        goto exit;
    }

    close(pfd[0]);

    cerr << PREFIX << "Spawned portod " << portod_pid << endl;

    while (!Done) {
        int status;
        pid_t pid = wait(&status);
        if (pid == EINTR)
            continue;
        if (pid == portod_pid)
            break;

        SendPidStatus(pfd[1], pid, status);
    }

exit:
    close(pfd[0]);
    close(pfd[1]);

    return ret;
}

int main(int argc, char * const argv[])
{
    // portod may die while we are writing into communication pipe
    signal(SIGPIPE, SIG_IGN);

    signal(SIGINT, DoExitAndCleanup);

    if (prctl(PR_SET_CHILD_SUBREAPER, 1) < 0) {
        cerr << PREFIX << "Can't set myself as a subreaper" << endl;
        return EXIT_FAILURE;
    }

    while (!Done)
        cerr << PREFIX << "Returned " << SpawnPortod() << endl;

    return EXIT_SUCCESS;
}
