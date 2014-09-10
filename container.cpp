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
#include <sys/reboot.h>
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

    static string Parent(TContainer& c) {
        return c.Parent->Name;
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
    { "parent", { "container parent", false, TData::Parent, { EContainerState::Stopped, EContainerState::Dead, EContainerState::Running, EContainerState::Paused } } },
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

const string TContainer::StripParentName(const string &name) const {
    if (name == ROOT_CONTAINER)
        return ROOT_CONTAINER;

    std::string::size_type n = name.rfind('/');
    if (n == std::string::npos)
        return name;
    else
        return string(name.begin() + n + 1, name.end());
}

TContainer::~TContainer() {
    if (State == EContainerState::Paused)
        Resume();

    Stop();

    if (Parent)
        for (auto iter = Children.begin(); iter != Children.end();) {
            if (auto child = iter->lock()) {
                if (child->GetName() == GetName()) {
                    iter = Children.erase(iter);
                    continue;
                }
            } else {
                iter = Children.erase(iter);
                continue;
            }
            iter++;
        }
}

const string TContainer::GetName() const {
    if (!Parent)
        return Name;

    if (Parent->Name == ROOT_CONTAINER)
        return Name;
    else
        return Parent->GetName() + "/" + Name;
}

bool TContainer::IsRoot() const {
    return Name == ROOT_CONTAINER;
}

std::shared_ptr<const TContainer> TContainer::GetRoot() const {
    if (Parent)
        return Parent->GetRoot();
    else
        return shared_from_this();
}

std::shared_ptr<const TContainer> TContainer::GetParent() const {
    return Parent;
}

uint64_t TContainer::GetPropertyUint64(const std::string &property) const {
    uint64_t val;

    if (StringToUint64(Spec.Get(property), val))
        return 0;

    return val;
}

uint64_t TContainer::GetChildrenSum(const std::string &property, std::shared_ptr<const TContainer> except, uint64_t exceptVal) const {
    uint64_t val = 0;

    for (auto iter : Children)
        if (auto child = iter.lock()) {
            if (except && except == child) {
                val += exceptVal;
                continue;
            }

            uint64_t childval = child->GetPropertyUint64(property);
            if (childval)
                val += childval;
            else
                val += child->GetChildrenSum(property, except, exceptVal);
        }

    return val;
}

bool TContainer::ValidHierarchicalProperty(const std::string &property, const std::string &value) const {
    uint64_t newval;

    if (StringToUint64(value, newval))
        return false;

    uint64_t children = GetChildrenSum(property);
    if (children && newval < children)
        return false;

    for (auto c = GetParent(); c; c = c->GetParent()) {
        uint64_t parent = c->GetPropertyUint64(property);
        if (parent && newval > parent)
            return false;
    }

    if (GetParent()) {
        uint64_t parent = GetParent()->GetPropertyUint64(property);
        uint64_t children = GetParent()->GetChildrenSum(property, shared_from_this(), newval);
        if (parent && children > parent)
            return false;
    }

    return true;
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

TError TContainer::ApplyDynamicProperties() {
    auto memcg = GetLeafCgroup(memorySubsystem);

    TError error = memorySubsystem->UseHierarchy(*memcg);
    TLogger::LogError(error, "Can't set use_hierarchy for " + memcg->Relpath());
    if (error)
        return error;

    auto memroot = memorySubsystem->GetRootCgroup();
    if (memroot->HasKnob("memory.low_limit_in_bytes") && Spec.GetAsInt("memory_guarantee") != 0) {
        TError error = memcg->SetKnobValue("memory.low_limit_in_bytes", Spec.Get("memory_guarantee"), false);
        TLogger::LogError(error, "Can't set memory_guarantee");
        if (error)
            return error;
    }

    if (Spec.GetAsInt("memory_limit") != 0) {
        error = memcg->SetKnobValue("memory.limit_in_bytes", Spec.Get("memory_limit"), false);
        TLogger::LogError(error, "Can't set memory_limit");
        if (error)
            return error;
    }

    auto cpucg = GetLeafCgroup(cpuSubsystem);
    if (Spec.Get("cpu_policy") == "normal") {
        int cpuPrio;
        error = StringToInt(Spec.Get("cpu_priority"), cpuPrio);
        TLogger::LogError(error, "Can't parse cpu_priority");
        if (error)
            return error;

        error = cpucg->SetKnobValue("cpu.shares", to_string(cpuPrio + 2), false);
        TLogger::LogError(error, "Can't set cpu_priority");
        if (error)
            return error;
    }

    return TError::Success();
}

TError TContainer::PrepareCgroups() {
    LeafCgroups[cpuSubsystem] = GetLeafCgroup(cpuSubsystem);
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

    auto cpucg = GetLeafCgroup(cpuSubsystem);
    auto cpuroot = cpuSubsystem->GetRootCgroup();
    if (cpuroot->HasKnob("cpu.smart")) {
        TError error;
        if (Spec.Get("cpu_policy") == "rt")
            error = cpucg->SetKnobValue("cpu.smart", "1", false);
        else
            error = cpucg->SetKnobValue("cpu.smart", "0", false);

        TLogger::LogError(error, "Can't enable smart");
        if (error)
            return error;
    }

    return ApplyDynamicProperties();
}

TError TContainer::PrepareTask() {
    TTaskEnv taskEnv(Spec.Get("command"), Spec.Get("cwd"), Spec.Get("root"), Spec.Get("user"), Spec.Get("group"), Spec.Get("env"));
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
    TLogger::Log() << "Create " << GetName() << endl;
    TError error = Spec.Create();
    if (error)
        return error;

    if (Parent)
        Parent->Children.push_back(std::weak_ptr<TContainer>(shared_from_this()));

    return TError::Success();
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
        return TError(EError::InvalidState, "invalid container state " + ContainerStateName(State));

    if (Parent && !Parent->IsRoot() && Parent->State != EContainerState::Running)
        return TError(EError::InvalidState, "parent is not running");

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

    TLogger::Log() << GetName() << " started " << to_string(Task->GetPid()) << endl;

    Spec.SetInternal("root_pid", to_string(Task->GetPid()));
    State = EContainerState::Running;

    return TError::Success();
}

TError TContainer::KillAll() {
    auto cg = GetLeafCgroup(freezerSubsystem);

    TLogger::Log() << "killall " << GetName() << endl;

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
        return TError(EError::InvalidState, "invalid container state " + ContainerStateName(State));

    TLogger::Log() << "stop " << GetName() << endl;

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
        return TError(EError::InvalidState, "invalid container state " + ContainerStateName(State));

    auto cg = GetLeafCgroup(freezerSubsystem);
    TError error(freezerSubsystem->Freeze(*cg));
    if (error) {
        TLogger::LogError(error, "Can't pause " + GetName());
        return error;
    }

    State = EContainerState::Paused;
    return TError::Success();
}

TError TContainer::Resume() {
    if (!CheckState(EContainerState::Paused))
        return TError(EError::InvalidState, "invalid container state " + ContainerStateName(State));

    auto cg = GetLeafCgroup(freezerSubsystem);
    TError error(freezerSubsystem->Unfreeze(*cg));
    if (error) {
        TLogger::LogError(error, "Can't resume " + GetName());
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

bool TContainer::ShouldApplyProperty(const std::string &property) {
    if (!Spec.IsDynamic(property))
       return false;

    if (State == EContainerState::Dead || State == EContainerState::Stopped)
        return false;

    return true;
}

TError TContainer::SetProperty(const string &property, const string &value) {
    if (IsRoot())
        return TError(EError::InvalidValue, "Can't set property for root");

    if (State != EContainerState::Stopped && !Spec.IsDynamic(property))
        return TError(EError::InvalidValue, "Can't set dynamic property " + property + " for running container");

    TError error = Spec.Set(shared_from_this(), property, value);
    if (error)
        return error;

    if (ShouldApplyProperty(property))
        error = ApplyDynamicProperties();

    return error;
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

    TLogger::Log() << GetName() << ": restore process " << to_string(pid) << " which " << (started ? "started" : "didn't start") << endl;

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

    return Parent->GetLeafCgroup(subsys)->GetChild(Name);
}

bool TContainer::DeliverExitStatus(int pid, int status) {
    if (State != EContainerState::Running || !Task)
        return false;

    if (Task->GetPid() != pid)
        return false;

    Task->DeliverExitStatus(status);
    TLogger::Log() << "Delivered " << to_string(status) << " to " << GetName() << " with root_pid " << to_string(Task->GetPid()) << endl;
    State = EContainerState::Dead;

    if (NeedRespawn()) {
        TError error = Respawn();
        TLogger::LogError(error, "Can't respawn " + GetName());
    }

    TimeOfDeath = GetCurrentTimeMs();
    return true;
}

bool TContainer::NeedRespawn() {
    if (State != EContainerState::Dead)
        return false;

    return Spec.Get("respawn") == "true" && TimeOfDeath + RESPAWN_DELAY_MS <= GetCurrentTimeMs();
}

TError TContainer::Respawn() {
    TError error = Stop();
    if (error)
        return error;

    return Start();
}

void TContainer::Heartbeat() {
    if (NeedRespawn()) {
        TError error = Respawn();
        TLogger::LogError(error, "Can't respawn " + GetName());
    }

    if (State != EContainerState::Running || !Task)
        return;

    Task->Rotate();
}

bool TContainer::CanRemoveDead() const {
    return State == EContainerState::Dead && TimeOfDeath + CONTAINER_AGING_TIME_MS <= GetCurrentTimeMs();
}

bool TContainer::HasChildren() const {
    // link #1 - this
    // link #2 - TContainerHolder->Containers
    // any other link comes from TContainer->Parent and indicates that
    // current container has children
    return shared_from_this().use_count() > 2;
}

// TContainerHolder

TContainerHolder::~TContainerHolder() {
    // we want children to be removed first
    for (auto i = Containers.rbegin(); i != Containers.rend(); ++i)
        Containers.erase(i->first);
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

    for (string::size_type i = 0; i + 1 < name.length(); i++)
        if (name[i] == '/' && name[i + 1] == '/')
            return false;

    if (*name.begin() == '/')
        return false;

    if (*(name.end()--) == '/')
        return false;

    // . (dot) is used for kvstorage, so don't allow it here
    return find_if(name.begin(), name.end(),
                   [](const char c) -> bool {
                        return !(isalnum(c) || c == '_' || c == '/');
                   }) == name.end();
}

std::shared_ptr<TContainer> TContainerHolder::GetParent(const std::string &name) const {
    std::shared_ptr<TContainer> parent;

    string::size_type n = name.rfind('/');
    if (n == string::npos) {
        return Containers.at(ROOT_CONTAINER);
    } else {
        string parentName = name.substr(0, n);

        if (Containers.find(parentName) == Containers.end())
            return nullptr;

        return Containers.at(parentName);
    }
}

TError TContainerHolder::Create(const string &name) {
    if (!ValidName(name))
        return TError(EError::InvalidValue, "invalid container name " + name);

    if (Containers.find(name) != Containers.end())
        return TError(EError::ContainerAlreadyExists, "container " + name + " already exists");

    auto parent = GetParent(name);
    if (!parent && name != ROOT_CONTAINER)
        return TError(EError::InvalidValue, "invalid parent container");

    auto c = make_shared<TContainer>(name, parent);
    TError error(c->Create());
    if (error)
        return error;

    Containers[name] = c;
    return TError::Success();
}

shared_ptr<TContainer> TContainerHolder::Get(const string &name) {
    if (Containers.find(name) == Containers.end())
        return shared_ptr<TContainer>();

    return Containers[name];
}

TError TContainerHolder::Destroy(const string &name) {
    if (name == ROOT_CONTAINER || Containers.find(name) == Containers.end())
        return TError(EError::InvalidValue, "invalid container name " + name);

    if (Containers[name]->HasChildren())
        return TError(EError::InvalidState, "container has children");

    Containers.erase(name);

    return TError::Success();
}

vector<string> TContainerHolder::List() const {
    vector<string> ret;

    for (auto c : Containers)
        ret.push_back(c.second->GetName());

    return ret;
}

TError TContainerHolder::Restore(const std::string &name, const kv::TNode &node) {
    if (name == ROOT_CONTAINER)
        return TError::Success();

    // TODO: we DO trust data from the persistent storage, do we?
    auto parent = GetParent(name);
    if (!parent)
        return TError(EError::InvalidValue, "invalid parent container");

    auto c = make_shared<TContainer>(name, parent);
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
