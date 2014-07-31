#ifndef __TASK_HPP__
#define __TASK_HPP__

#include <mutex>
#include <string>
#include <vector>
#include <cstdint>

using namespace std;

class TCgroup;

struct TExitStatus {
    // Task was not started due to the following error
    int error;
    // Task is terminated by given signal
    int signal;
    // Task exited normally with given status
    int status;
};

class TTask {
    mutex lock;
    enum ETaskState { Stopped, Running } state;
    string path;
    vector<string> args;
    TExitStatus exitStatus;

    pid_t pid;

    vector<TCgroup*> cgroups;

    int CloseAllFds(int except);
    const char** GetArgv();
public:
    TTask(string &path, vector<string> &args);

    void FindCgroups();

    bool Start();
    int GetPid();
    bool IsRunning();
    TExitStatus GetExitStatus();
    void Kill();
};

#endif
