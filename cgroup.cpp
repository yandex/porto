#include <iostream>
#include <sstream>
#include <algorithm>
#include <csignal>

#include "cgroup.hpp"
#include "subsystem.hpp"
#include "task.hpp"
#include "log.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"

using namespace std;

// TCgroup
TCgroup::TCgroup(const vector<shared_ptr<TSubsystem>> subsystems,
                 const std::shared_ptr<TMount> m) :
    name("/"), parent(shared_ptr<TCgroup>(nullptr)) {

    if (m)
        mount = m;
    else {
        set<string> flags;

        for (auto c : subsystems)
            flags.insert(c->Name());

        mount = make_shared<TMount>("cgroup", tmpfs + "/" +
                                    CommaSeparatedList(flags),
                                    "cgroup", flags);
    }
}

TCgroup::~TCgroup() {
    Remove();
}

shared_ptr<TCgroup> TCgroup::GetChild(const std::string& name) {
    vector<weak_ptr<TCgroup>>::iterator iter;
    for (iter = children.begin(); iter != children.end();) {
        if (auto child = iter->lock()) {
            if (child->name == name)
                return child;
        } else {
            iter = children.erase(iter);
            continue;
        }
        iter++;
    }

    auto child = make_shared<TCgroup>(name, shared_from_this());
    children.push_back(weak_ptr<TCgroup>(child));
    return child;
}

TError TCgroup::FindChildren(std::vector<std::shared_ptr<TCgroup>> &cglist) {
    TFolder f(Path());
    vector<string> list;

    // Ignore non-porto subtrees
    if (parent && parent->IsRoot() && name != PORTO_ROOT_CGROUP)
        return TError::Success();

    TError error = f.Subfolders(list);
    if (error)
        return error;

    for (auto s : list) {
        auto cg = GetChild(s);

        TError error = cg->FindChildren(cglist);
        if (error)
            return error;
    }

    cglist.push_back(shared_from_this());

    return TError::Success();
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
        return mount->GetMountpoint();
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

TError TCgroup::Remove() {
    if (IsRoot()) {
        TError error = mount->Umount();
        TLogger::LogError(error, "Can't umount root cgroup for root container");
        if (error)
            return error;
    } else {
        // at this point we should have gracefully terminated all tasks
        // in the container; if anything is still alive we have no other choice
        // but to kill it with SIGKILL
        int ret = RetryFailed(CGROUP_REMOVE_TIMEOUT_S * 10, 100,
                              [&]{ Kill(SIGKILL);
                                   return !IsEmpty(); });

        if (ret)
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
         for (auto name : mount->GetFlags()) {
             auto subsys = TSubsystem::Get(name);
             if (!subsys)
                 continue;

             auto root = subsys->GetRootCgroup(mount);
             cgroups.push_back(root);

             TError error = root->FindChildren(cgroups);
             if (error) {
                 TLogger::LogError(error, "Can't find children for " + root->Relpath());
                 return error;
             }
         }
     }

    return TError::Success();
}
