#pragma once

#include <string>
#include <vector>

#include "util/namespace.hpp"
#include "util/mount.hpp"
#include "util/path.hpp"
#include "util/cred.hpp"
#include "util/unix.hpp"
#include "container.hpp"
#include "stream.hpp"
#include "cgroup.hpp"
#include "env.hpp"
#include "filesystem.hpp"

extern "C" {
#include <sys/resource.h>
}

struct TTaskEnv {
    TContainer *CT;
    TScopedFd PortoInitFd;
    TMountNamespace Mnt;
    TEnv Env;
    bool TripleFork;
    bool QuadroFork;
    TStdStream Stdin, Stdout, Stderr;
    TNamespaceSnapshot ParentNs;
    bool SetEtcHostname;
    std::vector<TDevice> Devices;
    std::vector<std::string> Autoconf;
    bool NewMountNs;
    std::vector<TCgroup> Cgroups;
    TCred Cred;

    TUnixSocket Sock, MasterSock;
    TUnixSocket Sock2, MasterSock2;
    int ReportStage = 0;

    void ReportPid(pid_t pid);

    TError Start();
    TError ChildApplyCapabilities();
    TError ChildExec();
    TError ChildBindDns();
    TError ChildMountBinds();
    TError ChildMountRun();
    TError ChildMountRootFs();
    TError ChildRemountRootRo();
    TError ChildIsolateFs();

    TError ChildApplyLimits();
    TError ChildSetHostname();
    TError ConfigureChild();
    TError WaitAutoconf();
    void StartChild();
    void Restore(std::vector<int> pids);
    void Abort(const TError &error);
};
