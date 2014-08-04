#include <iostream>

#include <sstream>
#include <map>

#include <algorithm>

#include "cgroup.hpp"
#include "folder.hpp"
#include "registry.hpp"

using namespace std;

set<string> supported_subsystems = {"cpuset", "cpu", "cpuacct", "memory",
    "devices", "freezer", "net_cls", "net_prio", "blkio",
    "perf_event", "hugetlb", "name=systemd"};

set<string> create_subsystems = {"cpuset", "cpu", "cpuacct", "memory"};

// TCgroup

TCgroup::TCgroup(string name, shared_ptr<TCgroup> parent, int level) :
    name(name), parent(parent), level(level) {
}

TCgroup::TCgroup(shared_ptr<TMount> mount, set<shared_ptr<TSubsystem>> subsystems) :
    name("/"), parent(shared_ptr<TCgroup>(nullptr)), level(0), mount(mount),
    subsystems(subsystems) {
}

TCgroup::TCgroup(set<shared_ptr<TSubsystem>> subsystems) :
    name("/"), parent(shared_ptr<TCgroup>(nullptr)), level(0),
    subsystems(subsystems) {

    set<string> flags;

    for (auto c : subsystems)
        flags.insert(c->Name());

    mount = TRegistry<TMount>::Get(TMount("cgroup", tmpfs + "/" +
                                          CommaSeparatedList(flags),
                                          "cgroup", 0, flags));
}

TCgroup::~TCgroup() {
}

string TCgroup::Name() {
    return name;
}

vector<shared_ptr<TCgroup> > TCgroup::FindChildren() {
    TFolder f(Path());
    vector<shared_ptr<TCgroup> > ret;
    auto self = TRegistry<TCgroup>::Get(*this);

    for (auto s : f.Subfolders()) {
        auto cg = TRegistry<TCgroup>::Get(TCgroup(s, self, level + 1));
        
        children.push_back(weak_ptr<TCgroup>(cg));
        for (auto c : cg->FindChildren())
            ret.push_back(c);
    }

    ret.push_back(self);
    
    return ret;
}

bool TCgroup::IsRoot() const {
    return !parent;
}

string TCgroup::Path() {
    if (IsRoot())
        return mount->Mountpoint();
    else
        return parent->Path() + "/" + name;
}

void TCgroup::Create() {
    if (IsRoot()) {
        TMountSnapshot ms;
        for (auto m : ms.Mounts()) {
            if (m == mount)
                return;
        }

        TFolder f(mount->Mountpoint());
        f.Create(mode);
        mount->Mount();

    } else {
        parent->Create();

        TFolder f(Path());
        f.Create(mode);
    }
}

void TCgroup::Remove() {
    if (IsRoot()) {
        TFolder f(mount->Mountpoint());
        mount->Umount();
        f.Remove();
    } else {
        TFolder f(Path());
        f.Remove();
    }
}

TError TCgroup::Attach(int pid) {
    if (!IsRoot()) {
        TFile f(Path() + "/cgroup.procs");
        return f.AppendString(to_string(pid));
    }

    return 0;
}

ostream& operator<<(ostream& os, const TCgroup& cg) {
    if (cg.IsRoot()) {
        for (auto s : cg.subsystems)
            os << *s << ",";

        os << " {" << endl;
    } else
        os << string(4 * cg.level, ' ') << cg.name << " {" << endl;

    for (auto c : cg.children) {
        auto child = c.lock();
        if (child)
            os << *child << endl;
    }

    os << string(4 * cg.level, ' ') << "}";

    return os;
}

// TCgroupSnapshot
TCgroupSnapshot::TCgroupSnapshot() {
    TMountSnapshot ms;

    for (auto m : ms.Mounts()) {
        set<string> flags = m->Flags();
        set<string> cs;

        set_intersection(flags.begin(), flags.end(),
                         supported_subsystems.begin(), supported_subsystems.end(),
                         inserter(cs, cs.begin()));

        if (cs.size() == 0)
            continue;

        string name = CommaSeparatedList(cs);

        set<shared_ptr<TSubsystem>> cg_controllers;
        for (auto c : cs) {
            subsystems[c] = TRegistry<TSubsystem>::Get(TSubsystem(name));
            cg_controllers.insert(subsystems[c]);
        }

        auto root = TRegistry<TCgroup>::Get(TCgroup(m, cg_controllers));
        cgroups.push_back(root);

        for (auto cg : root->FindChildren())
            cgroups.push_back(cg);
    }
}

/*
void TCgroupSnapshot::MountMissingTmpfs(string tmpfs) {
    TMountSnapshot ms;

    for (auto m : ms.Mounts())
        if (m->Mountpoint() == tmpfs)
            return;

    TMount mount("cgroup", tmpfs, "tmpfs", 0, set<string>{});
    mount.Mount();
}

void TCgroupSnapshot::UmountAll() {
    for (auto root : root_cgroups) {
        root.second->Detach();
    }
}
*/

ostream& operator<<(ostream& os, const TCgroupSnapshot& st) {
    for (auto ss : st.cgroups)
        if (ss->IsRoot())
            os << *ss << endl;

    return os;
}
