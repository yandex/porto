#ifndef __TASK_HPP__
#define __TASK_HPP__

#include <string>
#include <vector>
#include <cstdint>
#include <functional>

extern "C" {
#include <signal.h>
}

using namespace std;

class TTask;
class TContainerEnv;

struct TExitStatus {
    // Task was not started due to the following error
    int error;
    // Task is terminated by given signal
    int signal;
    // Task exited normally with given status
    int status;
};

class TTaskEnv {
    friend TTask;
    std::string path;
    std::vector<std::string> args;
    std::string cwd;
    //std::vector<std::string> env;

public:
    TTaskEnv() {};
    TTaskEnv(const std::string command, const std::string cwd);
};

class TTask {
    TTaskEnv env;
    std::function<void(void)> fork_hook;

    enum ETaskState { Stopped, Running } state;
    TExitStatus exitStatus;

    pid_t pid;

    int CloseAllFds(int except);
    const char** GetArgv();
    void ReportResultAndExit(int fd, int result);

public:
    TTask(TTaskEnv& env, std::function<void(void)> fork_hook) : env(env),
        fork_hook(fork_hook) {};
    TTask(pid_t pid) : pid(pid) {};

    void FindCgroups();

    bool Start();
    int GetPid();
    bool IsRunning();
    TExitStatus GetExitStatus();
    void Kill(int signal);
};

#endif
