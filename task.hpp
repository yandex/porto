#ifndef __TASK_HPP__
#define __TASK_HPP__

#include <string>
#include <vector>
#include <cstdint>

#include "cgroup.hpp"

class TTask;
class TContainerEnv;

struct TExitStatus {
    // Task was not started due to the following error
    int Error;
    // Task exited with given status
    int Status;
};

class TTaskEnv {
    friend TTask;
    std::string Command;
    std::string Cwd;
    std::string Root;
    std::vector<std::string> Env;
    std::string User;
    std::string Group;
    std::string Envir;
    int Uid, Gid;

public:
    TTaskEnv() {};
    TTaskEnv(const std::string &command, const std::string &cwd, const std::string &root, const std::string &user, const std::string &group, const std::string &envir) : Command(command), Cwd(cwd), Root(root), User(user), Group(group), Envir(envir) { }
    TError Prepare();
    const char** GetEnvp();
};

class TTask {
    int Rfd, Wfd;
    TTaskEnv Env;
    std::vector<std::shared_ptr<TCgroup>> LeafCgroups;

    enum ETaskState { Stopped, Started } State;
    TExitStatus ExitStatus;

    pid_t Pid;
    std::string StdoutFile;
    std::string StderrFile;

    int CloseAllFds(int except);
    void Syslog(const std::string &s);
    void ReportResultAndExit(int fd, int result);

    TError RotateFile(const std::string path);
public:
    TTask(TTaskEnv& env, std::vector<std::shared_ptr<TCgroup>> &leafCgroups) : Env(env), LeafCgroups(leafCgroups) {};
    TTask(pid_t pid) : Pid(pid) {};
    ~TTask();

    TError Start();
    int GetPid();
    bool IsRunning();
    TExitStatus GetExitStatus();
    void Kill(int signal);
    void DeliverExitStatus(int status);

    std::string GetStdout();
    std::string GetStderr();

    int ChildCallback();
    TError Restore(int pid);
    TError ValidateCgroups();
    TError Rotate();
};

#endif
