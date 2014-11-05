#include <algorithm>
#include <sstream>
#include <memory>
#include <csignal>
#include <cstdlib>

#include "container.hpp"
#include "task.hpp"
#include "cgroup.hpp"
#include "subsystem.hpp"
#include "property.hpp"
#include "data.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/netlink.hpp"
#include "util/pwd.hpp"

extern "C" {
#include <sys/types.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/reboot.h>
#include <fcntl.h>
#include <sys/eventfd.h>
}

using std::string;
using std::vector;
using std::shared_ptr;
using std::unique_ptr;
using std::map;

// Data

static int64_t GetBootTime() {
    vector<string> lines;
    TFile f("/proc/stat");
    if (f.AsLines(lines))
        return 0;

    for (auto &line : lines) {
        vector<string> cols;
        if (SplitString(line, ' ', cols))
            return 0;

        if (cols[0] == "btime") {
            int64_t val;
            if (StringToInt64(cols[1], val))
                return 0;
            return val;
        }
    }

    return 0;
}

int64_t BootTime = 0;

std::string ContainerStateName(EContainerState state) {
    switch (state) {
    case EContainerState::Stopped:
        return "stopped";
    case EContainerState::Dead:
        return "dead";
    case EContainerState::Running:
        return "running";
    case EContainerState::Paused:
        return "paused";
    case EContainerState::Meta:
        return "meta";
    default:
        return "unknown";
    }
}

// TContainerEvent

std::string TContainerEvent::GetMsg() const {
    switch (Type) {
        case EContainerEventType::Exit:
            return "exit status " + std::to_string(Exit.Status)
                + " for pid " + std::to_string(Exit.Pid);
        case EContainerEventType::OOM:
            return "oom for fd " + std::to_string(Oom.Fd);
        default:
            return "unknown event";
    }
}

// TContainer

bool TContainer::HaveRunningChildren() {
    for (auto iter : Children)
        if (auto child = iter.lock()) {
            if (child->State == EContainerState::Running ||
                child->State == EContainerState::Dead) {
                return true;
            } else if (child->State == EContainerState::Meta) {
                if (child->HaveRunningChildren())
                    return true;
            }
        }

    return false;
}

EContainerState TContainer::GetState() {
    static bool rec = false;

    if (rec)
        return State;

    rec = true;

    if (State == EContainerState::Running && (!Task || !Task->IsRunning()))
        SetState(EContainerState::Stopped);

    // TODO: use some kind of reference count for accounting running children
    if (State == EContainerState::Meta && !IsRoot()) {
        if (!HaveRunningChildren())
            Stop();
    }

    rec = false;

    return State;
}

void TContainer::SetState(EContainerState newState) {
    if (State == newState)
        return;

    TLogger::Log() << GetName() << ": change state " << ContainerStateName(State) << " -> " << ContainerStateName(newState) << std::endl;
    State = newState;
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
    if (GetState() == EContainerState::Paused)
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

    /*
    if (Filter) {
        TError error = Filter->Remove();
        TLogger::LogError(error, "Can't remove tc filter");
    }
    */

    if (DefaultTclass) {
        TError error = DefaultTclass->Remove();
        TLogger::LogError(error, "Can't remove default tc class");
    }

    if (Qdisc) {
        TError error = Qdisc->Remove();
        TLogger::LogError(error, "Can't remove tc qdisc");
    }
}

