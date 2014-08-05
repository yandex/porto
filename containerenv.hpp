#ifndef __CONTAINERENV_HPP__
#define __CONTAINERENV_HPP__

#include <vector>
#include <string>
#include <memory>

#include "error.hpp"

class TCgroup;

class TContainerEnv {
    std::vector<std::shared_ptr<TCgroup> > leaf_cgroups;
    // namespaces
    // virtual devices
    // ...

public:

    TContainerEnv(std::vector<std::shared_ptr<TCgroup> > &cgroups) : leaf_cgroups(cgroups) {
    }

    void Create();
    TError Attach();
};

#endif
