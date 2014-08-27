#include <algorithm>
#include <sstream>
#include <memory>
#include <csignal>

#include "container.hpp"
#include "task.hpp"
#include "cgroup.hpp"
#include "log.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"

extern "C" {
#include <sys/types.h>
#include <unistd.h>
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
        auto cg = c.GetLeafCgroup(subsys);
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
        auto cg = c.GetLeafCgroup(subsys);
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
    { "state", { "container state", true, TData::State, { EContainerState::Stopped, EContainerState::Dead, EContainerState::Running, EContainerState::Paused } } },
    { "exit_status", { "container exit status", false, TData::ExitStatus, { EContainerState::Dead } } },
    { "start_errno", { "container start error", false, TData::StartErrno, { EContainerState::Stopped } } },
    { "root_pid", { "root process id", false, TData::RootPid, { EContainerState::Running, EContainerState::Paused } } },
    { "stdout", { "return task stdout", false, TData::Stdout, { EContainerState::Running, EContainerState::Paused, EContainerState::Dead } } },
    { "stderr", { "return task stderr", false, TData::Stderr, { EContainerState::Running, EContainerState::Paused, EContainerState::Dead } } },
    { "cpu_usage", { "return consumed CPU time in nanoseconds", true, TData::CpuUsage, { EContainerState::Running, EContainerState::Paused, EContainerState::Dead } } },
    { "memory_usage", { "return consumed memory in bytes", true, TData::MemUsage, { EContainerState::Running, EContainerState::Paused, EContainerState::Dead } } },
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
    auto cg = GetLeafCgroup(TSubsystem::Freezer());

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
    leaf_cgroups.push_back(GetLeafCgroup(TSubsystem::Cpuacct()));
    leaf_cgroups.push_back(GetLeafCgroup(TSubsystem::Memory()));
    leaf_cgroups.push_back(GetLeafCgroup(TSubsystem::Freezer()));

    for (auto cg : leaf_cgroups) {
        auto ret = cg->Create();
        if (ret) {
            leaf_cgroups.clear();
            return ret;
        }
    }

    auto memroot = TCgroupRegistry::GetRoot(TSubsystem::Memory());
    auto memcg = GetLeafCgroup(TSubsystem::Memory());

    if (memroot->HasKnob("memory.low_limit_in_bytes")) {
        TError error = memcg->SetKnobValue("memory.low_limit_in_bytes", spec.Get("memory_guarantee"), false);
        TLogger::LogError(error, "Can't set memory_guarantee");
        if (error)
            return error;
    }

    TError error = memcg->SetKnobValue("memory.limit_in_bytes", spec.Get("memory_limit"), false);
    TLogger::LogError(error, "Can't set memory_limit");
    if (error)
        return error;

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

TError TContainer::Create() {
    return spec.Create();
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
        return TError(EError::InvalidValue, "container command is empty");

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
    auto cg = GetLeafCgroup(TSubsystem::Freezer());

    TLogger::Log("killall " + name);

    vector<pid_t> reap;
    TError error = cg->GetTasks(reap);
    if (error) {
        TLogger::LogError(error, "Can't read tasks list while stopping container");
        return error;
    }

    // try to stop all tasks gracefully
    cg->Kill(SIGTERM);

    SleepWhile(100, [&]{ return !cg->IsEmpty(); });

    // then kill any task that didn't want to stop via SIGTERM;
    // freeze all container tasks to make sure no one forks and races with us
    error = TSubsystem::Freezer()->Freeze(*cg);
    if (error)
        TLogger::LogError(error, "Can't kill all tasks");

    error = cg->GetTasks(reap);
    if (error) {
        TLogger::LogError(error, "Can't read tasks list while stopping container");
        return error;
    }
    cg->Kill(SIGKILL);
    error = TSubsystem::Freezer()->Unfreeze(*cg);
    if (error)
        TLogger::LogError(error, "Can't kill all tasks");

    // after we killed all tasks, collect and ignore their exit status
    for (auto pid : reap) {
        TTask t(pid);
        if (t.CanReap()) {
            TError error = t.Reap(true);
            TLogger::LogError(error, "Can't reap task " + to_string(pid));
        }
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

    auto cg = GetLeafCgroup(TSubsystem::Freezer());
    TError error(TSubsystem::Freezer()->Freeze(*cg));
    if (error) {
        TLogger::LogError(error, "Can't pause " + name);
        return error;
    }

    state = EContainerState::Paused;
    return TError::Success();
}

TError TContainer::Resume() {
    if (!CheckState(EContainerState::Paused))
        return TError(EError::InvalidValue, "invalid container state");

    auto cg = GetLeafCgroup(TSubsystem::Freezer());
    TError error(TSubsystem::Freezer()->Unfreeze(*cg));
    if (error) {
        TLogger::LogError(error, "Can't resume " + name);
        return error;
    }


    state = EContainerState::Running;
    return TError::Success();
}

TError TContainer::GetData(const string &name, string &value) {
    if (dataSpec.find(name) == dataSpec.end())
        return TError(EError::InvalidValue, "invalid container data");

    if (IsRoot() && !dataSpec[name].root_valid)
        return TError(EError::InvalidData, "invalid data for root container");

    if (dataSpec[name].valid.find(state) == dataSpec[name].valid.end())
        return TError(EError::InvalidState, "invalid container state");

    value = dataSpec[name].handler(*this);
    return TError::Success();
}

TError TContainer::GetProperty(const string &property, string &value) {
    if (IsRoot())
        return TError(EError::InvalidProperty, "no properties for root container");

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
    bool started = true;
    error = StringToInt(spec.GetInternal("root_pid"), pid);
    if (error)
        started = false;

    TLogger::Log(name + ": restore process " + to_string(pid));

    state = EContainerState::Stopped;

    if (started) {
        error = PrepareTask();
        if (error) {
            TLogger::LogError(error, "Can't prepare task");
            return error;
        }

        error = task->Restore(pid);
        if (error) {
            task = nullptr;
            (void)KillAll();

            TLogger::LogError(error, "Can't restore task");
            return error;
        }

        state = task->IsRunning() ? EContainerState::Running : EContainerState::Stopped;
    } else {
        if (IsAlive()) {
            // we started container but died before saving root_pid,
            // state may be inconsistent so restart task

            (void)KillAll();
            return Start();
        } else {
            // if we didn't start container, make sure nobody is running

            (void)KillAll();
        }
    }

    return TError::Success();
}

std::shared_ptr<TCgroup> TContainer::GetLeafCgroup(shared_ptr<TSubsystem> subsys) {
    if (name == ROOT_CONTAINER)
        return TCgroupRegistry::Get(PORTO_ROOT_CGROUP, TCgroupRegistry::GetRoot(subsys));
    else
        return TCgroupRegistry::Get(name, TCgroupRegistry::Get(PORTO_ROOT_CGROUP, TCgroupRegistry::GetRoot(subsys)));
}

bool TContainer::DeliverExitStatus(int pid, int status) {
    if (!task)
        return false;

    if (task->GetPid() != pid)
        return false;

    task->DeliverExitStatus(status);
    TLogger::Log("Delivered " + to_string(status) + " to " + name + " with root_pid " + to_string(task->GetPid()));
    state = EContainerState::Dead;
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

    if (name.length() == 0 || name.length() > 128)
        return false;

    return find_if(name.begin(), name.end(),
                   [](const char c) -> bool {
                        return !(isalnum(c) || c == '_');
                   }) == name.end();
}

TError TContainerHolder::Create(const string &name) {
    if (!ValidName(name))
        return TError(EError::InvalidValue, "invalid container name " + name);

    if (containers[name] == nullptr) {
        auto c(make_shared<TContainer>(name));
        TError error(c->Create());
        if (error)
            return error;

        containers[name] = c;
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
