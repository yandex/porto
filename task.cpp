#include <fstream>
#include <climits>

#include "task.hpp"
#include "cgroup.hpp"

extern "C" {
#include <sys/types.h>
#include <sys/wait.h>
}

TTask::TTask(string &path, vector<string> &args) : path(path), args(args)
{
    pid_t pid = fork();
    if (pid < 0) {
        throw "Fork error";
    } else if (pid == 0) {
        // TODO: move to cgroups

        setpgrp();

        for (int i = 3; i < getdtablesize(); i++)
            close(i);

        auto argv = new const char* [args.size() + 2];
        argv[0] = path.c_str();
        for (size_t i = 0; i < args.size(); i++)
            argv[i + 1] = args[i].c_str();
        argv[args.size() + 1] = NULL;

        if (execvp(path.c_str(), (char *const *)argv)) {
            delete[] argv;
            throw "Execl error";
        }
    } else {
        this->pid = pid;
    }

    state = Running;
}

void TTask::FindCgroups()
{
#if 0
    ifstream in("proc/self/cgroup");

    if (!in.is_open())
        throw "Cannot open /proc/self/cgroup";

    while (getline(in, line)) {
        // new TCgroup
    }
#endif
}

int TTask::GetPid()
{
    lock_guard<mutex> guard(lock);

    return pid;
}

bool TTask::IsRunning()
{
    GetExitStatus();

    lock_guard<mutex> guard(lock);
    return state == Running;
}

int TTask::GetExitStatus()
{
    lock_guard<mutex> guard(lock);

    if (state != Stopped) {
        int status;
        pid_t ret;
        ret = waitpid(pid, &status, WNOHANG | WUNTRACED | WCONTINUED);
        if (!ret)
            return INT_MAX;

        exit_status = status;
        state = Stopped;
    }

    return exit_status;
}

void TTask::Kill()
{
    lock_guard<mutex> guard(lock);

    int ret = kill(pid, SIGTERM);
    if (ret == ESRCH)
        return;

    // TODO: add some sleep before killing with -9 ?

    kill(pid, SIGKILL);
}
