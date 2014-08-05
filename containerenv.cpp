#include <iterator>
#include <sstream>

#include "cgroup.hpp"
#include "containerenv.hpp"

extern "C" {
#include <sys/types.h>
#include <unistd.h>
}

using namespace std;

void TContainerEnv::Create() {
    for (auto cg : leaf_cgroups)
        cg->Create();
}

TError TContainerEnv::Attach() {
    pid_t self = getpid();

    for (auto cg : leaf_cgroups) {
        auto error = cg->Attach(self);
        if (error)
            return error;
    }

    return TError();
}

