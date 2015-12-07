#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>

#include "util/namespace.hpp"
#include "util/path.hpp"
#include "util/netlink.hpp"
#include "util/cred.hpp"

#include "network.hpp" // for TNetCfg

extern "C" {
#include <sys/resource.h>
}

class TTask;
class TContainerEnv;
class TFolder;
class TCgroup;
class TSubsystem;

struct TExitStatus {
    // Task was not started due to the following error
    int Error;
    // Task exited with given status
    int Status;
};

struct TBindMap {
    TPath Source;
    TPath Dest;
    bool Rdonly;
};

struct TTaskEnv : public TNonCopyable {
    std::string Container;
    std::string Command;
    TScopedFd PortoInitFd;
    TPath Cwd;
    TPath ParentCwd;
    TPath Root; /* path in ParentNs.Mnt */
    bool RootRdOnly;
    std::vector<std::string> Environ;
    bool Isolate = false;
    bool TripleFork;
    bool QuadroFork;
    TPath StdinPath;
    TPath StdoutPath;
    TPath StderrPath;
    bool DefaultStdin = false;
    bool DefaultStdout = false;
    bool DefaultStderr = false;
    TNamespaceSnapshot ParentNs;
    bool CloneParentMntNs;
    std::map<int,struct rlimit> Rlimit;
    std::string Hostname;
    bool SetEtcHostname;
    bool BindDns;
    std::vector<TBindMap> BindMap;
    TNetCfg NetCfg;
    int LoopDev;
    uint64_t Caps;
    bool NewMountNs;
    std::map<std::shared_ptr<TSubsystem>, std::shared_ptr<TCgroup>> LeafCgroups;
    TCred Cred;
    bool NetUp;

    const char** GetEnvp() const;
    bool EnvHasKey(const std::string &key);

    TUnixSocket Sock, MasterSock;
    TUnixSocket Sock2;
    int ReportStage = 0;
};

class TTask: public TNonCopyable {
    std::unique_ptr<TTaskEnv> Env;

    enum ETaskState { Stopped, Started } State;
    int ExitStatus;

    pid_t Pid, VPid, WPid;

    void ReportPid(pid_t pid) const;

    TError ReopenStdio(bool open_default);
    TError IsolateNet(int childPid);

    TError ChildOpenStdFile(const TPath &path, int expected);
    TError ChildApplyCapabilities();
    TError ChildDropPriveleges();
    TError ChildExec();
    TError ChildBindDns();
    TError ChildBindDirectores();
    TError ChildMountRootFs();
    TError ChildRemountRootRo();
    TError ChildIsolateFs();
    TError ChildEnableNet();

    TError DumpProcFsFile(const std::string &filename);

public:
    TTask(std::unique_ptr<TTaskEnv> &env);
    TTask(pid_t pid);
    ~TTask();

    TError Start();
    TError Wakeup();
    pid_t GetPid() const;
    pid_t GetWPid() const;
    pid_t GetPidFor(pid_t pid) const;
    std::vector<int> GetPids() const;
    bool IsRunning() const;
    int GetExitStatus() const;
    TError Kill(int signal) const;
    void Exit(int status);
    void ClearEnv();

    TError ChildApplyLimits();
    TError ChildSetHostname();
    TError ConfigureChild();
    void StartChild();
    void Restore(std::vector<int> pids);
    TError SyncTaskCgroups(pid_t pid) const;
    TError SyncCgroupsWithFreezer() const;
    void Abort(const TError &error) const;

    bool IsZombie() const;

    bool HasCorrectParent();
    bool HasCorrectFreezer();

    void DumpDebugInfo();
};

TError TaskGetLastCap();
