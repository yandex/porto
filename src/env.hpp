#pragma once

#include <string>
#include <vector>
#include "util/error.hpp"

struct TEnv {
    struct TEnvVar {
        std::string Name;
        std::string Value;
        bool Set;
        bool Locked;
        bool Overwritten;
        bool Secret;
        std::string Data;
    };
    std::vector<TEnvVar> Vars;
    std::vector<char *> Environ;

    void ClearEnv();
    TError GetEnv(const std::string &name, std::string &value) const;
    TError SetEnv(const std::string &name, const std::string &value,
                  bool overwrite = true, bool lock = false,
                  bool secret = false);
    TError UnsetEnv(const std::string &name, bool overwrite = true);
    char **Envp();

    TError Parse(const std::string &cfg, bool overwrite, bool secret = false);
    void Format(std::string &cfg, bool show_secret = false) const;
    TError Apply() const;
};

struct TUlimitResource {
    int Type;
    uint64_t Soft;
    uint64_t Hard;
    bool Overwritten;

    TError Parse(const std::string &str);
    std::string Format() const;
};

struct TUlimit {
    std::vector<TUlimitResource> Resources;

    static int GetType(const std::string &name);
    static std::string GetName(int type);
    TError Parse(const std::string &str);
    std::string Format() const;
    TError Load(pid_t pid = 0);
    TError Apply(pid_t pid = 0) const;
    void Clear() { Resources.clear(); }
    void Set(int type, uint64_t soft, uint64_t hard, bool overwrite = true);
    void Merge(const TUlimit &ulimit, bool owerwrite = true);
};
