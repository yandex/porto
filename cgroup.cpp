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

using namespace std;

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

        Mount = make_shared<TMount>("cgroup", Tmpfs + "/" +
                                    CommaSeparatedList(flags),
                                    "cgroup", flags);
    }
}

TCgroup::~TCgroup() {
    Remove();
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

    auto child = make_shared<TCgroup>(name, shared_from_this());
    Children.push_back(weak_ptr<TCgroup>(child));
    return child;
}

TError TCgroup::FindChildren(std::vector<std::shared_ptr<TCgroup>> &cglist) {
    TFolder f(Path());
    vector<string> list;

    // Ignore non-porto subtrees
    if (Parent && Parent->IsRoot() && Name != PORTO_ROOT_CGROUP)
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


        TMount root("cgroup", Tmpfs, "tmpfs", {});
        bool mountRoot = true;

        for (auto m : mounts) {
            if (*m == root)
                mountRoot = false;
            if (*m == *Mount)
                return TError::Success();
        }

        if (mountRoot) {
            TError error = root.Mount();
            TLogger::LogError(error, "Can't mount root cgroup");
            if (error)
                return error;
        }
    } else
        Parent->Create();

    TFolder f(Path());
    if (!f.Exists()) {
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
    if (IsRoot()) {
        TError error = Mount->Umount();
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
            TLogger::Log() << "Can't kill all tasks in cgroup " << Path() << endl;
    }

    TFolder f(Path());
    TError error = f.Remove();
    TLogger::LogError(error, "Can't remove cgroup directory");

    return TError::Success();
}

TError TCgroup::Kill(int signal) const {
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
        TError error = SetKnobValue("cgroup.procs", to_string(pid), true);
        TLogger::LogError(error, "Can't attach " + to_string(pid) + " to " + Name);
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
             Cgroups.push_back(root);

             TError error = root->FindChildren(Cgroups);
             if (error) {
                 TLogger::LogError(error, "Can't find children for " + root->Relpath());
                 return error;
             }
         }
     }

    return TError::Success();
}
