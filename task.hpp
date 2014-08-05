#ifndef __TASK_HPP__
#define __TASK_HPP__

#include <string>
#include <vector>
#include <cstdint>

using namespace std;

class TContainerEnv;

struct TExitStatus {
    // Task was not started due to the following error
    int error;
    // Task is terminated by given signal
    int signal;
    // Task exited normally with given status
    int status;
};

class TTask {
    enum ETaskState { Stopped, Running } state;
    TContainerEnv *env;
    TExitStatus exitStatus;

    pid_t pid;

    int CloseAllFds(int except);
    const char** GetArgv();
    void ReportResultAndExit(int fd, int result);
public:
    TTask(TContainerEnv *env);

    void FindCgroups();

    bool Start();
    int GetPid();
    bool IsRunning();
    TExitStatus GetExitStatus();
    void Kill();
};

#endif
