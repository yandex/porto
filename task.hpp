#ifndef __TASK_HPP__
#define __TASK_HPP__

#include <string>
#include <vector>
#include <cstdint>

#include "cgroup.hpp"
#include "util/namespace.hpp"
#include "util/path.hpp"

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
    TPath Cwd;
    bool CreateCwd;
    TPath Root;
    std::string User;
    std::string Group;
    std::string Environ;
    bool Isolate = false;
    TPath StdinPath;
    TPath StdoutPath;
    TPath StderrPath;
    TNamespaceSnapshot Ns;
    std::map<int,struct rlimit> Rlimit;
    std::string Hostname;
    bool BindDns;

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
    void ReportPid(int pid) const;
    void Abort(int result, const std::string &msg) const;
    void Abort(const TError &error, const std::string &msg = "") const;

    TError RotateFile(const TPath &path) const;
    TError CreateCwd();
    TError CreateNode(const TPath &path, unsigned int mode, unsigned int dev);
    void ChildOpenStdFile(const TPath &path, int expected);
    void ChildReopenStdio();
    void ChildDropPriveleges();
    void ChildExec();
    TError ChildBindDns();
    TError RestrictProc();
    TError ChildMountDev();
    TError ChildIsolateFs(bool priveleged);

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

    void ChildApplyLimits();
    void ChildSetHostname();
    int ChildCallback();
    TError Restore(int pid);
    TError ValidateCgroups() const;
    TError Rotate() const;
};

#endif
