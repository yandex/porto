#include <iostream>
#include <sstream>
#include <algorithm>

#include "cgroup.hpp"
#include "task.hpp"
#include "folder.hpp"
#include "registry.hpp"

using namespace std;

// TCgroup
shared_ptr<TCgroup> TCgroup::Get(const string &name, const shared_ptr<TCgroup> &parent) {
    return TRegistry<TCgroup>::Get(TCgroup(name, parent));
}

shared_ptr<TCgroup> TCgroup::GetRoot(const std::shared_ptr<TMount> mount, const std::vector<std::shared_ptr<TSubsystem>> subsystems) {
    return TRegistry<TCgroup>::Get(TCgroup(mount, subsystems));
}

shared_ptr<TCgroup> TCgroup::GetRoot(const shared_ptr<TSubsystem> subsystem) {
    return TRegistry<TCgroup>::Get(TCgroup({subsystem}));
}

TCgroup::TCgroup(const string &name, const shared_ptr<TCgroup> parent, int level) :
    name(name), parent(parent), level(level) {
}

TCgroup::TCgroup(const shared_ptr<TMount> mount, const vector<shared_ptr<TSubsystem>> subsystems) :
    name("/"), parent(shared_ptr<TCgroup>(nullptr)), level(0), mount(mount),
    subsystems(subsystems) {
}

TCgroup::TCgroup(const vector<shared_ptr<TSubsystem>> subsystems) :
    name("/"), parent(shared_ptr<TCgroup>(nullptr)), level(0),
    subsystems(subsystems) {

    set<string> flags;

    for (auto c : subsystems)
        flags.insert(c->Name());

    mount = make_shared<TMount>("cgroup", tmpfs + "/" +
                                CommaSeparatedList(flags),
                                "cgroup", 0, flags);
}

TCgroup::~TCgroup() {
    if (need_cleanup)
        Remove();
}

vector<shared_ptr<TCgroup> > TCgroup::FindChildren() {
    TFolder f(Path());
    vector<shared_ptr<TCgroup> > ret;
    auto self = TRegistry<TCgroup>::Get(*this);
    vector<string> list;

    TError error = f.Subfolders(list);
    if (error) {
        // TODO: handle error
    }

    for (auto s : list) {
        auto cg = TRegistry<TCgroup>::Get(TCgroup(s, self, level + 1));

        children.push_back(weak_ptr<TCgroup>(cg));
        for (auto c : cg->FindChildren())
            ret.push_back(c);
    }

    ret.push_back(self);

    return ret;
}

TError TCgroup::GetProcesses(vector<pid_t> &processes) {
    vector<string> lines;
    TError ret = GetKnobValueAsLines("cgroup.procs", lines);
    if (ret)
        return ret;
    return StringsToIntegers(lines, processes);
}

TError TCgroup::GetTasks(vector<pid_t> &tasks) {
    vector<string> lines;
    TError ret = GetKnobValueAsLines("tasks", lines);
    if (ret)
        return ret;
    return StringsToIntegers(lines, tasks);
}

bool TCgroup::IsEmpty() {
    vector<pid_t> tasks;
    GetTasks(tasks);
    return tasks.empty();
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

TError TCgroup::Create() {
    if (IsRoot()) {
        TMountSnapshot ms;

        TMount root("cgroup", tmpfs, "tmpfs", 0, set<string>{});
        bool mount_root = true;

        for (auto m : ms.Mounts()) {
            if (*m == root)
                mount_root = false;
            if (*m == *mount)
                return TError();
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

    return TError();
}

TError TCgroup::Remove() {
    if (IsRoot()) {
        mount->Umount();
    } else {
        while (!IsEmpty())
            Kill(SIGINT);
    }

    TFolder f(Path());
    f.Remove();

    return TError();
}

TError TCgroup::Kill(int signal) {
    if (!IsRoot()) {
        vector<pid_t> tasks;
        if (!GetTasks(tasks)) {
            for (auto pid : tasks) {
                TTask task(pid);
                task.Kill(signal);
            }
        }
    }
    return TError();
}

TError TCgroup::GetKnobValue(const std::string &knob, std::string &value) {
    TFile f(Path() + "/" + knob);
    return f.AsString(value);
}

TError TCgroup::GetKnobValueAsLines(const std::string &knob, vector<string> &lines) {
    TFile f(Path() + "/" + knob);
    return f.AsLines(lines);
}

TError TCgroup::SetKnobValue(const std::string &knob, const std::string &value, bool append) {
    TFile f(Path() + "/" + knob);

    if (append)
        return f.AppendString(value);
    else
        return f.WriteStringNoAppend(value);
}

TError TCgroup::Attach(int pid) {
    if (!IsRoot())
        return SetKnobValue("cgroup.procs", to_string(pid), true);

    return TError();
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
            subsystems[c] = TSubsystem::Get(name);
            cg_controllers.push_back(subsystems[c]);
        }

        auto root = TCgroup::GetRoot(mount, cg_controllers);
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
