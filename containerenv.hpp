#ifndef __CONTAINERENV_HPP__
#define __CONTAINERENV_HPP__

#include <vector>
#include <string>
#include <memory>

#include "error.hpp"

class TCgroup;

class TTaskEnv {
    std::string path;
    std::vector<std::string> args;
    std::string cwd;
    //std::vector<std::string> env;

public:
    TTaskEnv(const std::string &command, const std::string cwd);
    std::string GetPath();
    std::vector<std::string> GetArgs();
    std::string GetCwd();
};

class TContainerEnv {
    std::vector<std::shared_ptr<TCgroup> > leaf_cgroups;
    // namespaces
    // virtual devices
    // ...

public:
    TTaskEnv taskEnv;

    TContainerEnv(std::vector<std::shared_ptr<TCgroup> > &cgroups,
                  TTaskEnv &taskEnv) : leaf_cgroups(cgroups), taskEnv(taskEnv) {
    }

    void Create();
    TError Attach();
};

#endif
