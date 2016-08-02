#pragma once

#include <string>
#include <vector>

#include "util/namespace.hpp"
#include "util/mount.hpp"
#include "util/path.hpp"
#include "util/cred.hpp"
#include "util/unix.hpp"
#include "container.hpp"
#include "cgroup.hpp"
#include "env.hpp"
#include "filesystem.hpp"

extern "C" {
#include <sys/resource.h>
}

struct TTaskEnv {
    std::shared_ptr<TContainer> CT;
    std::shared_ptr<TClient> Client;
    TScopedFd PortoInitFd;
    TMountNamespace Mnt;
    TEnv Env;
    bool TripleFork;
    bool QuadroFork;
    TNamespaceSnapshot ParentNs;
    std::vector<TDevice> Devices;
    std::vector<std::string> Autoconf;
    bool NewMountNs;
    std::vector<TCgroup> Cgroups;
    TCred Cred;

    TUnixSocket Sock, MasterSock;
    TUnixSocket Sock2, MasterSock2;
    int ReportStage = 0;

    TError Start();
    void StartChild();

    TError ConfigureChild();
    TError ChildApplyLimits();
    TError WriteResolvConf();
    TError SetHostname();

    TError WaitAutoconf();
    TError ChildExec();

    void ReportPid(pid_t pid);
    void Abort(const TError &error);
};
