#include <iostream>
#include <vector>

#include "porto.hpp"
#include "util/unix.hpp"

extern "C" {
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <fcntl.h>
}

using namespace std;

static basic_ostream<char> &Log() {
    char tmstr[256];
    time_t t;
    struct tm *tmp;
    t = time(NULL);
    tmp = localtime(&t);

    if (tmp && strftime(tmstr, sizeof(tmstr), "%c", tmp))
        cerr << tmstr << " ";

    cerr << program_invocation_short_name << ": ";
    return cerr;
}

static void SendPidStatus(int fd, int pid, int status) {
    Log() << "Deliver " << pid << " status " << status << endl;

    if (write(fd, &pid, sizeof(pid)) < 0)
        Log() << "write(pid): " << strerror(errno) << endl;
    if (write(fd, &status, sizeof(status)) < 0)
        Log() << "write(status): " << strerror(errno) << endl;
}

static pid_t portod_pid;
static volatile sig_atomic_t Done = false;
static volatile sig_atomic_t NeedUpdate = false;

static void DoExitAndCleanup(int signum)
{
    Done = true;
}

static void DoUpdate(int signum)
{
    NeedUpdate = true;
}

static int SpawnPortod() {
    int pfd[2];
    int ret = EXIT_FAILURE;
    if (pipe(pfd) < 0) {
        Log() << "pipe(): " << strerror(errno) << endl;
        return EXIT_FAILURE;
    }

    portod_pid = fork();
    if (portod_pid < 0) {
        Log() << "fork(): " << strerror(errno) << endl;
        ret = EXIT_FAILURE;
        goto exit;
    } else if (portod_pid == 0) {
        close(pfd[1]);

        ret = execlp("portod", "portod", nullptr);
        goto exit;
    }

    close(pfd[0]);

    Log() << "Spawned portod " << portod_pid << endl;

    while (!Done) {
        if (NeedUpdate) {
            Log() << "Updating" << endl;

            if (kill(portod_pid, SIGKILL) < 0)
                Log() << "Can't send SIGKILL to portod: " << strerror(errno) << endl;
            if (waitpid(portod_pid, NULL, 0) != portod_pid)
                Log() << "Can't wait for portod exit status: " << strerror(errno) << endl;

            close(pfd[1]);

            execl(program_invocation_name, program_invocation_short_name, nullptr);
            Log() << "Can't execl(" << program_invocation_name << ", " << program_invocation_short_name << ", NULL)" << endl;
            return EXIT_FAILURE;
        }

        int status;
        pid_t pid = wait(&status);
        if (errno == EINTR)
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
    if (argc > 1) {
        string name(argv[1]);

        if (name == "-v" || name == "--version") {
            cout << GIT_TAG << " " << GIT_REVISION <<endl;
            return EXIT_FAILURE;
        }
    }
    Log() << "Started" << endl;

    // portod may die while we are writing into communication pipe
    (void)RegisterSignal(SIGPIPE, SIG_IGN);
    (void)RegisterSignal(SIGINT, DoExitAndCleanup);
    (void)RegisterSignal(SIGHUP, DoUpdate);

    if (prctl(PR_SET_CHILD_SUBREAPER, 1) < 0) {
        Log() << "Can't set myself as a subreaper" << endl;
        return EXIT_FAILURE;
    }

    int ret = EXIT_SUCCESS;
    while (!Done) {
        ret = SpawnPortod();
        Log() << "Returned " << ret << endl;
    }

    if (kill(portod_pid, SIGINT) < 0)
        Log() << "Can't send SIGINT to portod" << endl;

    Log() << "Stopped" << endl;

    return ret;
}
