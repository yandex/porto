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
