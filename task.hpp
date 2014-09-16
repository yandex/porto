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
    std::vector<std::string> EnvVec;
    std::string User;
    std::string Group;
    std::string Environ;
    int Uid, Gid;

    void ParseEnv();
public:
    TTaskEnv() {};
    TTaskEnv(const std::string &command, const std::string &cwd, const std::string &root, const std::string &user, const std::string &group, const std::string &environ) : Command(command), Cwd(cwd), Root(root), User(user), Group(group), Environ(environ) { }
    TError Prepare();
    const char** GetEnvp() const;
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

    int CloseAllFds(int except) const;
    void Syslog(const std::string &s) const;
    void ReportResultAndExit(int fd, int result) const;

    TError RotateFile(const std::string path) const;

    TTask(const TTask &) = delete;
    TTask &operator=(const TTask &) = delete;
public:
    TTask(TTaskEnv& env, std::vector<std::shared_ptr<TCgroup>> &leafCgroups) : Env(env), LeafCgroups(leafCgroups) {};
    TTask(pid_t pid) : Pid(pid) {};
    ~TTask();

    TError Start();
    int GetPid() const;
    bool IsRunning() const;
    TExitStatus GetExitStatus() const;
    void Kill(int signal) const;
    void DeliverExitStatus(int status);

    std::string GetStdout() const;
    std::string GetStderr() const;

    int ChildCallback();
    TError Restore(int pid);
    TError ValidateCgroups() const;
    TError Rotate() const;
};

#endif
