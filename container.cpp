#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <sstream>
#include <memory>

#include "container.hpp"
#include "task.hpp"
#include "cgroup.hpp"
#include "registry.hpp"
#include "log.hpp"
#include "util/string.hpp"

extern "C" {
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
}

using namespace std;

// Data

struct TData {
    static string State(TContainer& c) {
        c.UpdateState();

        switch (c.state) {
        case TContainer::Stopped:
            return "stopped";
        case TContainer::Dead:
            return "dead";
        case TContainer::Running:
            return "running";
        case TContainer::Paused:
            return "paused";
        default:
            return "unknown";
        }
    }

    static string RootPid(TContainer& c) {
        if (c.task)
            return to_string(c.task->GetPid());
        else
            return "-1";
    };

    static string ExitStatus(TContainer& c) {
        if (c.task && !c.task->IsRunning()) {
            TExitStatus status = c.task->GetExitStatus();
            stringstream ss;
            ss << status.error << " " << status.signal << " " << status.status;
            return ss.str();
        }
        else
            return "-1";
    };

    static string Stdout(TContainer& c) {
        if (c.task)
            return c.task->GetStdout();
        return "";
    };

    static string Stderr(TContainer& c) {
        if (c.task)
            return c.task->GetStderr();
        return "";
    };

    static string CpuUsage(TContainer& c) {
        auto subsys = TSubsystem::Cpuacct();
        auto cg = c.GetCgroup(subsys);
        if (!cg) {
            TLogger::LogAction("cpuacct cgroup not found");
            return "-1";
        }

        uint64_t val;
        TError error = subsys->Usage(cg, val);
        if (error) {
            TLogger::LogError(error, "Can't get CPU usage");
            return "-1";
        }

        return to_string(val);
    };

    static string MemUsage(TContainer& c) {
        auto subsys = TSubsystem::Memory();
        auto cg = c.GetCgroup(subsys);
        if (!cg) {
            TLogger::LogAction("memory cgroup not found");
            return "-1";
        }

        uint64_t val;
        TError error = subsys->Usage(cg, val);
        if (error) {
            TLogger::LogError(error, "Can't get CPU usage");
            return "-1";
        }

        return to_string(val);
    };
};

std::map<std::string, const TDataSpec> dataSpec = {
    { "state", { "container state", TData::State } },
    { "exit_status", { "container exit status", TData::ExitStatus } },
    { "root_pid", { "root process id", TData::RootPid } },
    { "stdout", { "return task stdout", TData::Stdout } },
    { "stderr", { "return task stderr", TData::Stderr } },
    { "cpu_usage", { "return consumed CPU time in nanoseconds", TData::CpuUsage } },
    { "mem_usage", { "return consumed memory in bytes", TData::MemUsage } },
};

// TContainer

bool TContainer::CheckState(EContainerState expected) {
    if (state == Running && (!task || !task->IsRunning()))
        state = Stopped;

    return state == expected;
}

TContainer::~TContainer() {
    if (state == Paused)
        Resume();

    Stop();
}

string TContainer::Name() {
    return name;
}

bool TContainer::IsRoot() {
    return name == RootName;
}

vector<pid_t> TContainer::Processes() {
    auto cg = TCgroup::Get(name, TCgroup::GetRoot(TSubsystem::Freezer()));

    vector<pid_t> ret;
    cg->GetProcesses(ret);
    return ret;
}

bool TContainer::IsAlive() {
    return IsRoot() || !Processes().empty();
}

void TContainer::UpdateState() {
    if (state == Running && !IsAlive()) {
        Stop();
        state = Dead;
    }
}

