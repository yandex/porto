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

using namespace std;

set<string> subsystems = {"cpuset", "cpu", "cpuacct", "memory", "devices",
                          "freezer", "net_cls", "net_prio", "blkio",
                          "perf_event", "hugetlb", "name=systemd"};

class TMount {
    string device;
    string mountpoint;
    string vfstype;
    set<string> flags;

public:
    TMount(string mounts_line) {
        // from single /proc/self/mounts line, like:
        // /dev/sda1 /boot ext4 rw,seclabel,relatime,data=ordered 0 0

        istringstream ss(mounts_line);
        string flag_string, t;
        ss >> device >> mountpoint >> vfstype >> flag_string;

        for (auto i = flag_string.begin(); i != flag_string.end(); i++) {
            if (*i == ',' && !t.empty()) {
                flags.insert(t);
                t.clear();
            } else
                t += *i;
        }

        if (!t.empty())
            flags.insert(t);
    }

    set<string> CgroupSubsystems() {
        set<string> ss;

        set_intersection(flags.begin(), flags.end(),
                         subsystems.begin(), subsystems.end(),
                         inserter(ss, ss.begin()));
        // TODO: systemd

        return ss;
    }

    friend ostream& operator<<(ostream& os, const TMount& m) {
        os << m.device << " " << m.mountpoint << " ";
        for (auto f : m.flags)
            os << f << " ";

        return os;
    }

    string Mountpoint() {
        return mountpoint;
    }
};

class TMountState {
    set<TMount*> mounts;
    map<string, TMount*> cgroup_mounts;

public:
    TMountState() {
        ifstream s("/proc/self/mounts");
        string line;

        while (getline(s, line)) {
            TMount *mount = new TMount(line);
            mounts.insert(mount);

            set<string> ss = mount->CgroupSubsystems();
            if (!ss.empty()) {
                for (auto s : ss)
                    cgroup_mounts[s] = mount;
            }
        }
    }

    ~TMountState() {
        for (auto m : mounts)
            delete m;
    }

    TMount* CgroupMountpoint(string subsystem) {
        return cgroup_mounts[subsystem];
    }

    friend ostream& operator<<(ostream& os, const TMountState& ms) {
        for (auto m : ms.mounts)
            os << *m << endl;

        return os;
    }
};

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
        TMountState mounts;

        for (auto s : subsystems) {
            TMount *mount = mounts.CgroupMountpoint(s);
            if (!mount)
                continue;

            controllers[s] = new TCgroup(mount->Mountpoint());
        }
    }

    friend ostream& operator<<(ostream& os, const TCgroupState& st) {
        for (auto ss : st.controllers) {
            os << ss.first << ":" << endl;
            os << *ss.second << endl;
        }

        return os;
    }
};

int main() {
    TMountState m;
    TCgroupState s;

    cout << m;
    cout << s;
}
