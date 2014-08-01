#include <iostream>

#include <sstream>
#include <map>

#include <algorithm>

#include "cgroup.hpp"
#include "folder.hpp"

using namespace std;

set<string> subsystems = {"cpuset", "cpu", "cpuacct", "memory", "devices",
                          "freezer", "net_cls", "net_prio", "blkio",
                          "perf_event", "hugetlb", "name=systemd"};

set<string> create_subsystems = {"cpuset", "cpu", "cpuacct", "memory"};

// TCgroup

TCgroup::TCgroup(string name, TRootCgroup *root, TCgroup *parent = nullptr,
                 int level = 0) : name(name), root(root), parent(parent),
                                  level(level) {
}

void TCgroup::DropChildren() {
    for (auto c : children)
        delete c;

    children.clear();
}

TCgroup::~TCgroup() {
    DropChildren();
}

string TCgroup::Name() {
    return name;
}

void TCgroup::FindChildren() {
    TFolder f(Path());

    DropChildren();

    for (auto s : f.Subfolders()) {
        TCgroup *cg = new TCgroup(s, root, this, level + 1);
        cg->FindChildren();
        children.insert(cg);
    }
}

string TCgroup::Path() {
    if (parent == nullptr)
        return root->Path() +  "/" + name;
    else
        return parent->Path() + "/" + name;
}

void TCgroup::Create() {
    TFolder f(Path());
    f.Create(mode);
}

void TCgroup::Remove() {
    TFolder f(Path());
    f.Remove();
}

ostream& operator<<(ostream& os, const TCgroup& cg) {
    os << string(4 * cg.level, ' ') << cg.name << " {" << endl;

    for (auto c : cg.children)
        os << *c << endl;

    os << string(4 * cg.level, ' ') << "}";

    return os;
}

// TController
TController::TController(string name) : name(name) {
}

string TController::Name() {
    return name;
}

// TRootCgroup
TRootCgroup::TRootCgroup(TMount *mount, set<TController*> controllers) :
    TCgroup("/", this, 0), mount(mount), controllers(controllers) {
}

TRootCgroup::TRootCgroup(set<TController*> controllers) :
    TCgroup("/", this, 0), controllers(controllers) {
    string mnt;
    set<string> flags;
    for (auto c = controllers.begin(); c != controllers.end(); ) {
        TController *ctrl = *c;
        string name = ctrl->Name();
        flags.insert(name);
        mnt += name;
        if (++c != controllers.end())
            mnt += ",";
    }
    mount = new TMount("cgroup", tmpfs + "/" + mnt, "cgroup", 0, flags);
}

TRootCgroup::~TRootCgroup() {
    for (auto c : controllers)
        delete c;
}

string TRootCgroup::Path() {
    return mount->Mountpoint();
}

void TRootCgroup::Attach() {
    TFolder f(mount->Mountpoint());
    if (!f.Exists())
        f.Create(mode);
    mount->Mount();
}

void TRootCgroup::Detach() {
    TFolder f(mount->Mountpoint());
    mount->Umount();
    f.Remove();
}

// TCgroupState
TCgroupState::TCgroupState() {
    ms = new TMountState;
}

TCgroupState::~TCgroupState() {
    for (auto c : root_cgroups)
        delete c.second;
    root_cgroups.clear();

    delete ms;
}

void TCgroupState::UpdateFromProcfs() {
    for (auto c : root_cgroups)
        delete c.second;
    root_cgroups.clear();
    
    ms->UpdateFromProcfs();

    for (auto m : ms->Mounts()) {
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

        set<TController*> cg_controllers;
        for (auto c : cs) {
            controllers[c] = new TController(name);
            cg_controllers.insert(controllers[c]);
        }

        TRootCgroup *cg = new TRootCgroup(m, cg_controllers);
        cg->FindChildren();
        root_cgroups[name] = cg;
    }
}

void TCgroupState::MountMissingControllers() {
    for (auto c : create_subsystems) {
        if (controllers[c] == nullptr) {
            TController *controller = new TController(c);
            TRootCgroup *cg = new TRootCgroup({controller});

            cg->Attach();

            root_cgroups[c] = cg;
            controllers[c] = controller;
        }
    }
}

void TCgroupState::MountMissingTmpfs(string tmpfs) {
    ms->UpdateFromProcfs();

    for (auto m : ms->Mounts())
        if (m->Mountpoint() == tmpfs)
            return;

    TMount mount("cgroup", tmpfs, "tmpfs", 0, set<string>{});
    mount.Mount();
}

void TCgroupState::UmountAll() {
    for (auto root : root_cgroups) {
        root.second->Detach();
    }
}

ostream& operator<<(ostream& os, const TCgroupState& st) {
    for (auto ss : st.root_cgroups) {
        os << ss.first << ":" << endl;
        os << *ss.second << endl;
    }

    return os;
}
