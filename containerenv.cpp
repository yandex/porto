#include "containerenv.hpp"

void TContainerEnv::Create() {
    for (auto cg : leaf_cgroups)
        cg.Create();
}

void TContainerEnv::Enter() {
    for (auto cg : leaf_cgroups)
        cg.Enter();
}

