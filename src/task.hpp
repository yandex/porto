#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>

#include "util/namespace.hpp"
#include "util/path.hpp"
#include "util/cred.hpp"
#include "stream.hpp"
#include "cgroup.hpp"

extern "C" {
#include <sys/resource.h>
}

class TTask;
class TContainerEnv;
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
    TStdStream Stdin, Stdout, Stderr;
    TNamespaceSnapshot ParentNs;
    std::map<int,struct rlimit> Rlimit;
    std::string Hostname;
    bool SetEtcHostname;
    bool BindDns;
    std::string ResolvConf;
    std::vector<TBindMap> BindMap;
    std::vector<TDevice> Devices;
    int LoopDev;
    uint64_t Caps;
    bool NewMountNs;
    std::vector<TCgroup> Cgroups;
    TCred Cred;

    const char** GetEnvp() const;
    bool EnvHasKey(const std::string &key);

    TUnixSocket Sock, MasterSock;
    TUnixSocket Sock2,  MasterSock2;
    int ReportStage = 0;
};

class TTask: public TNonCopyable {
    std::unique_ptr<TTaskEnv> Env;

    enum ETaskState { Stopped, Started } State;
    int ExitStatus;

    pid_t Pid, VPid, WPid;

    void ReportPid(pid_t pid) const;

    TError ChildApplyCapabilities();
    TError ChildDropPriveleges();
    TError ChildExec();
    TError ChildBindDns();
    TError ChildBindDirectores();
    TError ChildMountRootFs();
    TError ChildRemountRootRo();
    TError ChildIsolateFs();

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
    void Abort(const TError &error) const;

    bool IsZombie() const;

    bool HasCorrectParent();

    void DumpDebugInfo();
};

TError TaskGetLastCap();