TError TContainer::PrepareCgroups() {
    if (IsRoot()) {
        leaf_cgroups.push_back(TCgroup::GetRoot(TSubsystem::Cpuacct()));
        leaf_cgroups.push_back(TCgroup::GetRoot(TSubsystem::Memory()));
        leaf_cgroups.push_back(TCgroup::GetRoot(TSubsystem::Freezer()));
    } else {
        leaf_cgroups.push_back(TCgroup::Get(name, TCgroup::GetRoot(TSubsystem::Cpuacct())));
        leaf_cgroups.push_back(TCgroup::Get(name, TCgroup::GetRoot(TSubsystem::Memory())));
        leaf_cgroups.push_back(TCgroup::Get(name, TCgroup::GetRoot(TSubsystem::Freezer())));
    }

    for (auto cg : leaf_cgroups) {
        auto ret = cg->Create();
        if (ret) {
            leaf_cgroups.clear();
            return ret;
        }
    }

    return TError::Success();
}

TError TContainer::PrepareTask() {
    TTaskEnv taskEnv(spec.Get("command"), spec.Get("cwd"), spec.Get("root"), spec.Get("user"), spec.Get("group"), spec.Get("env"));
    TError error = taskEnv.Prepare();
    if (error)
        return error;

    task = unique_ptr<TTask>(new TTask(taskEnv, leaf_cgroups));
    return TError::Success();
}

TError TContainer::Start() {
    if (!CheckState(Stopped))
        return TError(EError::InvalidValue, "invalid container state");

    TError error = PrepareCgroups();
    if (error) {
        TLogger::LogError(error, "Can't prepare task cgroups");
        return error;
    }

    if (IsRoot()) {
        state = Running;
        return TError::Success();
    }

    if (!spec.Get("command").length())
        return TError(EError::InvalidValue, "invalid container command");

    error = PrepareTask();
    if (error) {
        TLogger::LogError(error, "Can't prepare task");
        return error;
    }

    error = task->Start();
    if (error) {
        leaf_cgroups.clear();
        TLogger::LogError(error, "Can't start task");
        return error;
    }

    spec.SetInternal("root_pid", to_string(task->GetPid()));
    state = Running;

    return TError::Success();
}

TError TContainer::KillAll() {
    auto cg = TCgroup::Get(name, TCgroup::GetRoot(TSubsystem::Freezer()));

    vector<pid_t> reap;
    TError error = cg->GetTasks(reap);
    if (error) {
        TLogger::LogError(error, "Can't read tasks list while stopping container");
        return error;
    }

    // try to stop all tasks gracefully
    cg->Kill(SIGTERM);

    // then kill any task that didn't want to stop via SIGTERM;
    // freeze all container tasks to make sure no one forks and races with us
    TSubsystem::Freezer()->Freeze(*cg);
    error = cg->GetTasks(reap);
    if (error) {
        TLogger::LogError(error, "Can't read tasks list while stopping container");
        return error;
    }
    cg->Kill(SIGKILL);
    TSubsystem::Freezer()->Unfreeze(*cg);

    // after we killed all tasks, collect and ignore their exit status
    for (auto pid : reap) {
        TTask t(pid);
        TError error = t.Reap(true);
        TLogger::LogError(error, "Can't reap task " + to_string(pid));
    }

    task = nullptr;

    return TError::Success();
}

TError TContainer::Stop() {
    if (IsRoot() || !(CheckState(Running) || CheckState(Dead)))
        return TError(EError::InvalidValue, "invalid container state");

    TError error = KillAll();
    if (error)
        TLogger::LogError(error, "Can't kill all tasks in container");

    leaf_cgroups.clear();

    state = Stopped;

    return TError::Success();
}

TError TContainer::Pause() {
    if (IsRoot() || !CheckState(Running))
        return TError(EError::InvalidValue, "invalid container state");

    auto cg = TCgroup::Get(name, TCgroup::GetRoot(TSubsystem::Freezer()));
    TSubsystem::Freezer()->Freeze(*cg);

    state = Paused;
    return TError::Success();
}

TError TContainer::Resume() {
    if (!CheckState(Paused))
        return TError(EError::InvalidValue, "invalid container state");

    auto cg = TCgroup::Get(name, TCgroup::GetRoot(TSubsystem::Freezer()));
    TSubsystem::Freezer()->Unfreeze(*cg);

    state = Running;
    return TError::Success();
}

TError TContainer::GetData(const string &name, string &value) {
    if (dataSpec.find(name) == dataSpec.end())
        return TError(EError::InvalidValue, "invalid container state");

    value = dataSpec[name].Handler(*this);
    return TError::Success();
}

