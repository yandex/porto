#include <iostream>
#include <sstream>
#include <algorithm>

#include "cgroup.hpp"
#include "folder.hpp"
#include "registry.hpp"

using namespace std;

// TCgroup
shared_ptr<TCgroup> TCgroup::Get(string name, shared_ptr<TCgroup> parent) {
    return TRegistry<TCgroup>::Get(TCgroup(name, parent));
}

shared_ptr<TCgroup> TCgroup::Get(shared_ptr<TSubsystem> subsystem) {
    return TRegistry<TCgroup>::Get(TCgroup({subsystem}));
}

TCgroup::TCgroup(string name, shared_ptr<TCgroup> parent, int level) :
    name(name), parent(parent), level(level) {
}

TCgroup::TCgroup(shared_ptr<TMount> mount, vector<shared_ptr<TSubsystem>> subsystems) :
    name("/"), parent(shared_ptr<TCgroup>(nullptr)), level(0), mount(mount),
    subsystems(subsystems) {
}

TCgroup::TCgroup(vector<shared_ptr<TSubsystem>> subsystems) :
    name("/"), parent(shared_ptr<TCgroup>(nullptr)), level(0),
    subsystems(subsystems) {

    set<string> flags;

    for (auto c : subsystems)
        flags.insert(c->Name());

    mount = TRegistry<TMount>::Get(TMount("cgroup", tmpfs + "/" +
                                          CommaSeparatedList(flags),
                                          "cgroup", 0, flags));
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

        TMount root("cgroup", tmpfs, "tmpfs", 0, set<string>{});
        bool mount_root = true;

        for (auto m : ms.Mounts()) {
            if (*m == root)
                mount_root = false;
            if (*m == *mount)
                return;
        }

        if (mount_root)
            root.Mount();
    } else
        parent->Create();

    TFolder f(Path());
    if (!f.Exists())
        f.Create(mode);

    if (IsRoot())
        mount->Mount();
}

void TCgroup::Remove() {
    if (IsRoot())
        mount->Umount();

    TFolder f(Path());
    f.Remove();
}

TError TCgroup::Attach(int pid) {
    if (!IsRoot()) {
        TFile f(Path() + "/cgroup.procs");
        return f.AppendString(to_string(pid));
    }

    return 0;
}

bool operator==(const TCgroup& c1, const TCgroup& c2) {
    if (c1.name != c2.name)
        return false;
    if (c1.parent != c2.parent)
        return false;
    if (!c1.parent && !c2.parent)
        return c1.subsystems == c2.subsystems;
    return true;
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

    static set<string> supported_subsystems =
        {"cpuset", "cpu", "cpuacct", "memory",
         "devices", "freezer", "net_cls", "net_prio", "blkio",
         "perf_event", "hugetlb", "name=systemd"};

    for (auto mount : ms.Mounts()) {
        set<string> flags = mount->Flags();
        set<string> cs;

        set_intersection(flags.begin(), flags.end(),
                         supported_subsystems.begin(),
                         supported_subsystems.end(),
                         inserter(cs, cs.begin()));

        if (cs.empty())
            continue;

        string name = CommaSeparatedList(cs);

        vector<shared_ptr<TSubsystem>> cg_controllers;
        for (auto c : cs) {
            subsystems[c] = TRegistry<TSubsystem>::Get(TSubsystem(name));
            cg_controllers.push_back(subsystems[c]);
        }

        auto root = TRegistry<TCgroup>::Get(TCgroup(mount, cg_controllers));
        cgroups.push_back(root);

        for (auto cg : root->FindChildren())
            cgroups.push_back(cg);
    }
}

ostream& operator<<(ostream& os, const TCgroupSnapshot& st) {
    for (auto ss : st.cgroups)
        if (ss->IsRoot())
            os << *ss << endl;

    return os;
}
