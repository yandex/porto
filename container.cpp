#include <algorithm>
#include <sstream>
#include <memory>
#include <csignal>
#include <cstdlib>

#include "container.hpp"
#include "task.hpp"
#include "cgroup.hpp"
#include "subsystem.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"

extern "C" {
#include <sys/types.h>
#include <unistd.h>
#include <sys/time.h>
}

using namespace std;

// Data

struct TData {
    static string State(TContainer& c) {
        switch (c.State) {
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
        if (c.Task)
            return to_string(c.Task->GetPid());
        else
            return "-1";
    };

    static string ExitStatus(TContainer& c) {
        if (c.Task && !c.Task->IsRunning()) {
            TExitStatus status = c.Task->GetExitStatus();
            return to_string(status.Status);
        }
        else
            return "-1";
    };

    static string StartErrno(TContainer& c) {
        if (c.Task && !c.Task->IsRunning()) {
            TExitStatus status = c.Task->GetExitStatus();
            return to_string(status.Error);
        }
        else
            return "-1";
    };

    static string Stdout(TContainer& c) {
        if (c.Task)
            return c.Task->GetStdout();
        return "";
    };

    static string Stderr(TContainer& c) {
        if (c.Task)
            return c.Task->GetStderr();
        return "";
    };

    static string CpuUsage(TContainer& c) {
        auto subsys = cpuacctSubsystem;
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
        auto subsys = memorySubsystem;
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
    if (State == EContainerState::Running && (!Task || !Task->IsRunning()))
        State = EContainerState::Stopped;

    return State == expected;
}

TContainer::~TContainer() {
    if (State == EContainerState::Paused)
        Resume();

    Stop();
}

const string &TContainer::GetName() const {
    return Name;
}

bool TContainer::IsRoot() const {
    return Name == ROOT_CONTAINER;
}

vector<pid_t> TContainer::Processes() {
    auto cg = GetLeafCgroup(freezerSubsystem);

    vector<pid_t> ret;
    cg->GetProcesses(ret);
    return ret;
}

bool TContainer::IsAlive() {
    return IsRoot() || !Processes().empty();
}

TError TContainer::PrepareCgroups() {
    LeafCgroups[cpuacctSubsystem] = GetLeafCgroup(cpuacctSubsystem);
    LeafCgroups[memorySubsystem] = GetLeafCgroup(memorySubsystem);
    LeafCgroups[freezerSubsystem] = GetLeafCgroup(freezerSubsystem);

    for (auto cg : LeafCgroups) {
        auto ret = cg.second->Create();
        if (ret) {
            LeafCgroups.clear();
            return ret;
        }
    }

    auto memroot = memorySubsystem->GetRootCgroup();
    auto memcg = GetLeafCgroup(memorySubsystem);

    TError error = memorySubsystem->UseHierarchy(*memcg);
    TLogger::LogError(error, "Can't set use_hierarchy for " + memcg->Relpath());
    if (error)
        return error;

    if (memroot->HasKnob("memory.low_limit_in_bytes")) {
        TError error = memcg->SetKnobValue("memory.low_limit_in_bytes", Spec.Get("memory_guarantee"), false);
        TLogger::LogError(error, "Can't set memory_guarantee");
        if (error)
            return error;
    }

    error = memcg->SetKnobValue("memory.limit_in_bytes", Spec.Get("memory_limit"), false);
    TLogger::LogError(error, "Can't set memory_limit");
    if (error)
        return error;

    return TError::Success();
}

TError TContainer::PrepareTask() {
    TTaskEnv taskEnv(Spec.Get("command"), Spec.Get("cwd"), Spec.Get("root"), Spec.Get("user"), Spec.Get("group"), Spec.Get("env"), Spec.Get("subreaper") == "true");
    TError error = taskEnv.Prepare();
    if (error)
        return error;

    vector<shared_ptr<TCgroup>> cgroups;
    for (auto cg : LeafCgroups)
        cgroups.push_back(cg.second);
    Task = unique_ptr<TTask>(new TTask(taskEnv, cgroups));
    return TError::Success();
}

TError TContainer::Create() {
    TLogger::Log() << "Create " << Name << endl;
    return Spec.Create();
}

static string ContainerStateName(EContainerState state) {
    switch (state) {
    case EContainerState::Stopped:
        return "stopped";
    case EContainerState::Dead:
        return "dead";
    case EContainerState::Running:
        return "running";
    case EContainerState::Paused:
        return "paused";
    default:
        return "?";
    }
}

TError TContainer::Start() {
    if ((State == EContainerState::Running || State == EContainerState::Dead) && MaybeReturnedOk) {
        TLogger::Log() << "Maybe running" << endl;
        MaybeReturnedOk = false;
        return TError::Success();
    }
    MaybeReturnedOk = false;

    if (!CheckState(EContainerState::Stopped))
        return TError(EError::InvalidValue, "invalid container state " + ContainerStateName(State));

    TError error = PrepareCgroups();
    if (error) {
        TLogger::LogError(error, "Can't prepare task cgroups");
        return error;
    }

    if (IsRoot()) {
        State = EContainerState::Running;
        return TError::Success();
    }

    if (!Spec.Get("command").length())
        return TError(EError::InvalidValue, "container command is empty");

    error = PrepareTask();
    if (error) {
        TLogger::LogError(error, "Can't prepare task");
        return error;
    }

    error = Task->Start();
    if (error) {
        LeafCgroups.clear();
        TLogger::LogError(error, "Can't start task");
        return error;
    }

    TLogger::Log() << Name << " started " << to_string(Task->GetPid()) << endl;

    Spec.SetInternal("root_pid", to_string(Task->GetPid()));
    State = EContainerState::Running;

    return TError::Success();
}

TError TContainer::KillAll() {
    auto cg = GetLeafCgroup(freezerSubsystem);

    TLogger::Log() << "killall " << Name << endl;

    vector<pid_t> reap;
    TError error = cg->GetTasks(reap);
    if (error) {
        TLogger::LogError(error, "Can't read tasks list while stopping container");
        return error;
    }

    // try to stop all tasks gracefully
    cg->Kill(SIGTERM);

    int ret = SleepWhile(1000, [&]{ return cg->IsEmpty() == false; });
    if (ret) {
        error = TError(EError::Unknown, errno, "sleep()");
        TLogger::LogError(error, "Error while sleeping");
    }

    // then kill any task that didn't want to stop via SIGTERM;
    // freeze all container tasks to make sure no one forks and races with us
    error = freezerSubsystem->Freeze(*cg);
    if (error)
        TLogger::LogError(error, "Can't kill all tasks");

    error = cg->GetTasks(reap);
    if (error) {
        TLogger::LogError(error, "Can't read tasks list while stopping container");
        return error;
    }
    cg->Kill(SIGKILL);
    error = freezerSubsystem->Unfreeze(*cg);
    if (error)
        TLogger::LogError(error, "Can't kill all tasks");

    return TError::Success();
}

// TODO: rework this into some kind of notify interface
extern void AckExitStatus(int pid);

TError TContainer::Stop() {
    if (IsRoot() || !(CheckState(EContainerState::Running) || CheckState(EContainerState::Dead)))
        return TError(EError::InvalidValue, "invalid container state " + ContainerStateName(State));

    TLogger::Log() << "stop " << Name << endl;

    int pid = Task->GetPid();

    TError error = KillAll();
    if (error)
        TLogger::LogError(error, "Can't kill all tasks in container");

    LeafCgroups.clear();

    AckExitStatus(pid);

    State = EContainerState::Stopped;

    return TError::Success();
}

TError TContainer::Pause() {
    if (IsRoot() || !CheckState(EContainerState::Running))
        return TError(EError::InvalidValue, "invalid container state " + ContainerStateName(State));

    auto cg = GetLeafCgroup(freezerSubsystem);
    TError error(freezerSubsystem->Freeze(*cg));
    if (error) {
        TLogger::LogError(error, "Can't pause " + Name);
        return error;
    }

    State = EContainerState::Paused;
    return TError::Success();
}

TError TContainer::Resume() {
    if (!CheckState(EContainerState::Paused))
        return TError(EError::InvalidValue, "invalid container state " + ContainerStateName(State));

    auto cg = GetLeafCgroup(freezerSubsystem);
    TError error(freezerSubsystem->Unfreeze(*cg));
    if (error) {
        TLogger::LogError(error, "Can't resume " + Name);
        return error;
    }


    State = EContainerState::Running;
    return TError::Success();
}

TError TContainer::GetData(const string &name, string &value) {
    if (dataSpec.find(name) == dataSpec.end())
        return TError(EError::InvalidValue, "invalid container data");

    if (IsRoot() && !dataSpec[name].RootValid)
        return TError(EError::InvalidData, "invalid data for root container");

    if (dataSpec[name].Valid.find(State) == dataSpec[name].Valid.end())
        return TError(EError::InvalidState, "invalid container state " + ContainerStateName(State));

    value = dataSpec[name].Handler(*this);
    return TError::Success();
}

TError TContainer::GetProperty(const string &property, string &value) const {
    if (IsRoot())
        return TError(EError::InvalidProperty, "no properties for root container");

    value = Spec.Get(property);
    return TError::Success();
}

TError TContainer::SetProperty(const string &property, const string &value) {
    if (IsRoot())
        return TError(EError::InvalidValue, "Can't set property for root");

    if (State != EContainerState::Stopped && !Spec.IsDynamic(property))
        return TError(EError::InvalidValue, "Can't set dynamic property " + property + " for running container");

    return Spec.Set(property, value);
}

TError TContainer::Restore(const kv::TNode &node) {
    TError error = Spec.Restore(node);
    if (error) {
        TLogger::LogError(error, "Can't restore task's spec");
        return error;
    }

    int pid = 0;
    bool started = true;
    string pidStr;
    error = Spec.GetInternal("root_pid", pidStr);
    if (error) {
        started = false;
    } else {
        error = StringToInt(pidStr, pid);
        if (error)
            started = false;
    }

    TLogger::Log() << Name << ": restore process " << to_string(pid) << " which " << (started ? "started" : "didn't start") << endl;

    State = EContainerState::Stopped;

    if (started) {
        error = PrepareCgroups();
        if (error) {
            TLogger::LogError(error, "Can't restore task cgroups");
            return error;
        }

        error = PrepareTask();
        if (error) {
            TLogger::LogError(error, "Can't prepare task");
            return error;
        }

        error = Task->Restore(pid);
        if (error) {
            Task = nullptr;
            (void)KillAll();

            TLogger::LogError(error, "Can't restore task");
            return error;
        }

        State = Task->IsRunning() ? EContainerState::Running : EContainerState::Stopped;
        if (State == EContainerState::Running)
            MaybeReturnedOk = true;
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
    if (LeafCgroups.find(subsys) != LeafCgroups.end())
        return LeafCgroups[subsys];

    if (Name == ROOT_CONTAINER)
        return subsys->GetRootCgroup()->GetChild(PORTO_ROOT_CGROUP);
    else
        return subsys->GetRootCgroup()->GetChild(PORTO_ROOT_CGROUP)->GetChild(Name);
}

bool TContainer::DeliverExitStatus(int pid, int status) {
    if (State != EContainerState::Running || !Task)
        return false;

    if (Task->GetPid() != pid)
        return false;

    Task->DeliverExitStatus(status);
    TLogger::Log() << "Delivered " << to_string(status) << " to " << Name << " with root_pid " << to_string(Task->GetPid()) << endl;
    State = EContainerState::Dead;
    TimeOfDeath = GetCurrentTime();
    return true;
}

void TContainer::Heartbeat() {
    if (Task)
        Task->Rotate();
}

bool TContainer::CanRemoveDead() const {
    return State == EContainerState::Dead && TimeOfDeath + CONTAINER_AGING_TIME <= GetCurrentTime();
}

// TContainerHolder

TError TContainerHolder::CreateInit() {
    TError error = Create(INIT_CONTAINER);
    if (error)
        return error;

    auto system = Get(INIT_CONTAINER);
    error = system->SetProperty("command", "/sbin/init");
    if (error)
        return error;

    error = system->SetProperty("cwd", "/");
    if (error)
        return error;

    string term;
    if (getenv("TERM"))
        term = string(";TERM=") + getenv("TERM");

    // we need to behave like lxc container, otherwise upstart won't run getty
    error = system->SetProperty("env", "container=lxc" + term);
    if (error)
        return error;

    error = system->SetProperty("user", "root");
    if (error)
        return error;

    error = system->SetProperty("group", "root");
    if (error)
        return error;

    error = system->SetProperty("subreaper", "true");
    if (error)
        return error;

    error = system->Start();
    if (error)
        return error;

    TLogger::Log() << "Waiting for sshd to start, then we can mount filesystems" << endl;
    int ret = RetryFailed(5 * 60, 1000, [] { return std::system("pgrep sshd &>/dev/null"); });
    if (ret)
        TLogger::Log() << "Waited for sshd bringup but it didn't start" << endl;

    TLogger::Log() << "Mount all filesystems in porto namespace" << endl;

    TMountSnapshot sysMntSnapshot("/etc/fstab");

    set<shared_ptr<TMount>> sysMnt;
    error = sysMntSnapshot.Mounts(sysMnt);
    if (error) {
        TLogger::LogError(error, "Error while mounting from /etc/fstab");
    } else {
        for (auto m : sysMnt) {
            if (m->VFSType() == "proc" || m->VFSType() == "swap")
                continue;

            error = m->Remount();
            TLogger::LogError(error, "Error remounting " + m->GetMountpoint());
        }
    }

    return TError::Success();
}

TError TContainerHolder::CreateRoot() {
    TError error = Create(ROOT_CONTAINER);
    if (error)
        return error;

    auto root = Get(ROOT_CONTAINER);
    error = root->Start();
    if (error)
        return error;

    return TError::Success();
}

bool TContainerHolder::ValidName(const string &name) const {
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

    if (Containers[name] == nullptr) {
        auto c = make_shared<TContainer>(name);
        TError error(c->Create());
        if (error)
            return error;

        Containers[name] = c;
        return TError::Success();
    } else
        return TError(EError::InvalidValue, "container " + name + " already exists");
}

shared_ptr<TContainer> TContainerHolder::Get(const string &name) {
    if (Containers.find(name) == Containers.end())
        return shared_ptr<TContainer>();

    return Containers[name];
}

void TContainerHolder::Destroy(const string &name) {
    if (name != ROOT_CONTAINER)
        Containers.erase(name);
}

vector<string> TContainerHolder::List() const {
    vector<string> ret;

    for (auto c : Containers)
        ret.push_back(c.second->GetName());

    return ret;
}

TError TContainerHolder::Restore(const std::string &name, const kv::TNode &node) {
    if (name == ROOT_CONTAINER || name == INIT_CONTAINER)
        return TError::Success();

    // TODO: we DO trust data from the persistent storage, do we?
    auto c = make_shared<TContainer>(name);
    auto e = c->Restore(node);
    if (e)
        return e;

    Containers[name] = c;
    return TError::Success();
}

bool TContainerHolder::DeliverExitStatus(int pid, int status) {
    for (auto c : Containers)
        if (c.second->DeliverExitStatus(pid, status))
            return true;

    return false;
}

void TContainerHolder::Heartbeat() {
    auto i = Containers.begin();

    while (i != Containers.end()) {
        auto &name = i->first;
        auto c = i->second;
        if (c->CanRemoveDead()) {
            TLogger::Log() << "Remove old dead container " << name << endl;
            i = Containers.erase(i);
        } else {
            c->Heartbeat();
            ++i;
        }
    }
}
