#pragma once

#include <string>
#include <vector>

#include "util/error.hpp"
#include "util/path.hpp"
#include "util/cred.hpp"

struct TBindMount {
    TPath Source;
    TPath Dest;
    bool ReadOnly = false;
    bool ReadWrite = false;

    TBindMount() {}
    TBindMount(TPath source, TPath dest, bool ro):
        Source(source), Dest(dest), ReadOnly(ro), ReadWrite(!ro) {}
};

struct TMountNamespace {
public:
    std::string Container; /* for logging and errors */
    TCred OwnerCred;
    TPath Cwd;
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
