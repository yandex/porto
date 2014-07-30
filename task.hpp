#ifndef __TASK_HPP__
#define __TASK_HPP__

#include <mutex>
#include <string>
#include <vector>

using namespace std;

class TCgroup;

class TTask {
    mutex lock;
    enum ETaskState { Stopped, Running } state;
    string path;
    vector<string> args;
    int exit_status;

    pid_t pid;

    vector<TCgroup*> cgroups;

public:
    TTask(string &path, vector<string> &args);

    void FindCgroups();

    int GetPid();
    bool IsRunning();
    int GetExitStatus();
    void Kill();
};

#endif
