#include <iterator>
#include <sstream>

#include "cgroup.hpp"
#include "containerenv.hpp"
#include "util/unix.hpp"

using namespace std;

void TContainerEnv::Create() {
    for (auto cg : leaf_cgroups)
        cg->Create();
}

TError TContainerEnv::Attach() {
    int self = GetPid();

    for (auto cg : leaf_cgroups) {
        auto error = cg->Attach(self);
        if (error)
            return error;
    }

    return TError::Success;
}

