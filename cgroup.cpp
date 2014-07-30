#include <iostream>

#include <sstream>
#include <fstream>
#include <map>

#include <algorithm>

#include <dirent.h>
#include <unistd.h>

#include "cgroup.hpp"
#include "folder.hpp"

using namespace std;

set<string> subsystems = {"cpuset", "cpu", "cpuacct", "memory", "devices",
                          "freezer", "net_cls", "net_prio", "blkio",
                          "perf_event", "hugetlb", "name=systemd"};

set<string> create_subsystems = {"cpuset", "cpu", "cpuacct", "memory"};

void TCgroup::FindChildren() {
    DIR *dirp;
    struct dirent *dp;

    dirp = opendir(name.c_str());
    if (!dirp)
        throw "Cannot read sysfs";

    try {
        while ((dp = readdir(dirp)) != nullptr) {
            if (!strcmp(".", dp->d_name) ||
                !strcmp ("..", dp->d_name) ||
                !(dp->d_type & DT_DIR) || (dp->d_type & DT_LNK))
                continue;

            string cp = name + "/" + string(dp->d_name);
            children.insert(new TCgroup(cp, this, level + 1));
        }
    } catch (...) {
        closedir(dirp);
        throw;
    }
}

void TCgroup::Create() {
    TFolder f(name);
    f.Create(mode);
}

void TCgroup::Remove() {
    TFolder f(name);
    f.Remove();
}

ostream& operator<<(ostream& os, const TCgroup& cg) {
    os << string(4 * cg.level, ' ') << cg.name << " {" << endl;

    for (auto c : cg.children)
        os << *c << endl;

    os << string(4 * cg.level, ' ') << "}";

    return os;
}

void TCgroupState::UpdateFromProcFs() {
    TMountState ms;
    ms.UpdateFromProcfs();
    
    for (auto cg : root_cgroups)
        delete cg.second;

    root_cgroups.clear();
    controllers.clear();

    for (auto m : ms.Mounts()) {
        set<string> flags = m->Flags();
        set<string> cs;

        set_intersection(flags.begin(), flags.end(),
                         subsystems.begin(), subsystems.end(),
                         inserter(cs, cs.begin()));

        if (cs.size() == 0)
            continue;

        string name;
        for (auto c = cs.begin(); c != cs.end(); ) {
            name += *c;
            if (++c != cs.end())
                name += ",";
        }

        TController *ctrl = new TController(name, m);
        TCgroup *cg = new TCgroup(m->Mountpoint());
        root_cgroups[name] = cg;

        for (auto c : cs)
            controllers[name] = ctrl;
    }
}

void TCgroupState::MountMissingControllers() {
    for (auto c : create_subsystems) {
        if (controllers[c] == nullptr) {
            controllers[c] = new TController(c);
            controllers[c]->Attach();
        }
    }
}

void TCgroupState::MountMissingTmpfs(string tmpfs) {
    TMountState ms;

    ms.UpdateFromProcfs();

    for (auto m : ms.Mounts())
        if (m->Mountpoint() == tmpfs)
            return;

    TMount mount("cgroup", tmpfs, "tmpfs", 0, set<string>{});
    mount.Mount();
}

void TCgroupState::UmountAll() {
    for (auto c : controllers) {
        c.second->Detach();
    }
}
