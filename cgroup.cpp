#include <iostream>
#include <sstream>
#include <algorithm>
#include <csignal>

#include "cgroup.hpp"
#include "subsystem.hpp"
#include "task.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"

using std::string;
using std::vector;
using std::shared_ptr;
using std::weak_ptr;
using std::set;

// TCgroup
TCgroup::TCgroup(const vector<shared_ptr<TSubsystem>> subsystems,
                 const std::shared_ptr<TMount> m) :
    Name("/"), Parent(shared_ptr<TCgroup>(nullptr)) {

    if (m)
        Mount = m;
    else {
        set<string> flags;

        for (auto c : subsystems)
            flags.insert(c->GetName());

        Mount = std::make_shared<TMount>("cgroup", SYSFS_CGROOT + "/" +
                                         CommaSeparatedList(flags),
                                         "cgroup", flags);
    }
}

TCgroup::~TCgroup() {
    TError error = Remove();
    TLogger::LogError(error, "Can't remove cgroup directory");
}

shared_ptr<TCgroup> TCgroup::GetChild(const std::string& name) {
    vector<weak_ptr<TCgroup>>::iterator iter;
    for (iter = Children.begin(); iter != Children.end();) {
        if (auto child = iter->lock()) {
            if (child->Name == name)
                return child;
        } else {
            iter = Children.erase(iter);
            continue;
        }
        iter++;
    }

    auto child = std::make_shared<TCgroup>(name, shared_from_this());
    Children.push_back(weak_ptr<TCgroup>(child));
    return child;
}

TError TCgroup::FindChildren(std::vector<std::shared_ptr<TCgroup>> &cglist) {
    TFolder f(Path());
    vector<string> list;

    TError error = f.Subfolders(list);
    if (error)
        return error;

    for (auto s : list) {
        // Ignore non-porto subtrees
        if (IsRoot() && s != PORTO_ROOT_CGROUP)
            continue;

        auto cg = GetChild(s);
        cglist.push_back(cg);

        TError error = cg->FindChildren(cglist);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TCgroup::GetProcesses(vector<pid_t> &processes) const {
    vector<string> lines;
    TError ret = GetKnobValueAsLines("cgroup.procs", lines);
    if (ret)
        return ret;
    return StringsToIntegers(lines, processes);
}

TError TCgroup::GetTasks(vector<pid_t> &tasks) const {
    vector<string> lines;
    TError ret = GetKnobValueAsLines("tasks", lines);
    if (ret)
        return ret;
    return StringsToIntegers(lines, tasks);
}

bool TCgroup::IsEmpty() const {
    vector<pid_t> tasks;
    GetTasks(tasks);
    return tasks.empty();
}

bool TCgroup::IsRoot() const {
    return !Parent;
}

string TCgroup::Path() const {
    if (IsRoot())
        return Mount->GetMountpoint();
    else
        return Parent->Path() + "/" + Name;
}

string TCgroup::Relpath() const {
    if (IsRoot())
        return "";
    else
        return Parent->Relpath() + "/" + Name;
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

        bool mountRoot = true;
        for (auto m : mounts) {
            if (m->GetMountpoint() == SYSFS_CGROOT && m->GetType() == "tmpfs")
                mountRoot = false;
            if (*m == *Mount)
                return TError::Success();
        }

        if (mountRoot) {
            TMount root("cgroup", SYSFS_CGROOT, "tmpfs", {});
            TError error = root.Mount();
            TLogger::LogError(error, "Can't mount root cgroup");
            if (error)
                return error;
        }
    } else
        Parent->Create();

    TFolder f(Path());
    if (!f.Exists()) {
        TLogger::Log() << "Create cgroup " << Path() << std::endl;

        TError error = f.Create(Mode);
        TLogger::LogError(error, "Can't create cgroup directory");
        if (error)
            return error;
    }

    if (IsRoot()) {
        TError error = Mount->Mount();
        TLogger::LogError(error, "Can't mount root cgroup for root container");
        if (error)
            return error;
    }

    return TError::Success();
}

TError TCgroup::Remove() {
    if (IsRoot())
        return TError::Success();

    // at this point we should have gracefully terminated all tasks
    // in the container; if anything is still alive we have no other choice
    // but to kill it with SIGKILL
    int ret = RetryFailed(config().daemon().cgroup_remove_timeout_s() * 10, 100,
                          [&]{ Kill(SIGKILL);
                               return !IsEmpty(); });

    if (ret)
        TLogger::Log() << "Can't kill all tasks in cgroup " << Path() << std::endl;

    TLogger::Log() << "Remove cgroup " << Path() << std::endl;
    TFolder f(Path());
    return f.Remove();
}

bool TCgroup::Exists() {
    TFolder f(Path());
    return f.Exists();
}

std::shared_ptr<TMount> TCgroup::GetMount() {
    return Mount;
}

TError TCgroup::Kill(int signal) const {
    if (!IsRoot()) {
        vector<pid_t> tasks;
        if (!GetTasks(tasks)) {
            for (auto pid : tasks) {
                TTask task(pid);
                TError error = task.Kill(signal);
                TLogger::LogError(error, "Can't kill child process");
            }
        }
    }
    return TError::Success();
}

bool TCgroup::HasKnob(const std::string &knob) const {
    TFile f(Path() + "/" + knob);
    return f.Exists();
}

TError TCgroup::GetKnobValue(const std::string &knob, std::string &value) const {
    TFile f(Path() + "/" + knob);
    return f.AsString(value);
}

TError TCgroup::GetKnobValueAsLines(const std::string &knob, vector<string> &lines) const {
    TFile f(Path() + "/" + knob);
    return f.AsLines(lines);
}

TError TCgroup::SetKnobValue(const std::string &knob, const std::string &value, bool append) const {
    TFile f(Path() + "/" + knob);

    if (append)
        return f.AppendString(value);
    else
        return f.WriteStringNoAppend(value);
}

TError TCgroup::Attach(int pid) const {
    if (!IsRoot()) {
        TError error = SetKnobValue("cgroup.procs", std::to_string(pid), true);
        TLogger::LogError(error, "Can't attach " + std::to_string(pid) + " to " + Name);
    }

    return TError::Success();
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

     for (auto mount : mounts) {
         for (auto name : mount->GetData()) {
             auto subsys = TSubsystem::Get(name);
             if (!subsys)
                 continue;

             auto root = subsys->GetRootCgroup(mount);
             TError error = root->FindChildren(Cgroups);
             if (error) {
                 TLogger::LogError(error, "Can't find children for " + root->Relpath());
                 return error;
             }
         }
     }

    return TError::Success();
}

void TCgroupSnapshot::Destroy() {
    Cgroups.clear();
}
