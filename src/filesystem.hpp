#pragma once

#include <string>
#include <vector>

#include "util/error.hpp"
#include "util/path.hpp"
#include "util/mount.hpp"
#include "util/cred.hpp"

struct TBindMount {
    TPath Source;
    TPath Dest;
    bool ReadOnly;
    bool ReadWrite;
};

struct TMountNamespace {
public:
    std::string Container;
    TCred OwnerCred;
    TPath Cwd;
    TPath ParentCwd;
    TPath Root; /* path in ParentNs.Mnt */
    bool RootRdOnly;
    std::vector<TBindMount> BindMounts;
    bool BindDns;
    bool BindPortoSock;
    std::string ResolvConf;
    int LoopDev;

    TError MountBinds();
    TError BindResolvConf();
    TError RemountRootRo();
    TError MountRun();
    TError MountRootFs();
    TError IsolateFs();
};
