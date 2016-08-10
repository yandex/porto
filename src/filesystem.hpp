#pragma once

#include <string>
#include <vector>

#include "util/error.hpp"
#include "util/path.hpp"
#include "util/cred.hpp"

struct TBindMount {
    TPath Source;
    TPath Dest;
    bool ReadOnly;
    bool ReadWrite;
};

struct TMountNamespace {
public:
    std::string Container; /* for logging and errors */
    TCred OwnerCred;
    TPath Cwd;
    TPath ParentCwd;
    TPath Root; /* path in ParentNs.Mnt */
    bool RootRdOnly;
    std::vector<TBindMount> BindMounts;
    bool BindPortoSock;
    int LoopDev;

    TError MountBinds();
    TError BindResolvConf();
    TError RemountRootRo();
    TError MountRun();
    TError MountTraceFs();
    TError MountRootFs();
    TError IsolateFs();
};

bool IsSystemPath(const TPath &path);