const string TContainer::GetName(bool recursive) const {
    if (!recursive)
        return Name;

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

uint64_t TContainer::GetChildrenSum(const std::string &property, std::shared_ptr<const TContainer> except, uint64_t exceptVal) const {
    uint64_t val = 0;

    for (auto iter : Children)
        if (auto child = iter.lock()) {
            if (except && except == child) {
                val += exceptVal;
                continue;
            }

            uint64_t childval = child->Prop->GetUint(property);
            if (childval)
                val += childval;
            else
                val += child->GetChildrenSum(property, except, exceptVal);
        }

    return val;
}

bool TContainer::ValidHierarchicalProperty(const std::string &property, const uint64_t value) const {
    uint64_t children = GetChildrenSum(property);
    if (children && value < children)
        return false;

    for (auto c = GetParent(); c; c = c->GetParent()) {
        uint64_t parent = c->Prop->GetUint(property);
        if (parent && value > parent)
            return false;
    }

    if (GetParent()) {
        uint64_t parent = GetParent()->Prop->GetUint(property);
        uint64_t children = GetParent()->GetChildrenSum(property, shared_from_this(), value);
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

TError TContainer::ApplyDynamicProperties() {
    auto memcg = GetLeafCgroup(memorySubsystem);

    TError error = memorySubsystem->UseHierarchy(*memcg);
    TLogger::LogError(error, "Can't set use_hierarchy for " + memcg->Relpath());
    if (error)
        return error;

    auto memroot = memorySubsystem->GetRootCgroup();
    if (memroot->HasKnob("memory.low_limit_in_bytes") && Prop->GetUint("memory_guarantee") != 0) {
        TError error = memcg->SetKnobValue("memory.low_limit_in_bytes", Prop->GetString("memory_guarantee"), false);
        TLogger::LogError(error, "Can't set memory_guarantee");
        if (error)
            return error;
    }

    if (Prop->GetUint("memory_limit") != 0) {
        error = memcg->SetKnobValue("memory.limit_in_bytes", Prop->GetString("memory_limit"), false);
        TLogger::LogError(error, "Can't set memory_limit");
        if (error)
            return error;
    }

    if (memroot->HasKnob("memory.recharge_on_pgfault")) {
        string value = Prop->GetBool("recharge_on_pgfault") ? "1" : "0";
        error = memcg->SetKnobValue("memory.recharge_on_pgfault", value, false);
        TLogger::LogError(error, "Can't set recharge_on_pgfault");
        if (error)
            return error;
    }

    auto cpucg = GetLeafCgroup(cpuSubsystem);
    if (Prop->GetString("cpu_policy") == "normal") {
        string smart;

        error = cpucg->GetKnobValue("cpu.smart", smart);
        if (!error && smart == "1") {
            error = cpucg->SetKnobValue("cpu.smart", "0", false);
            TLogger::LogError(error, "Can't disable smart");
            if (error)
                return error;
        }

        int cpuPrio = Prop->GetUint("cpu_priority");
        error = cpucg->SetKnobValue("cpu.shares", std::to_string(cpuPrio + 2), false);
        TLogger::LogError(error, "Can't set cpu_priority");
        if (error)
            return error;

    } else if (Prop->GetString("cpu_policy") == "rt") {
        string smart;

        error = cpucg->GetKnobValue("cpu.smart", smart);
        if (!error && smart == "0") {
            error = cpucg->SetKnobValue("cpu.smart", "1", false);
            TLogger::LogError(error, "Can't enable smart");
            if (error)
                return error;
        }
    }

    return TError::Success();
}

std::shared_ptr<TContainer> TContainer::FindRunningParent() const {
    auto p = Parent;
    while (p) {
        if (p->State == EContainerState::Running)
            return p;
        p = p->Parent;
    }

    return nullptr;
}

bool TContainer::UseParentNamespace() const {
    bool isolate = Prop->GetRawBool("isolate");
    if (isolate)
        return false;

    return FindRunningParent() != nullptr;
}

TError TContainer::PrepareNetwork() {
    if (!config().network().enabled())
        return TError::Success();

    PORTO_ASSERT(Tclass == nullptr);

    if (UseParentNamespace())
        return TError::Success();

    if (Parent) {
        PORTO_ASSERT(Parent->Tclass != nullptr);

        auto tclass = Parent->Tclass;
        uint32_t handle = TcHandle(TcMajor(tclass->GetHandle()), Id);
        Tclass = std::make_shared<TTclass>(tclass, handle);
    } else {
        uint32_t handle = TcHandle(TcMajor(Qdisc->GetHandle()), Id);
        Tclass = std::make_shared<TTclass>(Qdisc, handle);
    }

    TError error;
    uint32_t prio, rate, ceil;

    prio = Prop->GetUint("net_priority");
    rate = Prop->GetUint("net_guarantee");
    ceil = Prop->GetUint("net_ceil");

    if (Tclass->Exists())
        (void)Tclass->Remove();
    error = Tclass->Create(prio, rate, ceil);
    if (error) {
        TLogger::LogError(error, "Can't create tclass");
        return error;
    }

    return TError::Success();
}

TError TContainer::PrepareOomMonitor() {
    if (UseParentNamespace())
        return TError::Success();

    auto memcg = GetLeafCgroup(memorySubsystem);

    Efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (Efd.GetFd() < 0) {
        TError error(EError::Unknown, errno, "Can't create eventfd");
        TLogger::LogError(error, "Can't update OOM settings");
        return error;
    }

    string cfdPath = memcg->Path() + "/memory.oom_control";
    TScopedFd cfd(open(cfdPath.c_str(), O_RDONLY | O_CLOEXEC));
    if (cfd.GetFd() < 0) {
        TError error(EError::Unknown, errno, "Can't open " + memcg->Path());
        TLogger::LogError(error, "Can't update OOM settings");
        return error;
    }

    TFile f(memcg->Path() + "/cgroup.event_control");
    string s = std::to_string(Efd.GetFd()) + " " + std::to_string(cfd.GetFd());
    return f.WriteStringNoAppend(s);
}

TError TContainer::PrepareCgroups() {
    LeafCgroups[cpuSubsystem] = GetLeafCgroup(cpuSubsystem);
    LeafCgroups[cpuacctSubsystem] = GetLeafCgroup(cpuacctSubsystem);
    LeafCgroups[memorySubsystem] = GetLeafCgroup(memorySubsystem);
    LeafCgroups[freezerSubsystem] = GetLeafCgroup(freezerSubsystem);
    LeafCgroups[blkioSubsystem] = GetLeafCgroup(blkioSubsystem);
    if (config().network().enabled())
        LeafCgroups[netclsSubsystem] = GetLeafCgroup(netclsSubsystem);
    LeafCgroups[devicesSubsystem] = GetLeafCgroup(devicesSubsystem);

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
        if (Prop->GetString("cpu_policy") == "rt") {
            error = cpucg->SetKnobValue("cpu.smart", "1", false);
            TLogger::LogError(error, "Can't enable smart");
            if (error)
                return error;
        }
    }

    if (config().network().enabled() && !UseParentNamespace()) {
        auto netcls = GetLeafCgroup(netclsSubsystem);
        uint32_t handle = Tclass->GetHandle();
        TError error = netcls->SetKnobValue("net_cls.classid", std::to_string(handle), false);
        TLogger::LogError(error, "Can't set classid");
        if (error)
            return error;
    }

    TError error = ApplyDynamicProperties();
    if (error)
        return error;

    if (!IsRoot()) {
        error = PrepareOomMonitor();
        TLogger::LogError(error, "Can't prepare OOM monitoring");
        if (error)
            return error;
    }

    auto devices = GetLeafCgroup(devicesSubsystem);
    error = devicesSubsystem->AllowDevices(devices,
                                           Prop->GetString("allowed_devices"));
    if (error) {
        TLogger::LogError(error, "Can't set allowed_devices");
        return error;
    }

    return TError::Success();
}

TError TContainer::PrepareTask() {
    auto taskEnv = std::make_shared<TTaskEnv>();

    taskEnv->Command = Prop->GetString("command");
    taskEnv->Cwd = Prop->GetString("cwd");
    taskEnv->Root = Prop->GetString("root");
    taskEnv->CreateCwd = Prop->IsDefault("root") && Prop->IsDefault("cwd") && !UseParentNamespace();
    taskEnv->User = Prop->GetString("user");
    taskEnv->Group = Prop->GetString("group");
    taskEnv->Environ = Prop->GetString("env") + ";container=lxc;PORTO_NAME=" + GetName();
    taskEnv->Isolate = Prop->GetBool("isolate");
    taskEnv->StdinPath = Prop->GetString("stdin_path");
    taskEnv->StdoutPath = Prop->GetString("stdout_path");
    taskEnv->StderrPath = Prop->GetString("stderr_path");
    taskEnv->Hostname = Prop->GetString("hostname");
    taskEnv->BindDns = Prop->GetBool("bind_dns");

    TError error = ParseRlimit(Prop->GetString("ulimit"), taskEnv->Rlimit);
    if (error)
        return error;

    error = ParseBind(Prop->GetString("bind"), taskEnv->BindMap);
    if (error)
        return error;

    if (config().network().enabled()) {
        error = ParseNet(shared_from_this(), Prop->GetString("net"), taskEnv->NetCfg);
        if (error)
            return error;
    }

    if (UseParentNamespace()) {
        auto p = FindRunningParent();
        if (!p)
            return TError(EError::Unknown, "Couldn't find running parent");

        TError error = taskEnv->Ns.Create(p->Task->GetPid());
        if (error)
            return error;
    }

    error = taskEnv->Prepare();
    if (error)
        return error;

    vector<shared_ptr<TCgroup>> cgroups;
    for (auto cg : LeafCgroups)
        cgroups.push_back(cg.second);
    Task = unique_ptr<TTask>(new TTask(taskEnv, cgroups));
    return TError::Success();
}

TError TContainer::Create(int uid, int gid) {
    TLogger::Log() << "Create " << GetName() << " " << Id << std::endl;

    Uid = uid;
    Gid = gid;

    Prop = std::make_shared<TPropertySet>(shared_from_this());
    Data = std::make_shared<TVariantSet>(&dataSet, shared_from_this());

    TError error = Prop->Create();
    if (error)
        return error;

    if (Uid >= 0) {
        TUser u(Uid);
        error = u.Load();
        if (error)
            return error;
    }

    if (Gid >= 0) {
        TGroup g(Gid);
        error = g.Load();
        if (error)
            return error;
    }

    error = Data->SetInt(D_START_ERRNO, -1);
    if (error)
        return error;

    error = Prop->SetInt(P_RAW_UID, Uid);
    if (error)
        return error;
    error = Prop->SetInt(P_RAW_GID, Gid);
    if (error)
        return error;

    if (Parent)
        Parent->Children.push_back(std::weak_ptr<TContainer>(shared_from_this()));

    if (IsRoot()) {
        // 1:0 qdisc
        // 1:2 default class    1:1 root class
        // (unclassified        1:3 container a, 1:4 container b
        //          traffic)    1:5 container a/c

        PORTO_ASSERT(Id == 1);
        uint32_t defHandle = TcHandle(Id, Id + 1);
        uint32_t rootHandle = TcHandle(Id, 0);

        Qdisc = std::make_shared<TQdisc>(Link, rootHandle, defHandle);
        (void)Qdisc->Remove();
        error = Qdisc->Create();
        if (error) {
            TLogger::LogError(error, "Can't create root qdisc");
            return error;
        }

        Filter = std::make_shared<TFilter>(Qdisc);
        //(void)Filter->Remove();
        error = Filter->Create();
        if (error) {
            TLogger::LogError(error, "Can't create tc filter");
            return error;
        }

        DefaultTclass = std::make_shared<TTclass>(Qdisc, defHandle);
        if (DefaultTclass->Exists())
            (void)DefaultTclass->Remove();
        error = DefaultTclass->Create(DEF_CLASS_PRIO, DEF_CLASS_RATE, DEF_CLASS_CEIL);
        if (error) {
            TLogger::LogError(error, "Can't create default tclass");
            return error;
        }
    }

    return TError::Success();
}

TError TContainer::PrepareMetaParent() {
    if (Parent && Parent->GetState() != EContainerState::Meta) {
        TError error = Parent->PrepareMetaParent();
        if (error)
            return error;
    }

    auto state = GetState();
    if (state == EContainerState::Stopped) {
        SetState(EContainerState::Meta);

        TError error = PrepareNetwork();
        if (error) {
            FreeResources();
            return error;
        }

        error = PrepareCgroups();
        if (error) {
            FreeResources();
            return error;
        }
    } else if (state == EContainerState::Meta) {
        return TError::Success();
    } else if (state != EContainerState::Running) {
        return TError(EError::InvalidState, "invalid parent state");
    }

    return TError::Success();
}

TError TContainer::Start() {
    auto state = GetState();

    if ((state == EContainerState::Running ||
         state == EContainerState::Dead) && MaybeReturnedOk) {
        MaybeReturnedOk = false;
        return TError::Success();
    }
    MaybeReturnedOk = false;

    TError error = Data->SetUint(D_RESPAWN_COUNT, 0);
    if (error)
        return error;

    if (state != EContainerState::Stopped)
        return TError(EError::InvalidState, "invalid container state " +
                      ContainerStateName(state));

    if (!IsRoot() && !Prop->GetString("command").length()) {
        FreeResources();
        return TError(EError::InvalidValue, "container command is empty");
    }

    error = Prop->SetInt(P_RAW_ID, (int)Id);
    if (error)
        return error;

    error = Data->SetBool(D_OOM_KILLED, false);
    if (error)
        return error;

    error = PrepareResources();
    if (error)
        return error;

    if (IsRoot()) {
        SetState(EContainerState::Meta);
        return TError::Success();
    }

    error = PrepareTask();
    if (error) {
        TLogger::LogError(error, "Can't prepare task");
        FreeResources();
        return error;
    }

    error = Task->Start();
    if (error) {
        TLogger::LogError(error, "Can't start task");
        FreeResources();
        TError e = Data->SetInt(D_START_ERRNO, error.GetErrno());
        TLogger::LogError(e, "Can't set start_errno");
        return error;
    }

    error = Data->SetInt(D_START_ERRNO, -1);
    if (error)
        return error;

    TLogger::Log() << GetName() << " started " << std::to_string(Task->GetPid()) << std::endl;

    error = Prop->SetInt(P_RAW_ROOT_PID, Task->GetPid());
    if (error)
        return error;
    SetState(EContainerState::Running);

    return TError::Success();
}

TError TContainer::KillAll() {
    auto cg = GetLeafCgroup(freezerSubsystem);

    TLogger::Log() << "killall " << GetName() << std::endl;

    vector<pid_t> reap;
    TError error = cg->GetTasks(reap);
    if (error) {
        TLogger::LogError(error, "Can't read tasks list while stopping container (SIGTERM)");
        return error;
    }

    // try to stop all tasks gracefully
    cg->Kill(SIGTERM);

    int ret = SleepWhile(1000, [&]{ return cg->IsEmpty() == false; });
    if (ret)
        TLogger::Log() << "Warning: child didn't exit via SIGTERM, sending SIGKILL" << std::endl;

    // then kill any task that didn't want to stop via SIGTERM signal;
    // freeze all container tasks to make sure no one forks and races with us
    error = freezerSubsystem->Freeze(*cg);
    if (error)
        TLogger::LogError(error, "Can't kill all tasks");

    error = cg->GetTasks(reap);
    if (error) {
        TLogger::LogError(error, "Can't read tasks list while stopping container (SIGKILL)");
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

void TContainer::StopChildren() {
    for (auto iter : Children) {
        if (auto child = iter.lock()) {
            if (child->GetState() != EContainerState::Stopped && child->GetState() != EContainerState::Dead)
                child->Stop();
        } else {
            TLogger::Log() << "Warning: can't lock child while stopping" << std::endl;
        }
    }
}

TError TContainer::PrepareResources() {
    if (Parent) {
        TError error = Parent->PrepareMetaParent();
        if (error) {
            TLogger::LogError(error, "Can't prepare parent");
            return error;
        }
    }

    TError error = PrepareNetwork();
    if (error) {
        TLogger::LogError(error, "Can't prepare task network");
        FreeResources();
        return error;
    }

    error = PrepareCgroups();
    if (error) {
        TLogger::LogError(error, "Can't prepare task cgroups");
        FreeResources();
        return error;
    }

    return TError::Success();
}

void TContainer::FreeResources() {
    LeafCgroups.clear();

    if (Tclass) {
        TError error = Tclass->Remove();
        Tclass = nullptr;
        TLogger::LogError(error, "Can't remove tc classifier");
    }

    Task = nullptr;
    Efd = -1;
}

TError TContainer::Stop() {
    auto state = GetState();

    if (state == EContainerState::Stopped ||
        state == EContainerState::Paused)
        return TError(EError::InvalidState, "invalid container state " + ContainerStateName(state));

    TLogger::Log() << "Stop " << GetName() << " " << Id << std::endl;

    if (state == EContainerState::Running || state == EContainerState::Dead) {

        int pid = Task->GetPid();

        TError error = KillAll();
        if (error)
            TLogger::LogError(error, "Can't kill all tasks in container");

        int ret = SleepWhile(1000, [&]{ kill(pid, 0); return errno != ESRCH; });
        if (ret)
            TLogger::Log() << "Error while waiting for container to stop" << std::endl;

        AckExitStatus(pid);
        Task->DeliverExitStatus(-1);
    }

    if (!IsRoot())
        SetState(EContainerState::Stopped);
    StopChildren();
    if (!IsRoot())
        FreeResources();

    return TError::Success();
}

TError TContainer::Pause() {
    auto state = GetState();
    if (state != EContainerState::Running)
        return TError(EError::InvalidState, "invalid container state " +
                      ContainerStateName(state));

    auto cg = GetLeafCgroup(freezerSubsystem);
    TError error(freezerSubsystem->Freeze(*cg));
    if (error) {
        TLogger::LogError(error, "Can't pause " + GetName());
        return error;
    }

    SetState(EContainerState::Paused);
    return TError::Success();
}

TError TContainer::Resume() {
    auto state = GetState();
    if (state != EContainerState::Paused)
        return TError(EError::InvalidState, "invalid container state " +
                      ContainerStateName(state));

    auto cg = GetLeafCgroup(freezerSubsystem);
    TError error(freezerSubsystem->Unfreeze(*cg));
    if (error) {
        TLogger::LogError(error, "Can't resume " + GetName());
        return error;
    }

    SetState(EContainerState::Running);
    return TError::Success();
}

TError TContainer::Kill(int sig) {
    auto state = GetState();
    if (state != EContainerState::Running)
        return TError(EError::InvalidState, "invalid container state " +
                      ContainerStateName(state));

    return Task->Kill(sig);
}

TError TContainer::GetData(const string &name, string &value) {
    // TODO: InvalidData
    if (!dataSet.Valid(name))
        return TError(EError::InvalidValue, "invalid container data");

    auto d = dataSet.Get(name);
    if (d->State.find(GetState()) == d->State.end())
        return TError(EError::InvalidState, "invalid container state");

    TValue *p = nullptr;
    std::shared_ptr<TContainer> c;
    std::shared_ptr<TVariant> v;
    TError error = Data->Get(name, c, &p, v);
    if (error)
        return error;

    value = p->GetString(c, v);
    return TError::Success();
}

void TContainer::PropertyToAlias(const string &property, string &value) const {
        if (property == "cpu.smart") {
            if (value == "rt")
                value = "1";
            else
                value = "0";
        } else if (property == "memory.recharge_on_pgfault") {
            value = value == "true" ? "1" : "0";
        }
}

TError TContainer::AliasToProperty(string &property, string &value) {
        if (property == "cpu.smart") {
            if (value == "0") {
                property = "cpu_policy";
                value = "normal";
            } else {
                property = "cpu_policy";
                value = "rt";
            }
        } else if (property == "memory.limit_in_bytes") {
            property = "memory_limit";
            uint64_t n;

            TError error = StringWithUnitToUint64(value, n);
            if (error)
                return error;

            value = std::to_string(n);
        } else if (property == "memory.low_limit_in_bytes") {
            property = "memory_guarantee";
            uint64_t n;

            TError error = StringWithUnitToUint64(value, n);
            if (error)
                return error;

            value = std::to_string(n);
        } else if (property == "memory.recharge_on_pgfault") {
            property = "recharge_on_pgfault";
            value = value == "0" ? "false" : "true";
        }

        return TError::Success();
}

static std::map<std::string, std::string> alias = {
    { "cpu.smart", "cpu_policy" },
    { "memory.limit_in_bytes", "memory_limit" },
    { "memory.low_limit_in_bytes", "memory_guarantee" },
    { "memory.recharge_on_pgfault", "recharge_on_pgfault" },
};

TError TContainer::GetProperty(const string &origProperty, string &value) const {
    if (IsRoot())
        return TError(EError::InvalidProperty, "no properties for root container");

    string property = origProperty;
    if (alias.find(origProperty) != alias.end())
        property = alias.at(origProperty);

    TError error = Prop->PropertyExists(property);
    if (error)
        return error;

    value = Prop->GetString(property);
    PropertyToAlias(origProperty, value);

    return TError::Success();
}

bool TContainer::ShouldApplyProperty(const std::string &property) {
    if (!Prop->HasState(property, EContainerState::Running))
       return false;

    auto state = GetState();
    if (state == EContainerState::Dead || state == EContainerState::Stopped)
        return false;

    return true;
}

TError TContainer::SetProperty(const string &origProperty, const string &origValue, bool superuser) {
    if (IsRoot())
        return TError(EError::InvalidValue, "Can't set property for root containers");

    string property = origProperty;
    string value = StringTrim(origValue);

    TError error = AliasToProperty(property, value);
    if (error)
        return error;

    error = Prop->PropertyExists(property);
    if (error)
        return error;

    if (Prop->HasFlags(property, SUPERUSER_PROPERTY) && !superuser)
        if (Prop->GetString(property) != value)
            return TError(EError::Permission, "Only root can change this property");

    if (!Prop->HasState(property, GetState()))
        return TError(EError::InvalidState, "Can't set dynamic property " + property + " for running container");

    if (UseParentNamespace() && Prop->HasFlags(property, PARENT_RO_PROPERTY))
        return TError(EError::NotSupported, "Can't set " + property + " for child container");

    error = Prop->SetString(property, value);
    if (error) {
        TLogger::LogError(error, "Can't set property");
        return error;
    }

    if (ShouldApplyProperty(property))
        error = ApplyDynamicProperties();

    return error;
}

TError TContainer::Restore(const kv::TNode &node) {
    TLogger::Log() << "Restore " << GetName() << " " << Id << std::endl;

    Prop = std::make_shared<TPropertySet>(shared_from_this());
    Data = std::make_shared<TVariantSet>(&dataSet, shared_from_this());

    TError error = Prop->Restore(node);
    if (error) {
        TLogger::LogError(error, "Can't restore task's properties");
        return error;
    }

    bool started = true;
    int pid = Prop->GetInt(P_RAW_ROOT_PID);
    if (pid == 0)
        started = false;

    TLogger::Log() << GetName() << ": restore process " << std::to_string(pid) << " which " << (started ? "started" : "didn't start") << std::endl;

    Uid = Prop->GetInt(P_RAW_UID);
    Gid = Prop->GetInt(P_RAW_GID);

    SetState(EContainerState::Stopped);

    if (Parent)
        Parent->Children.push_back(std::weak_ptr<TContainer>(shared_from_this()));

    if (started) {
        TError error = PrepareResources();
        if (error)
            return error;

        error = PrepareTask();
        if (error) {
            TLogger::LogError(error, "Can't prepare task");
            return error;
        }

        error = Task->Restore(pid);
        if (error) {
            Task = nullptr;

            auto cg = GetLeafCgroup(freezerSubsystem);
            if (cg->Exists())
                (void)KillAll();

            TLogger::LogError(error, "Can't restore task");
            return error;
        }

        SetState(Task->IsRunning() ?
                 EContainerState::Running : EContainerState::Stopped);
        if (GetState() == EContainerState::Running)
            MaybeReturnedOk = true;
    } else {
        auto cg = GetLeafCgroup(freezerSubsystem);
        if (!Processes().empty()) {
            // we started container but died before saving root_pid,
            // state may be inconsistent so restart task

            if (cg->Exists())
                (void)KillAll();
            return Start();
        } else {
            // if we didn't start container, make sure nobody is running

            if (cg->Exists())
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

    if (UseParentNamespace() &&
        subsys != freezerSubsystem &&
        subsys != memorySubsystem)
        return Parent->GetLeafCgroup(subsys);

    return Parent->GetLeafCgroup(subsys)->GetChild(Name);
}

bool TContainer::DeliverExitStatus(int pid, int status) {
    if (GetState() != EContainerState::Running || !Task)
        return false;

    if (Task->GetPid() != pid)
        return false;

    Task->DeliverExitStatus(status);
    TLogger::Log() << "Delivered " << status << " to " << GetName() << " with root_pid " << Task->GetPid() << std::endl;
    SetState(EContainerState::Dead);

    if (!Prop->GetBool("isolate"))
        (void)KillAll();

    if (NeedRespawn()) {
        TError error = Respawn();
        TLogger::LogError(error, "Can't respawn " + GetName());
    } else {
        StopChildren();
    }

    TimeOfDeath = GetCurrentTimeMs();
    return true;
}

bool TContainer::NeedRespawn() {
    if (GetState() != EContainerState::Dead)
        return false;

    if (!Prop->GetBool("respawn"))
        return false;

    size_t startTime = TimeOfDeath + config().container().respawn_delay_ms();

    return startTime <= GetCurrentTimeMs() && (Prop->GetInt("max_respawns") < 0 || Data->GetUint(D_RESPAWN_COUNT) < (uint64_t)Prop->GetInt("max_respawns"));
}

TError TContainer::Respawn() {
    TError error = Stop();
    if (error)
        return error;

    uint64_t tmp = Data->GetUint(D_RESPAWN_COUNT);
    error = Start();
    Data->SetUint(D_RESPAWN_COUNT, tmp + 1);
    if (error)
        return error;

    return TError::Success();
}

void TContainer::Heartbeat() {
    if (NeedRespawn()) {
        TError error = Respawn();
        TLogger::LogError(error, "Can't respawn " + GetName());
    }

    if (GetState() != EContainerState::Running || !Task)
        return;

    Task->Rotate();
}

bool TContainer::CanRemoveDead() const {
    return State == EContainerState::Dead &&
        TimeOfDeath + config().container().aging_time_ms() <= GetCurrentTimeMs();
}

bool TContainer::HasChildren() const {
    // link #1 - this
    // link #2 - TContainerHolder->Containers
    // any other link comes from TContainer->Parent and indicates that
    // current container has children
    return shared_from_this().use_count() > 2;
}

bool TContainer::DeliverOom(int fd) {
    auto state = GetState();
    if (!(state == EContainerState::Running ||
          state == EContainerState::Dead))
        return false;

    if (Efd.GetFd() != fd)
        return false;

    int pid = Task->GetPid();
    TLogger::Log() << "Delivered OOM to " << GetName() << " with root_pid " << pid << std::endl;

    TError error = KillAll();
    TLogger::LogError(error, "Can't kill all tasks in container");

    AckExitStatus(pid);
    Task->DeliverExitStatus(SIGKILL);
    SetState(EContainerState::Dead);

    StopChildren();
    error = Data->SetBool(D_OOM_KILLED, true);
    TLogger::LogError(error, "Can't indicate whether container is killed by OOM");
    Efd = -1;
    return true;
}

bool TContainer::DeliverEvent(const TContainerEvent &event) {
    switch (event.Type) {
        case EContainerEventType::Exit:
            return DeliverExitStatus(event.Exit.Pid, event.Exit.Status);
        case EContainerEventType::OOM:
            return DeliverOom(event.Oom.Fd);
        default:
            return false;
    }
}

// TContainerHolder

TError TContainerHolder::GetId(uint16_t &id) {
    for (size_t i = 0; i < sizeof(Ids) / sizeof(Ids[0]); i++) {
        int bit = ffsll(Ids[i]);
        if (bit == 0)
            continue;

        bit--;
        Ids[i] &= ~(1 << bit);
        id = i * BITS_PER_LLONG + bit;
        id++;

        return TError::Success();
    }

    return TError(EError::ResourceNotAvailable, "Can't create more containers");
}

void TContainerHolder::PutId(uint16_t id) {
    id--;

    int bucket = id / BITS_PER_LLONG;
    int bit = id % BITS_PER_LLONG;

    Ids[bucket] |= 1 << bit;
}

TContainerHolder::~TContainerHolder() {
    // we want children to be removed first
    while (Containers.begin() != Containers.end()) {
        Containers.erase(std::prev(Containers.begin()));
    }
}

TError TContainerHolder::CreateRoot() {
    TError error = RegisterProperties();
    if (error)
        return error;

    error = RegisterData();
    if (error)
        return error;

    BootTime = GetBootTime();

    error = Create(ROOT_CONTAINER, -1, -1);
    if (error)
        return error;

    uint16_t id;
    error = GetId(id);
    if (error)
        return error;

    if (id != 2)
        return TError(EError::Unknown, "Unexpected root container id");

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
                        return !(isalnum(c) || c == '_' || c == '/' || c == '-' || c == '@' || c == ':');
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

TError TContainerHolder::Create(const string &name, int uid, int gid) {
    if (!ValidName(name))
        return TError(EError::InvalidValue, "invalid container name " + name);

    if (Containers.find(name) != Containers.end())
        return TError(EError::ContainerAlreadyExists, "container " + name + " already exists");

    auto parent = GetParent(name);
    if (!parent && name != ROOT_CONTAINER)
        return TError(EError::InvalidValue, "invalid parent container");

    uint16_t id;
    TError error = GetId(id);
    if (error)
        return error;

    auto c = std::make_shared<TContainer>(name, parent, id, Link);
    error = c->Create(uid, gid);
    if (error)
        return error;

    Containers[name] = c;
    return TError::Success();
}

shared_ptr<TContainer> TContainerHolder::Get(const string &name) {
    if (Containers.find(name) == Containers.end())
        return nullptr;

    return Containers[name];
}

TError TContainerHolder::CheckPermission(shared_ptr<TContainer> container,
                                         int uid, int gid) {
    int containerUid, containerGid;

    if (uid == 0 || gid == 0)
        return TError::Success();

    container->GetPerm(containerUid, containerGid);

    if (containerUid < 0 || containerGid < 0)
        return TError::Success();

    if (containerUid == uid || containerGid == gid)
        return TError::Success();

    return TError(EError::Permission, "Permission error");
}

TError TContainerHolder::Destroy(const string &name) {
    if (name == ROOT_CONTAINER || Containers.find(name) == Containers.end())
        return TError(EError::InvalidValue, "invalid container name " + name);

    if (Containers[name]->HasChildren())
        return TError(EError::InvalidState, "container has children");

    PutId(Containers[name]->GetId());

    Containers.erase(name);

    return TError::Success();
}

vector<string> TContainerHolder::List() const {
    vector<string> ret;

    for (auto c : Containers)
        ret.push_back(c.second->GetName());

    return ret;
}

TError TContainerHolder::RestoreId(const kv::TNode &node, uint16_t &id) {
    string value = "";
    for (int i = 0; i < node.pairs_size(); i++) {
        auto key = node.pairs(i).key();

        if (key == P_RAW_ID)
            value = node.pairs(i).val();
    }

    if (value.length() == 0) {
        TError error = GetId(id);
        if (error)
            return error;
    } else {
        uint32_t id32;
        TError error = StringToUint32(value, id32);
        if (error)
            return error;

        id = (uint16_t)id32;
    }

    return TError::Success();
}

TError TContainerHolder::Restore(const std::string &name, const kv::TNode &node) {
    if (name == ROOT_CONTAINER)
        return TError::Success();

    // TODO: we DO trust data from the persistent storage, do we?
    auto parent = GetParent(name);
    if (!parent)
        return TError(EError::InvalidValue, "invalid parent container");

    uint16_t id = 0;
    TError error = RestoreId(node, id);
    if (error)
        return error;

    if (!id)
        return TError(EError::Unknown, "Couldn't restore container id");

    auto c = std::make_shared<TContainer>(name, parent, id, Link);
    error = c->Restore(node);
    if (error)
        return error;

    Containers[name] = c;
    return TError::Success();
}

void TContainerHolder::Heartbeat() {
    auto i = Containers.begin();

    while (i != Containers.end()) {
        auto &name = i->first;
        auto c = i->second;
        if (c->CanRemoveDead()) {
            TLogger::Log() << "Remove old dead container " << name << std::endl;
            i = Containers.erase(i);
        } else {
            c->Heartbeat();
            ++i;
        }
    }
}

void TContainerHolder::PushOomFds(vector<int> &fds) {
    for (auto c : Containers) {
        int fd = c.second->GetOomFd();

        if (fd < 0)
            continue;

        fds.push_back(fd);
    }
}

bool TContainerHolder::DeliverEvent(const TContainerEvent &event) {
    for (auto c : Containers)
        if (c.second->DeliverEvent(event))
            return true;

    TLogger::Log() << "Couldn't deliver " << event.GetMsg() << std::endl;
    return false;
}
