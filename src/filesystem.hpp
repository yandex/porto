#pragma once

#include <string>
#include <vector>

#include "util/error.hpp"
#include "util/path.hpp"
#include "util/cred.hpp"

struct TBindMount {
    TPath Source;
    TPath Target;
    bool ReadOnly = false;
};

struct TMountNamespace {
public:
    std::string Container; /* for logging and errors */
    TCred BindCred;
    TPath ChildCwd;
    TPath ParentCwd;
    TPath Root; /* path in ParentNs.Mnt */
    bool RootRo;
    std::vector<TBindMount> BindMounts;
    bool BindPortoSock;
    bool BindResolvConf;
    uint64_t RunSize;

    TError SetupRoot();
    TError MountRun();
    TError MountBinds();
    TError ProtectProc();
    TError MountTraceFs();

    TError Setup();
};

bool IsSystemPath(const TPath &path);
