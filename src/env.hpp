#pragma once

#include <string>
#include <vector>
#include "util/error.hpp"

struct TEnv {
    struct TEnvVar {
        TString Name;
        TString Value;
        bool Set;
        bool Locked;
        bool Overwritten;
        TString Data;
    };
    std::vector<TEnvVar> Vars;
    std::vector<char *> Environ;

    void ClearEnv();
    bool GetEnv(const TString &name, TString &value) const;
    TError SetEnv(const TString &name, const TString &value,
                  bool overwrite = true, bool lock = false);
    TError UnsetEnv(const TString &name, bool overwrite = true);
    char **Envp();

    TError Parse(const TString &cfg, bool overwrite);
    void Format(TString &cfg) const;
    TError Apply() const;
};

struct TUlimitResource {
    int Type;
    uint64_t Soft;
    uint64_t Hard;
    bool Overwritten;

    TError Parse(const TString &str);
    TString Format() const;
};

struct TUlimit {
    std::vector<TUlimitResource> Resources;

    static int GetType(const TString &name);
    static TString GetName(int type);
    TError Parse(const TString &str);
    TString Format() const;
    TError Load(pid_t pid = 0);
    TError Apply(pid_t pid = 0) const;
    void Clear() { Resources.clear(); }
    void Set(int type, uint64_t soft, uint64_t hard, bool overwrite = true);
    void Merge(const TUlimit &ulimit, bool owerwrite = true);
};
