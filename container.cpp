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
#include <signal.h>
}

using namespace std;

// Data

struct TData {
    static string State(TContainer& c) {
        c.UpdateState();

        switch (c.state) {
        case EContainerState::Stopped:
            return "stopped";
        case EContainerState::Dead:
            return "dead";
        case EContainerState::Running:
            return "running";
        case EContainerState::Paused:
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
            return to_string(status.status);
        }
        else
            return "-1";
    };

    static string StartErrno(TContainer& c) {
        if (c.task && !c.task->IsRunning()) {
            TExitStatus status = c.task->GetExitStatus();
            return to_string(status.error);
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
    { "state", { "container state", TData::State, { EContainerState::Stopped, EContainerState::Dead, EContainerState::Running, EContainerState::Paused } } },
    { "exit_status", { "container exit status", TData::ExitStatus, { EContainerState::Dead } } },
    { "start_errno", { "container start error", TData::StartErrno, { EContainerState::Stopped } } },
    { "root_pid", { "root process id", TData::RootPid, { EContainerState::Running } } },
    { "stdout", { "return task stdout", TData::Stdout, { EContainerState::Running, EContainerState::Dead } } },
    { "stderr", { "return task stderr", TData::Stderr, { EContainerState::Running, EContainerState::Dead } } },
    { "cpu_usage", { "return consumed CPU time in nanoseconds", TData::CpuUsage, { EContainerState::Running, EContainerState::Dead } } },
    { "memory_usage", { "return consumed memory in bytes", TData::MemUsage, { EContainerState::Running, EContainerState::Dead } } },
};

// TContainer

bool TContainer::CheckState(EContainerState expected) {
    if (state == EContainerState::Running && (!task || !task->IsRunning()))
        state = EContainerState::Stopped;

    return state == expected;
}

TContainer::~TContainer() {
    if (state == EContainerState::Paused)
        Resume();

    TLogger::Log("stop " + name);

    Stop();
}

string TContainer::Name() {
    return name;
}

bool TContainer::IsRoot() {
    return name == ROOT_CONTAINER;
}

vector<pid_t> TContainer::Processes() {
    auto cg = GetCgroup(TSubsystem::Freezer());

    vector<pid_t> ret;
    cg->GetProcesses(ret);
    return ret;
}

bool TContainer::IsAlive() {
    return IsRoot() || !Processes().empty();
}

void TContainer::UpdateState() {
    if (state == EContainerState::Running && !IsAlive()) {
        if (task)
            task->Reap(false);
        Stop();
        state = EContainerState::Dead;
    }
}

TError TContainer::PrepareCgroups() {
    leaf_cgroups.push_back(GetCgroup(TSubsystem::Cpuacct()));
    leaf_cgroups.push_back(GetCgroup(TSubsystem::Memory()));
    leaf_cgroups.push_back(GetCgroup(TSubsystem::Freezer()));

    for (auto cg : leaf_cgroups) {
        auto ret = cg->Create();
        if (ret) {
            leaf_cgroups.clear();
            return ret;
        }
    }

    auto memroot = TCgroup::GetRoot(TSubsystem::Memory());
    auto memcg = GetCgroup(TSubsystem::Memory());

    if (memroot->HasKnob("memory.low_limit_in_bytes")) {
        TError error = memcg->SetKnobValue("memory.low_limit_in_bytes", spec.Get("memory_guarantee"), false);
        if (error)
            return error;
    }

#if 0
    TError error = memcg->SetKnobValue("memory.limit_in_bytes", spec.Get("memory_limit"), false);
    TLogger::Log("ERR " + error.GetMsg());
    if (error) {
        return error;
    }
#endif

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
    if (!CheckState(EContainerState::Stopped))
        return TError(EError::InvalidValue, "invalid container state");

    TError error = PrepareCgroups();
    if (error) {
        TLogger::LogError(error, "Can't prepare task cgroups");
        return error;
    }

    if (IsRoot()) {
        state = EContainerState::Running;
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

    TLogger::Log(name + " started " + to_string(task->GetPid()));

    spec.SetInternal("root_pid", to_string(task->GetPid()));
    state = EContainerState::Running;

    return TError::Success();
}

TError TContainer::KillAll() {
    auto cg = GetCgroup(TSubsystem::Freezer());

    vector<pid_t> reap;
    TError error = cg->GetTasks(reap);
    if (error) {
        TLogger::LogError(error, "Can't read tasks list while stopping container");
        return error;
    }

    // try to stop all tasks gracefully
    cg->Kill(SIGTERM);

    // TODO: do we need timeout here?!

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
    if (IsRoot() || !(CheckState(EContainerState::Running) || CheckState(EContainerState::Dead)))
        return TError(EError::InvalidValue, "invalid container state");

    TError error = KillAll();
    if (error)
        TLogger::LogError(error, "Can't kill all tasks in container");

    leaf_cgroups.clear();

    state = EContainerState::Stopped;

    return TError::Success();
}

TError TContainer::Pause() {
    if (IsRoot() || !CheckState(EContainerState::Running))
        return TError(EError::InvalidValue, "invalid container state");

    auto cg = GetCgroup(TSubsystem::Freezer());
    TSubsystem::Freezer()->Freeze(*cg);

    state = EContainerState::Paused;
    return TError::Success();
}

TError TContainer::Resume() {
    if (!CheckState(EContainerState::Paused))
        return TError(EError::InvalidValue, "invalid container state");

    auto cg = GetCgroup(TSubsystem::Freezer());
    TSubsystem::Freezer()->Unfreeze(*cg);

    state = EContainerState::Running;
    return TError::Success();
}

TError TContainer::GetData(const string &name, string &value) {
    if (dataSpec.find(name) == dataSpec.end())
        return TError(EError::InvalidValue, "invalid container data");

    if (dataSpec[name].valid.find(state) == dataSpec[name].valid.end())
        return TError(EError::InvalidState, "invalid container state");

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

    state = EContainerState::Stopped;

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

    state = task->IsRunning() ? EContainerState::Running : EContainerState::Stopped;

    return TError::Success();
}

std::shared_ptr<TCgroup> TContainer::GetCgroup(shared_ptr<TSubsystem> subsys) {
    if (name == ROOT_CONTAINER)
        return TCgroup::Get(ROOT_CGROUP, TCgroup::GetRoot(subsys));
    else
        return TCgroup::Get(name, TCgroup::Get(ROOT_CGROUP, TCgroup::GetRoot(subsys)));
}

bool TContainer::DeliverExitStatus(int pid, int status) {
    if (!task)
        return false;

    if (task->GetPid() != pid)
        return false;

    task->DeliverExitStatus(status);
    return true;
}

void TContainer::Heartbeat() {
    UpdateState();
    if (task)
        task->Rotate();
}

// TContainerHolder

TError TContainerHolder::CreateRoot() {
    TError error = Create(ROOT_CONTAINER);
    if (error)
        return error;

    auto root = Get(ROOT_CONTAINER);
    root->Start();

    return TError::Success();
}

bool TContainerHolder::ValidName(const string &name) {
    if (name == ROOT_CONTAINER)
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
    if (name != ROOT_CONTAINER)
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

void TContainerHolder::Heartbeat() {
    for (auto c : containers)
        c.second->Heartbeat();
}
