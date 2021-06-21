#pragma once

#include <string>
#include <vector>

#include "util/error.hpp"
#include "util/path.hpp"
#include "util/cred.hpp"
#include "util/namespace.hpp"

class TContainer;

struct TBindMount {
    TPath Source;
    TPath Target;
    uint64_t MntFlags = 0;
    bool ControlSource = false;
    bool ControlTarget = false;

    TError Mount(const TCred &cred, const TPath &target_root) const;

    static TError Parse(const std::string &str, std::vector<TBindMount> &binds);
    static std::string Format(const std::vector<TBindMount> &binds);

    TError Load(const rpc::TContainerBindMount &spec);
    void Dump(rpc::TContainerBindMount &spec);
};

struct TMountNamespace {
public:
    std::string Container; /* for logging and errors */
    TCred BindCred;
    TPath Cwd;
    TPath Root; /* path in ParentNs.Mnt */
    TFile RootFd;
    TFile ProcFd;
    TFile ProcSysFd;
    bool RootRo;
    TPath HostRoot;
    std::vector<TBindMount> BindMounts;
    std::map<TPath, TPath> Symlink;
    bool BindPortoSock;
    bool IsolateRun;
    uint64_t RunSize;
    std::string Systemd;

    TNamespaceFd HostNs;
    TNamespaceFd ContainerNs;

    TError SetupRoot(bool rootUser);
    TError MountRun();
    TError RemountRun();
    TError MountBinds();
    TError ProtectProc();
    TError MountTraceFs();
    TError MountSystemd();
    TError MountCgroups(bool rw);

    TError Setup(const TContainer &ct);

    TError Enter(pid_t pid);
    TError Leave();
    TError CreateSymlink(const TPath &symlink, const TPath &target);
};

bool IsSystemPath(const TPath &path);
