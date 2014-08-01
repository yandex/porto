#ifndef __CONTAINERENV_HPP__
#define __CONTAINERENV_HPP__

#include <vector>
#include <string>

#include "error.hpp"

class TCgroup;

class TExecEnv {
    std::string path;
    std::vector<std::string> args;
    //std::vector<std::string> env;

public:
    TExecEnv(const std::string &command);
    std::string GetPath();
    std::vector<std::string> GetArgs();
};

class TContainerEnv {
    std::vector<TCgroup*> cgroups;
    // namespaces
    // virtual devices
    // ...

    TExecEnv env;

public:
    TContainerEnv(std::vector<TCgroup*> &cgroups,
                  TExecEnv &env) : cgroups(cgroups), env(env) {
    }

    void Create();
    TError Enter();
};

#endif