TError TContainer::GetProperty(const string &property, string &value) {
    value = spec.Get(property);
    return TError::Success();
}

TError TContainer::SetProperty(const string &property, const string &value) {
    if (IsRoot())
        return TError(EError::InvalidValue, "Can't set property for root");

    if (task && task->IsRunning() && !spec.IsDynamic(property))
        return TError(EError::InvalidValue, "Can't set dynamic property " + property + " for running container");

    return spec.Set(property, value);
}

TError TContainer::Restore(const kv::TNode &node) {
    TError error = spec.Restore(node);
    if (error) {
        TLogger::LogError(error, "Can't restore task's spec");
        return error;
    }

    error = PrepareCgroups();
    if (error) {
        TLogger::LogError(error, "Can't restore task cgroups");
        return error;
    }

    int pid;
    error = StringToInt(spec.GetInternal("root_pid"), pid);
    if (error)
        pid = 0;

    TLogger::Log(name + ": restore process " + to_string(pid));

    state = Stopped;

    // if we didn't start container, make sure nobody is running
    if (pid <= 0) {
        TError error = KillAll();
        if (error)
            return error;
    }

    error = PrepareTask();
    if (error) {
        TLogger::LogError(error, "Can't prepare task");
        return error;
    }

    error = task->Restore(pid);
    if (error) {
        task = nullptr;
        (void)KillAll();

        TLogger::LogError(error, "Can't seize task");
        return error;
    }

    error = task->ValidateCgroups();
    if (error) {
        task = nullptr;
        (void)KillAll();

        TLogger::LogError(error, "Can't validate task cgroups");
        return error;
    }

    state = task->IsRunning() ? Running : Stopped;

    return TError::Success();
}

std::shared_ptr<TCgroup> TContainer::GetCgroup(shared_ptr<TSubsystem> subsys) {
    if (name == RootName)
        return TCgroup::GetRoot(subsys);
    else
        return TCgroup::Get(name, TCgroup::GetRoot(subsys));
}

bool TContainer::DeliverExitStatus(int pid, int status) {
    if (!task)
        return false;

    if (task->GetPid() != pid)
        return false;

    task->DeliverExitStatus(status);
    return true;
}

// TContainerHolder

TError TContainerHolder::CreateRoot() {
    TError error = Create(RootName);
    if (error)
        return error;

    auto root = Get(RootName);
    root->Start();

    return TError::Success();
}

bool TContainerHolder::ValidName(const string &name) {
    if (name == RootName)
        return true;

    return find_if(name.begin(), name.end(),
                   [](const char c) -> bool {
                        return !(isalnum(c) || c == '_');
                   }) == name.end();
}

TError TContainerHolder::Create(const string &name) {
    if (!ValidName(name))
        return TError(EError::InvalidValue, "invalid container name " + name);

    if (containers[name] == nullptr) {
        containers[name] = make_shared<TContainer>(name);
        return TError::Success();
    } else
        return TError(EError::InvalidValue, "container " + name + " already exists");
}

shared_ptr<TContainer> TContainerHolder::Get(const string &name) {
    if (containers.find(name) == containers.end())
        return shared_ptr<TContainer>();

    return containers[name];
}

void TContainerHolder::Destroy(const string &name) {
    if (name != RootName)
        containers.erase(name);
}

vector<string> TContainerHolder::List() {
    vector<string> ret;

    for (auto c : containers)
        ret.push_back(c.second->Name());

    return ret;
}

TError TContainerHolder::Restore(const std::string &name, const kv::TNode &node) {
    // TODO: we DO trust data from the persistent storage, do we?
    auto c = make_shared<TContainer>(name);
    auto e = c->Restore(node);
    if (e)
        return e;

    containers[name] = c;
    return TError::Success();
}

bool TContainerHolder::DeliverExitStatus(int pid, int status) {
    for (auto c : containers)
        if (c.second->DeliverExitStatus(pid, status))
            return true;

    return false;
}
