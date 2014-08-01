#ifndef __CONTAINERENV_HPP__
#define __CONTAINERENV_HPP__

#include <vector>
#include <string>

class TCgroup;

class TContainerEnv {
    std::vector<TCgroup*> leaf_cgroups;
    // namespaces
    // virtual devices
    // ...

    class TExecEnv {
        std::string path;
        std::vector<std::string> args;
        std::vector<std::string> env;
    } exec_env;

public:
    void Create();
    void Enter();
};

#endif
