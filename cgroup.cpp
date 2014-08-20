#include <iostream>
#include <sstream>
#include <algorithm>

#include "cgroup.hpp"
#include "task.hpp"
#include "registry.hpp"
#include "log.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"

using namespace std;

extern "C" {
#include <signal.h>
}

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

TCgroup::TCgroup(const vector<shared_ptr<TSubsystem>> subsystems) :
    name("/"), parent(shared_ptr<TCgroup>(nullptr)), level(0),
    subsystems(subsystems) {

    set<string> flags;

    for (auto c : subsystems)
        flags.insert(c->Name());

    mount = make_shared<TMount>("cgroup", tmpfs + "/" +
                                CommaSeparatedList(flags),
                                "cgroup", flags);
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
        auto cg = TRegistry<TCgroup>::Get(TCgroup(s, self));

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

string TCgroup::Relpath() {
    if (IsRoot())
        return "";
    else
        return parent->Relpath() + "/" + name;
}

TError TCgroup::Create() {
    if (IsRoot()) {
        TMountSnapshot ms;

        set<shared_ptr<TMount>> mounts;
        TError error = ms.Mounts(mounts);
        if (error) {
            TLogger::LogError(error, "Can't create mount snapshot");
            return error;
        }


        TMount root("cgroup", tmpfs, "tmpfs", {});
        bool mount_root = true;

        for (auto m : mounts) {
            if (*m == root)
                mount_root = false;
            if (*m == *mount)
                return TError::Success();
        }

        if (mount_root) {
            TError error = root.Mount();
            TLogger::LogError(error, "Can't mount root cgroup");
            if (error)
                return error;
        }
    } else
        parent->Create();

    TFolder f(Path());
    if (!f.Exists()) {
        TError error = f.Create(mode);
        TLogger::LogError(error, "Can't create cgroup directory");
        if (error)
            return error;
    }

    if (IsRoot()) {
        TError error = mount->Mount();
        TLogger::LogError(error, "Can't mount root cgroup for root container");
        if (error)
            return error;
    }

    return TError::Success();
}

bool TCgroup::RemoveSubtree(void) {
    if (IsRoot())
        return false;

    if (level == 1 && name == ROOT_CGROUP)
        return true;

    for (auto cg = parent; cg; cg = cg->parent)
        if (cg->level == 1 && cg->name == ROOT_CGROUP)
            return true;

    return false;
}

TError TCgroup::Remove() {
    // we don't manage anything outside /porto
    if (!RemoveSubtree())
        return TError::Success();

    if (IsRoot()) {
        TError error = mount->Umount();
        TLogger::LogError(error, "Can't umount root cgroup for root container");
        if (error)
            return error;
    } else {
        // at this point we should have gracefully terminated all tasks
        // in the container; if anything is still alive we have no other choice
        // but to kill it with SIGKILL
        int ret = RetryFailed(CGROUP_REMOVE_TIMEOUT_S * 10, 100000,
                              [=]{ Kill(SIGKILL); if (IsEmpty()) return 0; else return -1; });

        if (ret < 0)
            TLogger::Log("Can't kill all tasks in cgroup " + Path());
    }

    TFolder f(Path());
    TError error = f.Remove();
    TLogger::LogError(error, "Can't remove cgroup directory");

    return TError::Success();
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
    return TError::Success();
}

bool TCgroup::HasKnob(const std::string &knob) {
    TFile f(Path() + "/" + knob);
    return f.Exists();
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
    if (!IsRoot()) {
        TError error = SetKnobValue("cgroup.procs", to_string(pid), true);
        TLogger::LogError(error, "Can't attach " + to_string(pid) + " to " + name);
    }

    return TError::Success();
}

bool TCgroup::HasSubsystem(const string &name) {
    auto cg = parent;
    while (cg->parent)
        cg = cg->parent;

    for (auto c : cg->subsystems)
        if (c->Name() == name)
            return true;

    return false;
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
TError TCgroupSnapshot::Create() {
    TMountSnapshot ms;

    set<shared_ptr<TMount>> mounts;
    TError error = ms.Mounts(mounts);
    if (error) {
        TLogger::LogError(error, "Can't create mount snapshot");
        return error;
    }

    static set<string> supported_subsystems =
        {"cpuset", "cpu", "cpuacct", "memory",
         "devices", "freezer", "net_cls", "net_prio", "blkio",
         "perf_event", "hugetlb", "name=systemd"};

    for (auto mount : mounts) {
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

    return TError::Success();
}

ostream& operator<<(ostream& os, const TCgroupSnapshot& st) {
    for (auto ss : st.cgroups)
        if (ss->IsRoot())
            os << *ss << endl;

    return os;
}
