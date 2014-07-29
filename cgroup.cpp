#include <iostream>
#include <string>
#include <sstream>
#include <fstream>
#include <map>
#include <set>
#include <algorithm>

#include <sys/types.h>
#include <dirent.h>
#include <string.h>

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

public:
    TCgroup(string path, TCgroup *parent = nullptr, int level = 0) :
        name (path), parent(parent), level(level) {
            DIR *dirp;
            struct dirent *dp;

            dirp = opendir(path.c_str());
            if (!dirp)
                throw errno;

            while ((dp = readdir(dirp)) != nullptr) {
                if (!strcmp(".", dp->d_name) ||
                    !strcmp ("..", dp->d_name) ||
                    !(dp->d_type & DT_DIR) || (dp->d_type & DT_LNK))
                    continue;

                string cp = path + "/" + string(dp->d_name);
                children.insert(new TCgroup(cp, this, level + 1));
            }

            closedir(dirp);
    }

    ~TCgroup() {
        for (auto c : children)
            delete c;
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
    map<string, TCgroup*> controllers;

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
            for (auto c : cs)
                name += c; //TODO: add ","

            TCgroup *cg = new TCgroup(m->Mountpoint());
            controllers[name] = cg;
        }
    }

    ~TCgroupState() {
        for (auto c : controllers)
            delete c.second;
    }

    friend ostream& operator<<(ostream& os, const TCgroupState& st) {
        for (auto ss : st.controllers) {
            os << ss.first << ":" << endl;
            os << *ss.second << endl;
        }

        return os;
    }
};
