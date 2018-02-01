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
        std::string Data;
    };
    std::vector<TEnvVar> Vars;
    std::vector<char *> Environ;

    void ClearEnv();
    bool GetEnv(const std::string &name, std::string &value) const;
    TError SetEnv(const std::string &name, const std::string &value,
                  bool overwrite = true, bool lock = false);
    TError UnsetEnv(const std::string &name, bool overwrite = true);
    char **Envp();

    TError Parse(const std::vector<std::string> &cfg, bool overwrite);
    void Format(std::vector<std::string> &cfg) const;
    TError Apply() const;
};

struct TUlimitResource {
    int Type;
    uint64_t Soft;
    uint64_t Hard;

    TError Parse(const std::string &str);
    std::string Format() const;
};

struct TUlimit {
    std::vector<TUlimitResource> Resources;

    static int GetType(const std::string &name);
    static std::string GetName(int type);
    TError Parse(const std::string &str);
    std::string Format() const;
    TError Apply(pid_t pid = 0) const;
    void Clear() { Resources.clear(); }
    void Set(int type, uint64_t soft, uint64_t hard, bool overwrite = true);
    void Merge(const TUlimit &ulimit, bool owerwrite = true);
};
