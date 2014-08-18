#ifndef __TASK_HPP__
#define __TASK_HPP__

#include <string>
#include <vector>
#include <cstdint>

extern "C" {
#include <signal.h>
#include <grp.h>
}

#include "cgroup.hpp"

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
    std::string command;
    std::vector<std::string> args;
    std::string cwd;
    std::string root;
    std::vector<std::string> env;
    std::string user;
    std::string group;
    std::string envir;
    int uid, gid;

public:
    TTaskEnv() {};
    TTaskEnv(const std::string &command, const std::string &cwd, const std::string &root, const std::string &user, const std::string &group, const std::string &envir) : command(command), cwd(cwd), root(root), user(user), group(group), envir(envir) { }
    TError Prepare();
    const char** GetEnvp();
};

class TTask {
    int rfd, wfd;
    TTaskEnv env;
    std::vector<std::shared_ptr<TCgroup>> leaf_cgroups;

    enum ETaskState { Stopped, Started } state;
    TExitStatus exitStatus;

    pid_t pid;
    std::string stdoutFile;
    std::string stderrFile;

    pid_t fwdpid;

    int CloseAllFds(int except);
    void Syslog(const std::string &s);
    void ReportResultAndExit(int fd, int result);

public:
    TTask(TTaskEnv& env, std::vector<std::shared_ptr<TCgroup>> &leaf_cgroups) : env(env), leaf_cgroups(leaf_cgroups) {};
    TTask(pid_t pid) : pid(pid) {};
    ~TTask();

    void FindCgroups();

    TError Start();
    int GetPid();
    bool IsRunning();
    TExitStatus GetExitStatus();
    void Kill(int signal);
    TError Reap(bool wait);

    std::string GetStdout();
    std::string GetStderr();

    int ChildCallback();
    TError Seize(int pid);
    TError ValidateCgroups();
};

#endif
