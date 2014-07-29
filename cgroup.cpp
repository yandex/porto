#include <iostream>
#include <string>
#include <sstream>
#include <fstream>
#include <map>
#include <set>
#include <algorithm>

#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>

#include "mount.hpp"

using namespace std;

set<string> subsystems = {"cpuset", "cpu", "cpuacct", "memory", "devices",
                          "freezer", "net_cls", "net_prio", "blkio",
                          "perf_event", "hugetlb", "name=systemd"};

class TCgroup {
    string name;
    TCgroup *parent;
    int level;
    set<TCgroup*> children;

    mode_t mode = 0x666;

public:
    TCgroup(string path, TCgroup *parent = nullptr, int level = 0) :
        name (path), parent(parent), level(level) {
        try {
            FindChildren();
        } catch (...) {
            DropChildren();
            throw;
        }
    }

    void FindChildren() {
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

    void DropChildren() {
        for (auto c : children)
            delete c;
    }

    ~TCgroup() {
        DropChildren();
    }

    void Create() {
        if (parent == nullptr)
            throw "Cannot create root cgroup";

        if (mkdir(name.c_str(), mode)) {
            switch (errno) {
            case EEXIST:
                throw "Cgroup already exists";
            default:
                throw "Cannot create cgroup";
            }
        }
    }
    
    friend ostream& operator<<(ostream& os, const TCgroup& cg) {
        os << string(4 * cg.level, ' ') << cg.name << " {" << endl;

        for (auto c : cg.children)
            os << *c << endl;

        os << string(4 * cg.level, ' ') << "}";

        return os;
    }
};

class TCgroupState {
    // cgroups are owned by mounts, not controllers
    map<string, TCgroup*> mounts; // can be net_cls,netprio
    map<string, TCgroup*> controllers; // can be net_cls _or_ net_prio

public:
    TCgroupState() {
        TMountState ms;

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
            mounts[name] = cg;

            for (auto c : cs)
                controllers[name] = cg;
        }
    }

    ~TCgroupState() {
        for (auto c : mounts)
            delete c.second;
    }

    friend ostream& operator<<(ostream& os, const TCgroupState& st) {
        for (auto ss : st.controllers) {
            os << ss.first << ":" << endl;
            os << *ss.second << endl;
        }

        return os;
    }

    const string DefaultMountpoint(const string controller) {
        return "/sys/fs/cgroup/" + controller;
    }
    
    void MountMissingControllers() {
        for (auto c : subsystems) {
            if (controllers[c] == nullptr) {
                TMount mount("cgroup", DefaultMountpoint(c),
                             "cgroup", 0, set<string>{c});

                mount.Mount();

                TCgroup *cg = new TCgroup(mount.Mountpoint());
                mounts[c] = cg;
                controllers[c] = cg;
            }
        }
    }
};
