#include <vector>
#include <map>
#include <iostream>

#include "porto.hpp"
#include "util/log.hpp"
#include "util/unix.hpp"

extern "C" {
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <fcntl.h>
#include <poll.h>
}

using namespace std;

static void SendPidStatus(int fd, int pid, int status, size_t queued) {
    TLogger::Log() << "Deliver " << pid << " status " << status << " (" + to_string(queued) + " queued)" << endl;

    if (write(fd, &pid, sizeof(pid)) < 0)
        TLogger::Log() << "write(pid): " << strerror(errno) << endl;
    if (write(fd, &status, sizeof(status)) < 0)
        TLogger::Log() << "write(status): " << strerror(errno) << endl;
}

static pid_t portoPid;
static volatile sig_atomic_t done = false;
static volatile sig_atomic_t needUpdate = false;
static volatile sig_atomic_t gotAlarm = false;

static void DoExitAndCleanup(int signum) {
    done = true;
}

static void DoUpdate(int signum) {
    needUpdate = true;
}

static void DoAlarm(int signum) {
    // we just need to interrupt poll
    gotAlarm = true;
}

static void ReceiveAcks(int fd, map<int,int> &pidToStatus) {
    int pid;

    while (read(fd, &pid, sizeof(pid)) == sizeof(pid)) {
        TLogger::Log() << "Got acknowledge for " << pid << endl;
        pidToStatus.erase(pid);
    }
}

static void SignalMask(int how) {
    sigset_t mask;
    int sigs[] = { SIGALRM };


    if (sigemptyset(&mask) < 0) {
        TLogger::Log() << "Can't initialize signal mask: " << strerror(errno) << endl;
        return;
    }

    for (auto sig: sigs)
        if (sigaddset(&mask, sig) < 0) {
            TLogger::Log() << "Can't add signal to mask: " << strerror(errno) << endl;
            return;
        }


    if (sigprocmask(how, &mask, NULL) < 0)
        TLogger::Log() << "Can't set signal mask: " << strerror(errno) << endl;
}

static int SpawnPortod(map<int,int> &pidToStatus) {
    int evtfd[2];
    int ackfd[2];
    int ret = EXIT_FAILURE;

    if (pipe(evtfd) < 0) {
        TLogger::Log() << "pipe(): " << strerror(errno) << endl;
        return EXIT_FAILURE;
    }

    if (pipe2(ackfd, O_NONBLOCK) < 0) {
        TLogger::Log() << "pipe(): " << strerror(errno) << endl;
        return EXIT_FAILURE;
    }

    portoPid = fork();
    if (portoPid < 0) {
        TLogger::Log() << "fork(): " << strerror(errno) << endl;
        ret = EXIT_FAILURE;
        goto exit;
    } else if (portoPid == 0) {
        close(evtfd[1]);
        close(ackfd[0]);

        ret = execlp("portod", "portod", nullptr);
        TLogger::Log() << "execlp(): " << strerror(errno) << endl;
        goto exit;
    }

    close(evtfd[0]);
    close(ackfd[1]);

    TLogger::Log() << "Spawned portod " << portoPid << endl;

    for (auto &pair : pidToStatus)
        SendPidStatus(evtfd[1], pair.first, pair.second, pidToStatus.size());

    SignalMask(SIG_BLOCK);

    while (!done) {
        int pid;

        ReceiveAcks(ackfd[0], pidToStatus);

        if (needUpdate) {
            TLogger::Log() << "Updating" << endl;

            if (kill(portoPid, SIGKILL) < 0)
                TLogger::Log() << "Can't send SIGKILL to portod: " << strerror(errno) << endl;
            if (waitpid(portoPid, NULL, 0) != portoPid)
                TLogger::Log() << "Can't wait for portod exit status: " << strerror(errno) << endl;

            close(evtfd[1]);
            close(ackfd[0]);

            TLogger::CloseLog();
            execlp(program_invocation_name, program_invocation_short_name, nullptr);
            TLogger::OpenLog(LOOP_LOG_FILE, LOOP_LOG_FILE_PERM);
            TLogger::Log() << "Can't execlp(" << program_invocation_name << ", " << program_invocation_short_name << ", NULL)" << endl;
            TLogger::CloseLog();
            ret = EXIT_FAILURE;
            break;
        }

        (void)alarm(LOOP_WAIT_TIMEOUT_S);

        SignalMask(SIG_UNBLOCK);
        int status;
        pid = wait(&status);
        SignalMask(SIG_BLOCK);

        if (gotAlarm) {
            gotAlarm = false;
            continue;
        }

        if (errno == EINTR) {
            TLogger::Log() << "wait(): " << strerror(errno) << endl;
            continue;
        }
        if (pid == portoPid) {
            TLogger::Log() << "portod exited with " << status << endl;
            ret = EXIT_SUCCESS;
            break;
        }

        SendPidStatus(evtfd[1], pid, status, pidToStatus.size());
        pidToStatus[pid] = status;
    }

    SignalMask(SIG_UNBLOCK);

    ReceiveAcks(ackfd[0], pidToStatus);

exit:
    close(evtfd[0]);
    close(evtfd[1]);

    close(ackfd[0]);
    close(ackfd[1]);

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

    TLogger::OpenLog(LOOP_LOG_FILE, LOOP_LOG_FILE_PERM);

    if (getuid() != 0) {
        TLogger::Log() << "Need root privileges to start" << endl;
        return EXIT_FAILURE;
    }

    TLogger::Log() << "Started" << endl;

    // portod may die while we are writing into communication pipe
    (void)RegisterSignal(SIGPIPE, SIG_IGN);
    (void)RegisterSignal(SIGINT, DoExitAndCleanup);
    (void)RegisterSignal(SIGHUP, DoUpdate);
    (void)RegisterSignal(SIGALRM, DoAlarm);

    SignalMask(SIG_UNBLOCK);

    if (prctl(PR_SET_CHILD_SUBREAPER, 1) < 0) {
        TLogger::Log() << "Can't set myself as a subreaper" << endl;
        return EXIT_FAILURE;
    }

    int ret = EXIT_SUCCESS;
    map<int,int> pidToStatus;

    while (!done) {
        ret = SpawnPortod(pidToStatus);
        TLogger::Log() << "Returned " << ret << endl;
        if (!done && ret != EXIT_SUCCESS)
            usleep(1000000);
    }

    if (kill(portoPid, SIGINT) < 0)
        TLogger::Log() << "Can't send SIGINT to portod" << endl;

    TLogger::Log() << "Stopped" << endl;

    TLogger::CloseLog();

    return ret;
}
