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

TCgroup::TCgroup(string name, shared_ptr<TCgroup> parent, int level) :
    name(name), parent(parent), level(level) {
}

TCgroup::~TCgroup() {
}

string TCgroup::Name() {
    return name;
}

void TCgroup::FindChildren() {
    TFolder f(Path());

    // for (auto s : f.Subfolders()) {
    //     auto cg = make_shared<TCgroup>(s, this, level + 1);
    //     cg->FindChildren();
    //     children.push_back(weak_ptr<TCgroup>(cg));
    // }
}

string TCgroup::Path() {
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

TError TCgroup::Attach(int pid) {
    TFile f(Path() + "/cgroup.procs");
    return f.AppendString(to_string(pid));
}

ostream& operator<<(ostream& os, const TCgroup& cg) {
    os << string(4 * cg.level, ' ') << cg.name << " {" << endl;

    for (auto c : cg.children) {
        os << *c.lock() << endl;
    }

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
TRootCgroup::TRootCgroup(shared_ptr<TMount> mount,
                         set<TController*> controllers) :
    TCgroup("/", shared_ptr<TCgroup>(nullptr), 0), mount(mount), controllers(controllers) {
}

TRootCgroup::TRootCgroup(set<TController*> controllers) :
    TCgroup("/", shared_ptr<TCgroup>(nullptr), 0), controllers(controllers) {

    set<string> flags;

    for (auto c : controllers)
        flags.insert(c->Name());
    
    mount = make_shared<TMount>("cgroup", tmpfs + "/" + CommaSeparatedList(flags),
                                "cgroup", 0, flags);
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
    TMountState ms;

    for (auto m : ms.Mounts()) {
        set<string> flags = m->Flags();
        set<string> cs;

        set_intersection(flags.begin(), flags.end(),
                         subsystems.begin(), subsystems.end(),
                         inserter(cs, cs.begin()));

        if (cs.size() == 0)
            continue;

        string name = CommaSeparatedList(cs);

        set<TController*> cg_controllers;
        for (auto c : cs) {
            controllers[c] = new TController(name);
            cg_controllers.insert(controllers[c]);
        }

        root_cgroups[name] = make_shared<TRootCgroup>(m, cg_controllers);
        root_cgroups[name]->FindChildren();
    }
}

TCgroupState::~TCgroupState() {
    root_cgroups.clear();
}

void TCgroupState::MountMissingControllers() {
    for (auto c : create_subsystems) {
        if (controllers[c] == nullptr) {
            TController *controller = new TController(c);
            set<TController*> tmp = {controller};
            auto cg = make_shared<TRootCgroup>(tmp);

            cg->Attach();

            root_cgroups[c] = cg;
            controllers[c] = controller;
        }
    }
}

void TCgroupState::MountMissingTmpfs(string tmpfs) {
    TMountState ms;

    for (auto m : ms.Mounts())
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
