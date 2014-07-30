#include <iostream>

#include <sstream>
#include <fstream>
#include <map>

#include <algorithm>

#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>

#include "cgroup.hpp"

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
    int ret = mkdir(name.c_str(), mode);

    TLogger::LogAction("mkdir " + name, ret, errno);

    if (ret) {
        switch (errno) {
        case EEXIST:
            throw "Cgroup already exists";
        default:
            throw "Cannot create cgroup: " + string(strerror(errno));
        }
    }
}

void TCgroup::Remove() {
    int ret = rmdir(name.c_str());

    TLogger::LogAction("rmdir " + name, ret, errno);

    if (ret)
        throw "Cannot remove cgroup: " + name;
}

ostream& operator<<(ostream& os, const TCgroup& cg) {
    os << string(4 * cg.level, ' ') << cg.name << " {" << endl;

    for (auto c : cg.children)
        os << *c << endl;

    os << string(4 * cg.level, ' ') << "}";

    return os;
}

TCgroupState::TCgroupState() {
    Update();
}

void TCgroupState::Update() {
    TMountState ms;
    
    for (auto c : controllers)
        delete c.second;

    controllers.clear();
    raw_controllers.clear();

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

        TCgroup *cg = new TCgroup(m->Mountpoint());
        controllers[name] = cg;

        for (auto c : cs)
            raw_controllers[name] = cg;
    }
}
    
void TCgroupState::MountMissingControllers() {
    if (controllers.empty()) {
        TMount root("cgroups", DefaultMountpoint(""),
                    "tmpfs", 0, set<string>{});
        root.Mount();
    }

    for (auto c : create_subsystems) {
        if (raw_controllers[c] == nullptr) {
            TCgroup *cg = new TCgroup(DefaultMountpoint(c));
            cg->Create();

            TMount *mount = new TMount("cgroup", DefaultMountpoint(c),
                                       "cgroup", 0, set<string>{c});
            mount->Mount();
            delete mount;

            raw_controllers[c] = cg;
            controllers[c] = cg;

            cg->FindChildren();
        }
    }
}

void TCgroupState::UmountAll() {
    Update();

    for (auto cg : controllers) {
        TMount *mount = new TMount("cgroup", cg.second->Name(),
                                   "cgroup", 0, set<string>{});
        mount->Umount();
        cg.second->Remove();
    }

    Update();
}
