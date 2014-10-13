#ifndef __TASK_HPP__
#define __TASK_HPP__

#include <string>
#include <vector>
#include <cstdint>

#include "cgroup.hpp"
#include "util/namespace.hpp"

extern "C" {
#include <sys/resource.h>
}

class TTask;
class TContainerEnv;

struct TExitStatus {
    // Task was not started due to the following error
    int Error;
    // Task exited with given status
    int Status;
};

class TTaskEnv {
    NO_COPY_CONSTRUCT(TTaskEnv);
    friend TTask;
    std::vector<std::string> EnvVec;
    int Uid, Gid;

    void ParseEnv();
public:
    TTaskEnv() {}
    std::string Command;
    std::string Cwd;
    bool CreateCwd;
    std::string Root;
    std::string User;
    std::string Group;
    std::string Environ;
    bool Isolate = false;
    std::string StdinPath;
    std::string StdoutPath;
    std::string StderrPath;
    TNamespaceSnapshot Ns;
    std::map<int,struct rlimit> Rlimit;

    TError Prepare();
    const char** GetEnvp() const;
};

class TTask {
    NO_COPY_CONSTRUCT(TTask);
    int Rfd, Wfd;
    std::shared_ptr<TTaskEnv> Env;
    std::vector<std::shared_ptr<TCgroup>> LeafCgroups;

    enum ETaskState { Stopped, Started } State;
    int ExitStatus;

    pid_t Pid;
    std::shared_ptr<TFolder> Cwd;

    int CloseAllFds(int except) const;
    void Syslog(const std::string &s) const;
    void ReportResultAndExit(int fd, int result) const;

    TError RotateFile(const std::string path) const;
    TError CreateCwd();
    void OpenStdFile(const std::string &path, int expected);
    void ChildReopenStdio();
    void ChildDropPriveleges();
    void ChildExec();
    void BindDns();
    void ChildIsolateFs();

public:
    TTask(std::shared_ptr<TTaskEnv> env, std::vector<std::shared_ptr<TCgroup>> &leafCgroups) : Env(env), LeafCgroups(leafCgroups) {};
    TTask(pid_t pid) : Pid(pid) {};
    ~TTask();

    TError Start();
    int GetPid() const;
    bool IsRunning() const;
    int GetExitStatus() const;
    TError Kill(int signal) const;
    void DeliverExitStatus(int status);

    std::string GetStdout(size_t limit) const;
    std::string GetStderr(size_t limit) const;

    void ApplyLimits();
    int ChildCallback();
    TError Restore(int pid);
    TError ValidateCgroups() const;
    TError Rotate() const;
};

#endif
