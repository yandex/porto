#ifndef __TASK_HPP__
#define __TASK_HPP__

#include "cgroup.hpp"

class TTask {
    pid_t pid;

    vector<TCgroup*> cgroups;

public:
    FindCgroups() {
        ifstream in("proc/self/cgroup");

        if (!in.is_open())
            throw "Cannot open /proc/self/cgroup";

        while (getline(in, line)) {
            // new TCgroup
        }
    }
};

#endif
