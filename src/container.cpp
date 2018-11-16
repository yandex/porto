#include <sstream>
#include <fstream>
#include <memory>
#include <csignal>
#include <cstdlib>
#include <algorithm>
#include <condition_variable>

#include "portod.hpp"
#include "container.hpp"
#include "config.hpp"
#include "task.hpp"
#include "cgroup.hpp"
#include "device.hpp"
#include "property.hpp"
#include "event.hpp"
#include "waiter.hpp"
#include "network.hpp"
#include "epoll.hpp"
#include "kvalue.hpp"
#include "volume.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/cred.hpp"
#include "util/unix.hpp"
#include "util/proc.hpp"
#include "client.hpp"
#include "filesystem.hpp"
#include "rpc.hpp"

extern "C" {
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/reboot.h>
#include <fcntl.h>
#include <sys/fsuid.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/vfs.h>
#include <linux/magic.h>
}

std::mutex ContainersMutex;
static std::condition_variable ContainersCV;
std::shared_ptr<TContainer> RootContainer;
std::map<std::string, std::shared_ptr<TContainer>> Containers;
TPath ContainersKV;
TIdMap ContainerIdMap(1, CONTAINER_ID_MAX);

std::mutex CpuAffinityMutex;
static std::vector<TBitMap> CoreThreads;

static TBitMap NumaNodes;
static std::vector<TBitMap> NodeThreads;

TError TContainer::ValidName(const std::string &name, bool superuser) {

    if (name.length() == 0)
        return TError(EError::InvalidValue, "container path too short");

    unsigned path_max = superuser ? CONTAINER_PATH_MAX_FOR_SUPERUSER : CONTAINER_PATH_MAX;
    if (name.length() > path_max)
        return TError(EError::InvalidValue, "container path too long, limit is {}", path_max);

    if (name[0] == '/') {
        if (name == ROOT_CONTAINER)
            return OK;
        return TError(EError::InvalidValue, "container path starts with '/': " + name);
    }

    for (std::string::size_type first = 0, i = 0; i <= name.length(); i++) {
        switch (name[i]) {
            case '/':
            case '\0':
                if (i == first)
                    return TError(EError::InvalidValue,
                            "double/trailing '/' in container path: " + name);
                if (i - first > CONTAINER_NAME_MAX)
                    return TError(EError::InvalidValue,
                            "container name component too long, limit is " +
                            std::to_string(CONTAINER_NAME_MAX) +
                            ": '" + name.substr(first, i - first) + "'");
                if (name.substr(first, i - first) == SELF_CONTAINER)
                    return TError(EError::InvalidValue,
                            "container name 'self' is reserved");
                if (name.substr(first, i - first) == DOT_CONTAINER)
                    return TError(EError::InvalidValue,
                            "container name '.' is reserved");
                first = i + 1;
            case 'a'...'z':
            case 'A'...'Z':
            case '0'...'9':
            case '_':
            case '-':
            case '@':
            case ':':
            case '.':
                /* Ok */
                break;
            default:
                return TError(EError::InvalidValue, "forbidden character " +
                              StringFormat("%#x", (unsigned char)name[i]));
        }
    }

    return OK;
}

std::string TContainer::ParentName(const std::string &name) {
    auto sep = name.rfind('/');
    if (sep == std::string::npos)
        return ROOT_CONTAINER;
    return name.substr(0, sep);
}

std::shared_ptr<TContainer> TContainer::Find(const std::string &name) {
    PORTO_LOCKED(ContainersMutex);
    auto it = Containers.find(name);
    if (it == Containers.end())
        return nullptr;
    return it->second;
}

TError TContainer::Find(const std::string &name, std::shared_ptr<TContainer> &ct) {
    ct = Find(name);
    if (ct)
        return OK;
    return TError(EError::ContainerDoesNotExist, "container " + name + " not found");
}

TError TContainer::FindTaskContainer(pid_t pid, std::shared_ptr<TContainer> &ct) {
    TError error;
    TCgroup cg;

    error = FreezerSubsystem.TaskCgroup(pid, cg);
    if (error)
        return error;

    if (cg.Name == PORTO_DAEMON_CGROUP)
        return TError(EError::NotSupported, "Recursion?");

    std::string prefix = std::string(PORTO_CGROUP_PREFIX) + "/";
    std::string name = cg.Name;
    std::replace(name.begin(), name.end(), '%', '/');

    auto containers_lock = LockContainers();

    if (!StringStartsWith(name, prefix))
        return TContainer::Find(ROOT_CONTAINER, ct);

    return TContainer::Find(name.substr(prefix.length()), ct);
}

/* lock subtree shared or exclusive */
TError TContainer::LockAction(std::unique_lock<std::mutex> &containers_lock, bool shared) {
    L_DBG("LockAction{} CT{}:{}", (shared ? "Shared" : ""), Id, Name);

    while (1) {
        if (State == EContainerState::Destroyed) {
            L_DBG("Lock failed, CT{}:{} was destroyed", Id, Name);
            return TError(EError::ContainerDoesNotExist, "Container was destroyed");
        }
        bool busy;
        if (shared)
            busy = ActionLocked < 0 || PendingWrite || SubtreeWrite;
        else
            busy = ActionLocked || SubtreeRead || SubtreeWrite;
        for (auto ct = Parent.get(); !busy && ct; ct = ct->Parent.get())
            busy = ct->PendingWrite || (shared ? ct->ActionLocked < 0 : ct->ActionLocked);
        if (!busy)
            break;
        if (!shared)
            PendingWrite = true;
        ContainersCV.wait(containers_lock);
    }
    PendingWrite = false;
    ActionLocked += shared ? 1 : -1;
    LastActionPid = GetTid();
    for (auto ct = Parent.get(); ct; ct = ct->Parent.get()) {
        if (shared)
            ct->SubtreeRead++;
        else
            ct->SubtreeWrite++;
    }
    return OK;
}

void TContainer::UnlockAction(bool containers_locked) {
    L_DBG("UnlockAction{} CT{}:{}", (ActionLocked > 0 ? "Shared" : ""), Id, Name);
    if (!containers_locked)
        ContainersMutex.lock();
    for (auto ct = Parent.get(); ct; ct = ct->Parent.get()) {
        if (ActionLocked > 0) {
            PORTO_ASSERT(ct->SubtreeRead > 0);
            ct->SubtreeRead--;
        } else {
            PORTO_ASSERT(ct->SubtreeWrite > 0);
            ct->SubtreeWrite--;
        }
    }
    PORTO_ASSERT(ActionLocked);
    ActionLocked += (ActionLocked > 0) ? -1 : 1;
    /* not so effective and fair but simple */
    ContainersCV.notify_all();
    if (!containers_locked)
        ContainersMutex.unlock();
}

bool TContainer::IsActionLocked(bool shared) {
    for (auto ct = this; ct; ct = ct->Parent.get())
        if (ct->ActionLocked < 0 || (shared && ct->ActionLocked > 0))
            return true;
    return false;
}

void TContainer::DowngradeActionLock() {
    auto lock = LockContainers();
    PORTO_ASSERT(ActionLocked == -1);

    L_DBG("Downgrading exclusive to shared CT{}:{}", Id, Name);

    for (auto ct = Parent.get(); ct; ct = ct->Parent.get()) {
        ct->SubtreeRead++;
        ct->SubtreeWrite--;
    }

    ActionLocked = 1;
    ContainersCV.notify_all();
}

/* only after downgrade */
void TContainer::UpgradeActionLock() {
    auto lock = LockContainers();

    L_DBG("Upgrading shared back to exclusive CT{}:{}", Id, Name);

    PendingWrite = true;

    for (auto ct = Parent.get(); ct; ct = ct->Parent.get()) {
        ct->SubtreeRead--;
        ct->SubtreeWrite++;
    }

    while (ActionLocked != 1)
        ContainersCV.wait(lock);

    ActionLocked = -1;
    LastActionPid = GetTid();

    PendingWrite = false;
}

void TContainer::LockStateRead() {
    auto lock = LockContainers();
    L_DBG("LockStateRead CT{}:{}", Id, Name);
    while (StateLocked < 0)
        ContainersCV.wait(lock);
    StateLocked++;
    LastStatePid = GetTid();
}

void TContainer::LockStateWrite() {
    auto lock = LockContainers();
    L_DBG("LockStateWrite CT{}:{}", Id, Name);
    while (StateLocked < 0)
        ContainersCV.wait(lock);
    StateLocked = -1 - StateLocked;
    while (StateLocked != -1)
        ContainersCV.wait(lock);
    LastStatePid = GetTid();
}

void TContainer::DowngradeStateLock() {
    auto lock = LockContainers();
    L_DBG("DowngradeStateLock CT{}:{}", Id, Name);
    PORTO_ASSERT(StateLocked == -1);
    StateLocked = 1;
    ContainersCV.notify_all();
}

void TContainer::UnlockState() {
    auto lock = LockContainers();
    L_DBG("UnlockState CT{}:{}", Id, Name);
    PORTO_ASSERT(StateLocked);
    if (StateLocked > 0)
        --StateLocked;
    else if (++StateLocked >= -1)
        ContainersCV.notify_all();
}

void TContainer::DumpLocks() {
    auto lock = LockContainers();
    for (auto &it: Containers) {
        auto &ct = it.second;
        if (ct->ActionLocked || ct->PendingWrite || ct->StateLocked ||
                ct->SubtreeRead || ct->SubtreeWrite)
            L_SYS("CT{}:{} StateLocked {} by {} ActionLocked {} by {} Read {} Write {}{}",
                  ct->Id, ct->Name, ct->StateLocked, ct->LastStatePid,
                  ct->ActionLocked, ct->LastActionPid,
                  ct->SubtreeRead, ct->SubtreeWrite,
                  (ct->PendingWrite ? " PendingWrite" : ""));
    }
}

void TContainer::Register() {
    PORTO_LOCKED(ContainersMutex);
    Containers[Name] = shared_from_this();
    if (Parent)
        Parent->Children.emplace_back(shared_from_this());
    Statistics->ContainersCreated++;
}

void TContainer::Unregister() {
    PORTO_LOCKED(ContainersMutex);
    Containers.erase(Name);
    if (Parent)
        Parent->Children.remove(shared_from_this());

    TError error = ContainerIdMap.Put(Id);
    if (error)
        L_WRN("Cannot put CT{}:{} id: {}", Id, Name, error);

    PORTO_ASSERT(State == EContainerState::Stopped);
    State = EContainerState::Destroyed;
}

TContainer::TContainer(std::shared_ptr<TContainer> parent, int id, const std::string &name) :
    Parent(parent), Level(parent ? parent->Level + 1 : 0), Id(id), Name(name),
    FirstName(!parent ? "" : parent->IsRoot() ? name : name.substr(parent->Name.length() + 1)),
    Stdin(0), Stdout(1), Stderr(2),
    ClientsCount(0), ContainerRequests(0), OomEvents(0)
{
    Statistics->ContainersCount++;
    RealCreationTime = time(nullptr);

    memset(&TaintFlags, 0, sizeof(TaintFlags));

    std::fill(PropSet, PropSet + sizeof(PropSet), false);
    std::fill(PropDirty, PropDirty + sizeof(PropDirty), false);


    Stdin.SetOutside("/dev/null");
    Stdout.SetOutside("stdout");
    Stderr.SetOutside("stderr");
    Stdout.Limit = config().container().stdout_limit();
    Stderr.Limit = config().container().stdout_limit();
    Root = "/";
    RootPath = Parent ? Parent->RootPath : TPath("/");
    RootRo = false;
    Umask = 0002;
    Isolate = true;
    HostMode = IsRoot();

    NetProp = { { "inherited" } };
    NetIsolate = false;
    NetInherit = true;

    IpLimit = { { "any" } };
    IpPolicy = "any";

    Hostname = "";
    CapAmbient = NoCapabilities;
    CapAllowed = NoCapabilities;
    CapLimit = NoCapabilities;
    CapBound = NoCapabilities;

    if (IsRoot())
        NsName = ROOT_PORTO_NAMESPACE;
    else if (config().container().default_porto_namespace())
        NsName = FirstName + "/";
    else
        NsName = "";
    SetProp(EProperty::PORTO_NAMESPACE);

    if (IsRoot())
        PlacePolicy = { PORTO_PLACE, "***" };
    else
        PlacePolicy = Parent->PlacePolicy;

    CpuPolicy = "normal";
    ChooseSchedPolicy();

    CpuPeriod = config().container().cpu_period();

    if (IsRoot()) {
        CpuLimit = GetNumCores() * CPU_POWER_PER_SEC;
        SetProp(EProperty::CPU_LIMIT);
        SetProp(EProperty::MEM_LIMIT);
    }

    IoPolicy = "";
    IoPrio = 0;

    NetClass.DefaultTos = TNetwork::DefaultTos;
    NetClass.Fold = &NetClass;

    PressurizeOnDeath = config().container().pressurize_on_death();
    RunningChildren = 0;
    StartingChildren = 0;

    if (IsRoot()) {
        for (auto &extra: config().container().extra_env())
            EnvCfg += fmt::format("{}={};", extra.name(), extra.value());
    }

    Controllers |= CGROUP_FREEZER;

    if (CpuacctSubsystem.Controllers == CGROUP_CPUACCT)
        Controllers |= CGROUP_CPUACCT;

    if (Level <= 1) {
        Controllers |= CGROUP_MEMORY | CGROUP_CPU | CGROUP_CPUACCT |
                       CGROUP_NETCLS | CGROUP_DEVICES;

        if (BlkioSubsystem.Supported)
            Controllers |= CGROUP_BLKIO;

        if (CpusetSubsystem.Supported)
            Controllers |= CGROUP_CPUSET;

        if (HugetlbSubsystem.Supported)
            Controllers |= CGROUP_HUGETLB;
    }

    if (Level == 1 && config().container().memory_limit_margin()) {
        uint64_t total = GetTotalMemory();

        MemLimit = total - std::min(total / 4,
                config().container().memory_limit_margin());
        SetPropDirty(EProperty::MEM_LIMIT);

        if (MemorySubsystem.SupportAnonLimit() &&
                config().container().anon_limit_margin()) {
            AnonMemLimit = MemLimit - std::min(MemLimit / 4,
                    config().container().anon_limit_margin());
            SetPropDirty(EProperty::ANON_LIMIT);
        }
    }

    if (Level == 1 && PidsSubsystem.Supported) {
        Controllers |= CGROUP_PIDS;

        if (config().container().default_thread_limit()) {
            ThreadLimit = config().container().default_thread_limit();
            SetProp(EProperty::THREAD_LIMIT);
        }
    }

    SetProp(EProperty::CONTROLLERS);

    RespawnDelay = config().container().respawn_delay_ms() * 1000000;

    Private = "";
    AgingTime = config().container().default_aging_time_s() * 1000;

    if (Parent && Parent->AccessLevel == EAccessLevel::None)
        AccessLevel = EAccessLevel::None;
    else if (Parent && Parent->AccessLevel <= EAccessLevel::ReadOnly)
        AccessLevel = EAccessLevel::ReadOnly;
    else
        AccessLevel = EAccessLevel::Normal;
    SetProp(EProperty::ENABLE_PORTO);
}

TContainer::~TContainer() {
    PORTO_ASSERT(Net == nullptr);
    PORTO_ASSERT(!NetClass.Registered);
    Statistics->ContainersCount--;
    if (TaintFlags.TaintCounted && Statistics->ContainersTainted)
        Statistics->ContainersTainted--;
}

TError TContainer::Create(const std::string &name, std::shared_ptr<TContainer> &ct) {
    auto max_ct = config().container().max_total();
    TError error;
    int id = -1;

    if (CL->IsSuperUser())
        max_ct += NR_SUPERUSER_CONTAINERS;

    error = ValidName(name, CL->IsSuperUser());
    if (error)
        return error;

    auto lock = LockContainers();

    auto parent = TContainer::Find(TContainer::ParentName(name));
    if (parent) {
        if (parent->Level == CONTAINER_LEVEL_MAX)
            return TError(EError::InvalidValue, "You shall not go deeper! Maximum level is {}", CONTAINER_LEVEL_MAX);
        error = parent->LockActionShared(lock);
        if (error)
            return error;
        error = CL->CanControl(*parent, true);
        if (error)
            goto err;
    } else if (name != ROOT_CONTAINER)
        return TError(EError::ContainerDoesNotExist, "parent container not found for " + name);

    if (Containers.find(name) != Containers.end()) {
        error = TError(EError::ContainerAlreadyExists, "container " + name + " already exists");
        goto err;
    }

    if (Containers.size() >= max_ct + NR_SERVICE_CONTAINERS) {
        error = TError(EError::ResourceNotAvailable,
                "number of containers reached limit: " + std::to_string(max_ct));
        goto err;
    }

    error = ContainerIdMap.Get(id);
    if (error)
        goto err;

    L_ACT("Create CT{}:{}", id, name);

    ct = std::make_shared<TContainer>(parent, id, name);

    ct->OwnerCred = CL->Cred;
    ct->SetProp(EProperty::OWNER_USER);
    ct->SetProp(EProperty::OWNER_GROUP);

    /*
     * For sub-containers of client container use its task credentials.
     * This is safe because new container will have the same restrictions.
     */
    if (ct->IsChildOf(*CL->ClientContainer))
        ct->TaskCred = CL->TaskCred;
    else
        ct->TaskCred = CL->Cred;

    ct->SetProp(EProperty::USER);
    ct->SetProp(EProperty::GROUP);

    ct->SanitizeCapabilities();

    ct->SetProp(EProperty::STATE);

    ct->RespawnCount = 0;
    ct->SetProp(EProperty::RESPAWN_COUNT);

    error = ct->Save();
    if (error)
        goto err;

    ct->Register();

    if (parent)
        parent->UnlockAction(true);

    TContainerWaiter::ReportAll(*ct);

    return OK;

err:
    if (parent)
        parent->UnlockAction(true);
    if (id >= 0)
        ContainerIdMap.Put(id);
    ct = nullptr;
    return error;
}

TError TContainer::Restore(const TKeyValue &kv, std::shared_ptr<TContainer> &ct) {
    TError error;
    int id;

    error = StringToInt(kv.Get(P_RAW_ID), id);
    if (error)
        return error;

    L_ACT("Restore CT{}:{}", id, kv.Name);

    auto lock = LockContainers();

    if (Containers.find(kv.Name) != Containers.end())
        return TError(EError::ContainerAlreadyExists, kv.Name);

    std::shared_ptr<TContainer> parent;
    error = TContainer::Find(TContainer::ParentName(kv.Name), parent);
    if (error)
        return error;

    error = ContainerIdMap.GetAt(id);
    if (error)
        return error;

    ct = std::make_shared<TContainer>(parent, id, kv.Name);

    ct->Register();

    lock.unlock();

    error = SystemClient.LockContainer(ct);
    if (error)
        goto err;

    error = ct->Load(kv);
    if (error)
        goto err;

    ct->SyncState();

    TNetwork::InitClass(*ct);

    /* Do not rewrite resolv.conf at restore */
    ct->TestClearPropDirty(EProperty::RESOLV_CONF);

    /* Restore cgroups only for running containers */
    if (ct->State != EContainerState::Stopped &&
            ct->State != EContainerState::Dead) {

        error = TNetwork::RestoreNetwork(*ct);
        if (error)
            goto err;

        error = ct->PrepareCgroups();
        if (error)
            goto err;

        error = ct->PrepareOomMonitor();
        if (error)
            goto err;

        /* Kernel without group rt forbids moving RT tasks in to cpu cgroup */
        if (ct->Task.Pid && !CpuSubsystem.HasRtGroup) {
            auto cpuCg = ct->GetCgroup(CpuSubsystem);
            TCgroup cg;

            if (!CpuSubsystem.TaskCgroup(ct->Task.Pid, cg) && cg != cpuCg) {
                auto freezerCg = ct->GetCgroup(FreezerSubsystem);
                bool smart;

                /* Disable smart if we're moving tasks into another cgroup */
                if (!cg.GetBool("cpu.smart", smart) && smart) {
                    cg.SetBool("cpu.smart", false);
                } else if (!CpuSubsystem.HasRtGroup) {
                    std::vector<pid_t> prev, pids;
                    struct sched_param param;
                    param.sched_priority = 0;
                    bool retry;

                    /* Disable RT for all task in freezer cgroup */
                    do {
                        error = freezerCg.GetTasks(pids);
                        retry = false;
                        for (auto pid: pids) {
                            if (std::find(prev.begin(), prev.end(), pid) == prev.end() &&
                                    sched_getscheduler(pid) == SCHED_RR &&
                                    !sched_setscheduler(pid, SCHED_OTHER, &param))
                                retry = true;
                        }
                        prev = pids;
                    } while (retry);
                }

                /* Move tasks into correct cpu cgroup before enabling RT */
                if (!CpuSubsystem.HasRtGroup && ct->SchedPolicy == SCHED_RR) {
                    error = cpuCg.AttachAll(freezerCg);
                    if (error)
                        L_WRN("Cannot move to corrent cpu cgroup: {}", error);
                }
            }
        }

        /* Disable memory guarantee in old cgroup */
        if (ct->MemGuarantee) {
            TCgroup memCg;
            if (!MemorySubsystem.TaskCgroup(ct->Task.Pid, memCg) &&
                    memCg != ct->GetCgroup(MemorySubsystem))
                MemorySubsystem.SetGuarantee(memCg, 0);
        }

        error = ct->ApplyDynamicProperties();
        if (error)
            goto err;

        ct->PropagateCpuLimit();

        error = ct->SyncCgroups();
        if (error)
            goto err;
    }

    if (ct->State == EContainerState::Dead && ct->AutoRespawn)
        ct->ScheduleRespawn();

    /* Do not apply dynamic properties to dead container */
    if (ct->State == EContainerState::Dead)
        memset(ct->PropDirty, 0, sizeof(ct->PropDirty));

    error = ct->Save();
    if (error)
        goto err;

    if (ct->State == EContainerState::Stopped)
        ct->RemoveWorkDir();

    SystemClient.ReleaseContainer();

    return OK;

err:
    TNetwork::StopNetwork(*ct);
    ct->SetState(EContainerState::Stopped);
    ct->RemoveWorkDir();
    lock.lock();
    SystemClient.ReleaseContainer(true);
    ct->Unregister();
    ct = nullptr;
    return error;
}

std::string TContainer::StateName(EContainerState state) {
    switch (state) {
    case EContainerState::Stopped:
        return "stopped";
    case EContainerState::Dead:
        return "dead";
    case EContainerState::Respawning:
        return "respawning";
    case EContainerState::Starting:
        return "starting";
    case EContainerState::Running:
        return "running";
    case EContainerState::Stopping:
        return "stopping";
    case EContainerState::Paused:
        return "paused";
    case EContainerState::Meta:
        return "meta";
    case EContainerState::Destroyed:
        return "destroyed";
    default:
        return "unknown";
    }
}

EContainerState TContainer::ParseState(const std::string &name) {
    if (name == "stopped")
        return EContainerState::Stopped;
    if (name == "dead")
        return EContainerState::Dead;
    if (name == "respawning")
        return EContainerState::Respawning;
    if (name == "starting")
        return EContainerState::Starting;
    if (name == "running")
        return EContainerState::Running;
    if (name == "stopping")
        return EContainerState::Stopping;
    if (name == "paused")
        return EContainerState::Paused;
    if (name == "meta")
        return EContainerState::Meta;
    return EContainerState::Destroyed;
}

TError TContainer::ValidLabel(const std::string &label, const std::string &value) {

    if (label.size() > PORTO_LABEL_NAME_LEN_MAX)
        return TError(EError::InvalidLabel, "Label name too log, max {} bytes", PORTO_LABEL_NAME_LEN_MAX);

    if (value.size() > PORTO_LABEL_VALUE_LEN_MAX)
        return TError(EError::InvalidLabel, "Label value too log, max {} bytes", PORTO_LABEL_VALUE_LEN_MAX);

    auto sep = label.find('.');
    if (sep == std::string::npos ||
            sep < PORTO_LABEL_PREFIX_LEN_MIN ||
            sep > PORTO_LABEL_PREFIX_LEN_MAX ||
            label.find_first_not_of(PORTO_LABEL_PREFIX_CHARS) < sep ||
            label.find_first_not_of(PORTO_NAME_CHARS) != std::string::npos ||
            StringStartsWith(label, "PORTO"))
        return TError(EError::InvalidLabel, "Invalid label name: {}", label);

    if (value.find_first_not_of(PORTO_NAME_CHARS) != std::string::npos)
        return TError(EError::InvalidLabel, "Invalid label value: {}", value);

    return OK;
}

TError TContainer::GetLabel(const std::string &label, std::string &value) const {
    if (label[0] == '.') {
        auto nm = label.substr(1);
        for (auto ct = this; ct; ct = ct->Parent.get()) {
            auto it = ct->Labels.find(nm);
            if (it != ct->Labels.end()) {
                value = it->second;
                return OK;
            }
        }
    } else {
        auto it = Labels.find(label);
        if (it != Labels.end()) {
            value = it->second;
            return OK;
        }
    }
    return TError(EError::LabelNotFound, "Label {} is not set", label);
}

void TContainer::SetLabel(const std::string &label, const std::string &value) {
    if (value.empty())
        Labels.erase(label);
    else
        Labels[label] = value;
    SetProp(EProperty::LABELS);
}

TError TContainer::IncLabel(const std::string &label, int64_t &result, int64_t add) {
    int64_t val;
    result = 0;

    auto it = Labels.find(label);
    if (it == Labels.end()) {
        if (!add)
            return TError(EError::LabelNotFound, "Label {} is not set", label);
        if (Labels.size() >= PORTO_LABEL_COUNT_MAX)
            return TError(EError::ResourceNotAvailable, "Too many labels");
        val = 0;
    } else {
        TError error = StringToInt64(it->second, val);
        if (error)
            return error;
    }

    result = val;

    if ((add > 0 && val > INT64_MAX - add) || (add < 0 && val < INT64_MIN - add))
        return TError(EError::InvalidValue, "Label {} overflow {} + {}", label, val, add);

    val += add;

    if (it == Labels.end())
        Labels[label] = std::to_string(val);
    else
        it->second = std::to_string(val);

    result = val;

    SetProp(EProperty::LABELS);
    return OK;
}

TPath TContainer::WorkDir() const {
    return TPath(PORTO_WORKDIR) / Name;
}

TError TContainer::CreateWorkDir() const {
    if (IsRoot())
        return OK;

    TFile dir;
    TError error;

    error = dir.OpenDir(PORTO_WORKDIR);
    if (error)
        return error;

    if (Level > 1) {
        for (auto &name: TPath(Parent->Name).Components()) {
            error = dir.OpenDirStrictAt(dir, name);
            if (error)
                return error;
        }
    }

    TPath name = FirstName;

    if (dir.ExistsAt(name)) {
        L_ACT("Remove stale working dir");
        error = dir.RemoveAt(name);
        if (error)
            L_ERR("Cannot remove working dir: {}", error);
    }

    error = dir.MkdirAt(name, 0775);
    if (!error) {
        error = dir.ChownAt(name, TaskCred);
        if (error)
            (void)dir.RemoveAt(name);
    }

    if (error) {
        if (error.Errno == ENOSPC || error.Errno == EROFS)
            L("Cannot create working dir: {}", error);
        else
            L_ERR("Cannot create working dir: {}", error);
    }

    return error;
}

void TContainer::RemoveWorkDir() const {
    if (IsRoot() || !WorkDir().Exists())
        return;

    TFile dir;
    TError error;

    error = dir.OpenDir(PORTO_WORKDIR);
    if (!error && Level > 1) {
        for (auto &name: TPath(Parent->Name).Components()) {
            error = dir.OpenDirStrictAt(dir, name);
            if (error)
                break;
        }
    }
    if (!error)
        error = dir.RemoveAt(FirstName);
    if (error)
        L_ERR("Cannot remove working dir: {}", error);
}

TPath TContainer::GetCwd() const {
    TPath cwd;

    for (auto ct = shared_from_this(); ct; ct = ct->Parent) {
        if (!ct->Cwd.IsEmpty())
            cwd = ct->Cwd / cwd;
        if (cwd.IsAbsolute())
            return cwd;
        if (ct->Root != "/")
            return TPath("/") / cwd;
    }

    if (IsRoot())
        return TPath("/");

    return WorkDir();
}

int TContainer::GetExitCode() const {
    if (State != EContainerState::Dead &&
            State != EContainerState::Respawning)
        return 256;
    if (OomKilled)
        return -99;
    if (WIFSIGNALED(ExitStatus))
        return -WTERMSIG(ExitStatus);
    return WEXITSTATUS(ExitStatus);
}

TError TContainer::UpdateSoftLimit() {
    auto lock = LockContainers();
    TError error;

    for (auto ct = this; !ct->IsRoot(); ct = ct->Parent.get()) {
        if (!(ct->Controllers & CGROUP_MEMORY))
            continue;

        int64_t lim = -1;

        /* Set memory soft limit for dead or hollow meta containers */
        if (ct->PressurizeOnDeath &&
                (ct->State == EContainerState::Dead ||
                 (ct->State == EContainerState::Meta &&
                  !ct->RunningChildren && !ct->StartingChildren)))
            lim = config().container().dead_memory_soft_limit();

        if (ct->MemSoftLimit != lim) {
            auto cg = ct->GetCgroup(MemorySubsystem);
            error = MemorySubsystem.SetSoftLimit(cg, lim);
            if (error)
                return error;
            ct->MemSoftLimit = lim;
        }
    }

    return OK;
}

void TContainer::SetState(EContainerState next) {
    if (State == next)
        return;

    L_ACT("Change CT{}:{} state {} -> {}", Id, Name, StateName(State), StateName(next));

    LockStateWrite();

    auto prev = State;
    State = next;

    if (prev == EContainerState::Starting || next == EContainerState::Starting) {
        for (auto p = Parent; p; p = p->Parent)
            p->StartingChildren += next == EContainerState::Starting ? 1 : -1;
    }

    if (prev == EContainerState::Running || next == EContainerState::Running) {
        for (auto p = Parent; p; p = p->Parent)
            p->RunningChildren += next == EContainerState::Running ? 1 : -1;
    }

    if (next == EContainerState::Dead && AutoRespawn)
        ScheduleRespawn();

    DowngradeStateLock();

    if (prev == EContainerState::Running || next == EContainerState::Running) {
        for (auto p = Parent; p; p = p->Parent)
            if (!p->RunningChildren && p->State == EContainerState::Meta)
                TContainerWaiter::ReportAll(*p);
    }

    std::string label, value;

    if ((State == EContainerState::Stopped ||
         State == EContainerState::Dead ||
         State ==  EContainerState::Respawning) && StartError) {
        label = P_START_ERROR;
        value = StartError.ToString();
    } else if (State == EContainerState::Dead ||
               State == EContainerState::Respawning) {
        label = P_EXIT_CODE;
        value = std::to_string(GetExitCode());
    }

    TContainerWaiter::ReportAll(*this, label, value);

    UnlockState();
}

TError TContainer::Destroy(std::list<std::shared_ptr<TVolume>> &unlinked) {
    TError error;

    L_ACT("Destroy CT{}:{}", Id, Name);

    if (State != EContainerState::Stopped) {
        error = Stop(0);
        if (error)
            return error;
    }

    if (!Children.empty()) {
        for (auto &ct: Subtree()) {
            if (ct.get() != this) {
                TVolume::UnlinkAllVolumes(ct, unlinked);
                error = ct->Destroy(unlinked);
                if (error)
                    return error;
            }
        }
    }

    TVolume::UnlinkAllVolumes(shared_from_this(), unlinked);

    TPath path(ContainersKV / std::to_string(Id));
    error = path.Unlink();
    if (error)
        L_ERR("Can't remove key-value node {}: {}", path, error);

    auto lock = LockContainers();
    Unregister();
    lock.unlock();

    TContainerWaiter::ReportAll(*this);

    return OK;
}

bool TContainer::IsChildOf(const TContainer &ct) const {
    for (auto ptr = Parent.get(); ptr; ptr = ptr->Parent.get()) {
        if (ptr == &ct)
            return true;
    }
    return false;
}

/* Subtree in DFS post-order: childs first */
std::list<std::shared_ptr<TContainer>> TContainer::Subtree() {
    std::list<std::shared_ptr<TContainer>> subtree {shared_from_this()};
    auto lock = LockContainers();
    for (auto it = subtree.rbegin(); it != subtree.rend(); ++it) {
        for (auto child: (*it)->Children)
            subtree.emplace(std::prev(it.base()), child);
    }
    return subtree;
}

/* Builds list of childs at this moment. */
std::list<std::shared_ptr<TContainer>> TContainer::Childs() {
    auto lock = LockContainers();
    auto childs(Children);
    return childs;
}

std::shared_ptr<TContainer> TContainer::GetParent() const {
    return Parent;
}

bool TContainer::HasPidFor(const TContainer &ct) const {
    auto ns = &ct;

    while (!ns->Isolate && ns->Parent)
        ns = ns->Parent.get();

    return ns == this || IsChildOf(*ns);
}

TError TContainer::GetPidFor(pid_t pidns, pid_t &pid) const {
    ino_t inode = TNamespaceFd::PidInode(pidns, "ns/pid");
    TError error;

    if (IsRoot()) {
        pid = 1;
    } else if (!Task.Pid) {
        error = TError(EError::InvalidState, "container isn't running");
    } else if (TNamespaceFd::PidInode(getpid(), "ns/pid") == inode) {
        pid = Task.Pid;
    } else if (WaitTask.Pid != Task.Pid && TNamespaceFd::PidInode(WaitTask.Pid, "ns/pid") == inode) {
        pid = TaskVPid;
    } else if (TNamespaceFd::PidInode(Task.Pid, "ns/pid") == inode) {
        if (!Isolate)
            pid = TaskVPid;
        else if (OsMode || IsMeta())
            pid = 1;
        else
            pid = 2;
    } else {
        error = TranslatePid(-Task.Pid, pidns, pid);
        if (!pid && !error)
            error = TError(EError::Permission, "pid is unreachable");
    }
    return error;
}

TError TContainer::GetThreadCount(uint64_t &count) const {
    if (IsRoot()) {
        struct sysinfo si;
        if (sysinfo(&si) < 0)
            return TError::System("sysinfo");
        count = si.procs;
    } else if (Controllers & CGROUP_PIDS) {
        auto cg = GetCgroup(PidsSubsystem);
        return PidsSubsystem.GetUsage(cg, count);
    } else {
        auto cg = GetCgroup(FreezerSubsystem);
        return cg.GetCount(true, count);
    }
    return OK;
}

TError TContainer::GetProcessCount(uint64_t &count) const {
    TError error;
    if (IsRoot()) {
        struct stat st;
        error = TPath("/proc").StatStrict(st);
        if (error)
            return error;
        count = st.st_nlink > ProcBaseDirs ? st.st_nlink  - ProcBaseDirs : 0;
    } else {
        auto cg = GetCgroup(FreezerSubsystem);
        return cg.GetCount(false, count);
    }
    return OK;
}

TError TContainer::GetVmStat(TVmStat &stat) const {
    auto cg = GetCgroup(FreezerSubsystem);
    std::vector<pid_t> pids;
    TError error;

    error = cg.GetProcesses(pids);
    if (error)
        return error;

    for (auto pid: pids)
        stat.Parse(pid);

    return OK;
}

void TContainer::CollectOomKills(bool event) {

    /*
     * Kernel sends OOM events before actual OOM kill into each cgroup in subtree.
     * We turn these events into speculative OOM kill.
     */

    auto plan = Subtree();
    for (auto p = Parent; event && p; p = p->Parent)
        plan.push_front(p);

    auto total = OomKillsTotal;

    for (auto &ct: plan) {
        if (!HasResources() || !(ct->Controllers & CGROUP_MEMORY))
            continue;

        auto cg = ct->GetCgroup(MemorySubsystem);
        uint64_t kills = 0;

        if (MemorySubsystem.GetOomKills(cg, kills))
            continue;

        auto lock = LockContainers();

        /* Collect this kill later from child event. */
        if (event && kills > ct->OomKillsRaw && ct->Level > Level)
            break;

        /* Already have speculative kill at parent, ignore this event. */
        if (event && ct->OomKills > ct->OomKillsRaw && ct->Level < Level)
            event = false;

        if (kills > ct->OomKillsRaw)
            L_EVT("OOM Kill in CT{}:{}", ct->Id, ct->Name);

        ct->OomKillsRaw = kills;

        if (event && ct.get() == this &&
                OomKills == OomKillsRaw &&
                OomKillsTotal == total) {
            L_EVT("Speculative OOM Kill in CT{}:{}", Id, Name);
            kills++;
        }

        /* Nothing new happened. */
        if (ct->OomKills >= kills)
            continue;

        kills -= ct->OomKills;

        ct->OomKills += kills;
        ct->SetProp(EProperty::OOM_KILLS);

        ct->OomKillsTotal += kills;
        ct->SetProp(EProperty::OOM_KILLS_TOTAL);

        for (auto p = ct->Parent; kills && p ; p = p->Parent) {
            if (p->OomKills > p->OomKillsRaw) {
                L_EVT("Move speculative OOM Kill from CT{}:{} into CT{}:{}", p->Id, p->Name, ct->Id, ct->Name);
                p->OomKills--;
                kills--;
            }
            p->OomKillsTotal += kills;
            p->SetProp(EProperty::OOM_KILLS_TOTAL);
        }

        lock.unlock();

        for (auto p = ct; p ; p = p->Parent)
            p->Save();
    }
}

TError TContainer::CheckMemGuarantee() const {
    uint64_t total = GetTotalMemory();
    uint64_t usage = RootContainer->GetTotalMemGuarantee();
    uint64_t old_usage = usage - std::min(NewMemGuarantee, usage);
    uint64_t reserve = config().daemon().memory_guarantee_reserve();
    uint64_t hugetlb = GetHugetlbMemory();

    if (usage + reserve + hugetlb > total) {

        /*
         * under overcommit allow to start containers without guarantee
         * for root user or nested containers
         */
        if (!NewMemGuarantee && (Level > 1 || CL->IsSuperUser()))
            return OK;

        return TError(EError::ResourceNotAvailable,
                "Memory guarantee overcommit: requested {}, available {}, guaranteed {} + reserved {} + hugetlb {} of {}",
                StringFormatSize(NewMemGuarantee),
                StringFormatSize(total - std::min(total, old_usage + reserve + hugetlb)),
                StringFormatSize(old_usage),
                StringFormatSize(reserve),
                StringFormatSize(hugetlb),
                StringFormatSize(total));
    }

    return OK;
}

uint64_t TContainer::GetTotalMemGuarantee(bool containers_locked) const {
    uint64_t sum = 0lu;

    /* Stopped container doesn't have memory guarantees */
    if (State == EContainerState::Stopped)
        return 0;

    if (!containers_locked)
        ContainersMutex.lock();

    for (auto &child : Children)
        sum += child->GetTotalMemGuarantee(true);

    sum = std::max(NewMemGuarantee, sum);

    if (!containers_locked)
        ContainersMutex.unlock();

    return sum;
}

uint64_t TContainer::GetMemLimit(bool effective) const {
    uint64_t lim = 0;

    for (auto ct = this; ct; ct = ct->Parent.get())
        if ((effective || ct->HasProp(EProperty::MEM_LIMIT)) &&
                ct->MemLimit && (ct->MemLimit < lim || !lim))
            lim = ct->MemLimit;

    if (effective && !lim)
        lim = GetTotalMemory();

    return lim;
}

uint64_t TContainer::GetAnonMemLimit(bool effective) const {
    uint64_t lim = 0;

    for (auto ct = this; ct; ct = ct->Parent.get())
        if ((effective || ct->HasProp(EProperty::ANON_LIMIT)) &&
                ct->AnonMemLimit && (ct->AnonMemLimit < lim || !lim))
            lim = ct->AnonMemLimit;

    if (effective && !lim)
        lim = GetTotalMemory();

    return lim;
}

TError TContainer::ApplyUlimits() {
    auto cg = GetCgroup(FreezerSubsystem);
    std::map<int, struct rlimit> map;
    std::vector<pid_t> prev, pids;
    TError error;
    bool retry;

    L_ACT("Apply ulimits");
    auto lim = GetUlimit();
    do {
        error = cg.GetTasks(pids);
        if (error)
            return error;
        retry = false;
        for (auto pid: pids) {
            if (std::find(prev.begin(), prev.end(), pid) != prev.end())
                continue;
            error = lim.Apply(pid);
            if (error && error.Errno != ESRCH)
                return error;
            retry = true;
        }
        prev = pids;
    } while (retry);

    return OK;
}

void TContainer::ChooseSchedPolicy() {
    SchedPolicy = SCHED_OTHER;
    SchedPrio = 0;
    SchedNice = 0;

    if (CpuPolicy == "rt") {
        SchedNice = config().container().rt_nice();
        if (config().container().rt_priority()) {
            SchedPolicy = SCHED_RR;
            SchedPrio = config().container().rt_priority();
        }
    } else if (CpuPolicy == "high") {
        SchedNice = config().container().high_nice();
    } else if (CpuPolicy == "batch") {
        SchedPolicy = SCHED_BATCH;
    } else if (CpuPolicy == "idle") {
        SchedPolicy = SCHED_IDLE;
    } else if (CpuPolicy == "iso") {
        SchedPolicy = 4;
        SchedNice = config().container().high_nice();
    }

    if (SchedPolicy != SCHED_RR) {
        /* -1 nice is a +10% cpu weight */
        SchedNice -= std::log(CpuWeight) / std::log(1.1);
        SchedNice = std::min(std::max(SchedNice, -20), 19);
    }
}

TError TContainer::ApplySchedPolicy() const {
    auto cg = GetCgroup(FreezerSubsystem);
    struct sched_param param;
    param.sched_priority = SchedPrio;
    TError error;

    std::vector<pid_t> prev, pids;
    bool retry;

    L_ACT("Set {} scheduler policy {}", cg, CpuPolicy);
    do {
        error = cg.GetTasks(pids);
        retry = false;
        for (auto pid: pids) {
            if (std::find(prev.begin(), prev.end(), pid) != prev.end() &&
                    sched_getscheduler(pid) == SchedPolicy)
                continue;
            if (setpriority(PRIO_PROCESS, pid, SchedNice) && errno != ESRCH)
                return TError::System("setpriority");
            if (sched_setscheduler(pid, SchedPolicy, &param) &&
                    errno != ESRCH)
                return TError::System("sched_setscheduler");
            retry = true;
        }
        prev = pids;
    } while (retry);

    return OK;
}

TError TContainer::ApplyIoPolicy() const {
    auto cg = GetCgroup(FreezerSubsystem);
    TError error;

    std::vector<pid_t> prev, pids;
    bool retry;

    L_ACT("Set {} io policy {} ioprio {}", cg, IoPolicy, IoPrio);
    do {
        error = cg.GetTasks(pids);
        retry = false;
        for (auto pid: pids) {
            if (std::find(prev.begin(), prev.end(), pid) != prev.end())
                continue;
            if (SetIoPrio(pid, IoPrio) && errno != ESRCH)
                return TError::System("ioprio");
            retry = true;
        }
        prev = pids;
    } while (retry);

    return OK;
}

TError TContainer::ApplyResolvConf() const {
    TError error;
    TFile file;

    if (HasProp(EProperty::RESOLV_CONF) ? !ResolvConf.size() : Root == "/")
        return OK;

    if (!Task.Pid)
        return TError(EError::InvalidState, "No container task pid");

    error = file.Open("/proc/" + std::to_string(Task.Pid) + "/root/etc/resolv.conf",
                      O_WRONLY | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY);
    if (error)
        return error;

    if (file.FsType() != TMPFS_MAGIC)
        return TError(EError::NotSupported, "resolv.conf not on tmpfs");

    L_ACT("Apply resolv_conf for CT{}:{}", Id, Name);
    error = file.Truncate(0);
    if (!error)
        error = file.WriteAll(ResolvConf.size() ? ResolvConf : RootContainer->ResolvConf);
    return error;
}

TError TContainer::ReserveCpus(unsigned nr_threads, unsigned nr_cores,
                               TBitMap &threads, TBitMap &cores) {
    bool try_thread = true;

    threads.Clear();
    cores.Clear();

again:
    for (unsigned cpu = 0; cpu < CpuVacant.Size(); cpu++) {
        if (!CpuVacant.Get(cpu))
            continue;

        if (CoreThreads[cpu].IsSubsetOf(CpuVacant)) {
            if (nr_cores) {
                nr_cores--;
                cores.Set(cpu);
                threads.Set(CoreThreads[cpu]);
                CpuVacant.Set(CoreThreads[cpu], false);
            } else if (!try_thread) {
                nr_threads--;
                threads.Set(cpu);
                CpuVacant.Set(cpu, false);
                try_thread = true;
            }
        } else if (nr_threads) {
            nr_threads--;
            threads.Set(cpu);
            CpuVacant.Set(cpu, false);
        }

        if (!nr_threads && !nr_cores)
            break;
    }

    if (try_thread && nr_threads) {
        try_thread = false;
        goto again;
    }

    if (nr_threads || nr_cores || (IsRoot() && !CpuVacant.Weight())) {
        CpuVacant.Set(threads);
        threads.Clear();
        cores.Clear();
        return TError(EError::ResourceNotAvailable, "Not enough cpus in CT{}:{}", Id, Name);
    }

    return OK;
}

TError TContainer::DistributeCpus() {
    auto lock = LockCpuAffinity();
    TError error;

    if (IsRoot()) {
        error = CpuAffinity.Read("/sys/devices/system/cpu/online");
        if (error)
            return error;

        CoreThreads.clear();
        CoreThreads.resize(CpuAffinity.Size());

        for (unsigned cpu = 0; cpu < CpuAffinity.Size(); cpu++) {
            if (!CpuAffinity.Get(cpu))
                continue;
            error = CoreThreads[cpu].Read(StringFormat("/sys/devices/system/cpu/cpu%u/topology/thread_siblings_list", cpu));
            if (error)
                return error;
        }

        error = NumaNodes.Read("/sys/devices/system/node/online");
        if (error)
            return error;

        NodeThreads.clear();
        NodeThreads.resize(NumaNodes.Size());

        for (unsigned node = 0; node < NumaNodes.Size(); node++) {
            if (!NumaNodes.Get(node))
                continue;
            error = NodeThreads[node].Read(StringFormat("/sys/devices/system/node/node%u/cpulist", node));
            if (error)
                return error;
        }
    }

    CpuVacant.Clear();
    CpuVacant.Set(CpuAffinity);

    static ECpuSetType order[] = {
        ECpuSetType::Absolute,
        ECpuSetType::Node,
        ECpuSetType::Cores,
        ECpuSetType::Threads,
        ECpuSetType::Reserve,
        ECpuSetType::Inherit,
    };

    auto subtree = Subtree();
    subtree.reverse();

    for (auto &parent: subtree) {
        if (parent->State == EContainerState::Stopped ||
                parent->State == EContainerState::Dead)
            continue;

        auto childs = parent->Childs();
        if (childs.empty())
            continue;

        L_VERBOSE("Distribute CPUs {} in CT{}:{}", parent->CpuVacant.Format(), parent->Id, parent->Name);

        uint64_t vacantGuarantee = 0;

        for (auto type: order) {
            for (auto &ct: childs) {
                if (ct->CpuSetType != type ||
                        ct->State == EContainerState::Stopped ||
                        ct->State == EContainerState::Dead)
                    continue;

                ct->CpuVacant.Clear();
                ct->CpuReserve.Clear();

                TBitMap affinity;

                switch (type) {
                case ECpuSetType::Inherit:
                    affinity.Set(parent->CpuVacant);
                    break;
                case ECpuSetType::Absolute:
                    affinity.Set(ct->CpuAffinity);
                    break;
                case ECpuSetType::Node:
                    if (!NumaNodes.Get(ct->CpuSetArg))
                        return TError(EError::ResourceNotAvailable, "Numa node not found for CT{}:{}", ct->Id, ct->Name);
                    affinity.Set(NodeThreads[ct->CpuSetArg]);
                    break;
                case ECpuSetType::Cores:
                    error = parent->ReserveCpus(0, ct->CpuSetArg,
                                                ct->CpuReserve, affinity);
                    if (error)
                        return error;
                    break;
                case ECpuSetType::Threads:
                    error = parent->ReserveCpus(ct->CpuSetArg, 0,
                                                ct->CpuReserve, affinity);
                    if (error)
                        return error;
                    affinity.Set(ct->CpuReserve);
                    break;
                case ECpuSetType::Reserve:
                    error = parent->ReserveCpus(ct->CpuSetArg, 0,
                                                ct->CpuReserve, affinity);
                    if (error)
                        return error;
                    affinity.Set(parent->CpuAffinity);
                    break;
                }

                if (!affinity.Weight() || !affinity.IsSubsetOf(parent->CpuAffinity))
                    return TError(EError::ResourceNotAvailable, "Not enough cpus for CT{}:{}", ct->Id, ct->Name);

                if (!ct->CpuAffinity.IsEqual(affinity)) {
                    ct->CpuAffinity.Clear();
                    ct->CpuAffinity.Set(affinity);
                    ct->SetProp(EProperty::CPU_SET_AFFINITY);
                }

                if (ct->CpuReserve.Weight())
                    L_ACT("Reserve CPUs {} for CT{}:{}", ct->CpuReserve.Format(), ct->Id, ct->Name);
                else
                    vacantGuarantee += std::max(ct->CpuGuarantee, ct->CpuGuaranteeSum);

                L_VERBOSE("Assign CPUs {} for CT{}:{}", ct->CpuAffinity.Format(), ct->Id, ct->Name);

                ct->CpuVacant.Set(ct->CpuAffinity);
            }
        }

        if (vacantGuarantee > parent->CpuVacant.Weight() * CPU_POWER_PER_SEC) {
            if (!parent->CpuVacant.IsEqual(parent->CpuAffinity))
                return TError(EError::ResourceNotAvailable, "Not enough cpus for cpu_guarantee in CT{}:{}", parent->Id, Parent->Name);
            L("CPU guarantee overcommit in CT{}:{}", parent->Id, parent->Name);
        }
    }

    for (auto &ct: subtree) {
        if (ct.get() == this || !(ct->Controllers & CGROUP_CPUSET) ||
                !ct->TestPropDirty(EProperty::CPU_SET_AFFINITY) ||
                ct->State == EContainerState::Stopped ||
                ct->State == EContainerState::Dead)
            continue;

        auto cg = ct->GetCgroup(CpusetSubsystem);

        if (!cg.Exists())
            continue;

        error = CpusetSubsystem.SetCpus(cg, CpuAffinity.Format());
        if (error) {
            L("Cannot set cpu affinity: {}", error);
            return error;
        }
    }

    subtree.reverse();

    for (auto &ct: subtree) {
        if (ct.get() == this || !(ct->Controllers & CGROUP_CPUSET) ||
                !ct->TestClearPropDirty(EProperty::CPU_SET_AFFINITY) ||
                ct->State == EContainerState::Stopped ||
                ct->State == EContainerState::Dead)
            continue;

        auto cg = ct->GetCgroup(CpusetSubsystem);

        if (!cg.Exists())
            continue;

        error = CpusetSubsystem.SetCpus(cg, ct->CpuAffinity.Format());
        if (error) {
            L("Cannot set cpu affinity: {}", error);
            return error;
        }

        error = CpusetSubsystem.SetMems(cg, "");
        if (error) {
            L("Cannot set mem affinity: {}", error);
            return error;
        }
    }

    return OK;
}

TError TContainer::ApplyCpuGuarantee() {
    auto cpu_lock = LockCpuAffinity();
    TError error;

    if (config().container().propagate_cpu_guarantee()) {
        auto ct_lock = LockContainers();
        CpuGuaranteeSum = 0;
        for (auto child: Children) {
            if (child->State == EContainerState::Running ||
                    child->State == EContainerState::Meta ||
                    child->State == EContainerState::Starting)
                CpuGuaranteeSum += std::max(child->CpuGuarantee, child->CpuGuaranteeSum);
        }
        ct_lock.unlock();
    }

    auto cur = std::max(CpuGuarantee, CpuGuaranteeSum);
    if (!IsRoot() && (Controllers & CGROUP_CPU) && cur != CpuGuaranteeCur) {
        L_ACT("Set cpu guarantee CT{}:{} {} -> {}", Id, Name,
                CpuPowerToString(CpuGuaranteeCur), CpuPowerToString(cur));
        auto cpucg = GetCgroup(CpuSubsystem);
        error = CpuSubsystem.SetGuarantee(cpucg, CpuPolicy, CpuWeight, CpuPeriod, cur);
        if (error) {
            L_ERR("Cannot set cpu guarantee: {}", error);
            return error;
        }
        CpuGuaranteeCur = cur;
    }

    return OK;
}

void TContainer::PropagateCpuLimit() {
    uint64_t max = RootContainer->CpuLimit;
    auto ct_lock = LockContainers();

    for (auto ct = this; ct; ct = ct->Parent.get()) {
        uint64_t sum = 0;

        if (ct->State == EContainerState::Running ||
                (ct->State == EContainerState::Starting && !ct->IsMeta()))
            sum += ct->CpuLimit ?: max;

        for (auto &child: ct->Children) {
            if (child->State == EContainerState::Running ||
                    (child->State == EContainerState::Starting && !child->IsMeta()))
                sum += child->CpuLimit ?: max;
            else if (child->State == EContainerState::Meta)
                sum += std::min(child->CpuLimit ?: max, child->CpuLimitSum);
        }

        if (sum == ct->CpuLimitSum)
            break;

        L_DBG("Propagate total cpu limit CT{}:{} {} -> {}", ct->Id, ct->Name,
                CpuPowerToString(ct->CpuLimitSum), CpuPowerToString(sum));

        ct->CpuLimitSum = sum;
    }

    ct_lock.unlock();
}

TError TContainer::SetCpuLimit(uint64_t limit) {
    auto cpucg = GetCgroup(CpuSubsystem);
    TError error;

    L_ACT("Set cpu limit CT{}:{} {} -> {}", Id, Name,
            CpuPowerToString(CpuLimitCur), CpuPowerToString(limit));

    error = CpuSubsystem.SetRtLimit(cpucg, CpuPeriod, limit);
    if (error) {
        if (CpuPolicy == "rt")
            return error;
        L_WRN("Cannot set rt cpu limit: {}", error);
    }

    error = CpuSubsystem.SetLimit(cpucg, CpuPeriod, limit);
    if (error)
        return error;

    CpuLimitCur = limit;
    return OK;
}

TError TContainer::ApplyCpuLimit() {
    uint64_t limit = CpuLimit;
    TError error;

    for (auto *p = Parent.get(); p; p = p->Parent.get()) {
        if (p->CpuLimit && p->CpuLimit <= limit) {
            L_ACT("Disable cpu limit {} for CT{}:{} parent CT{}:{} has lower limit {}",
                 CpuPowerToString(limit), Id, Name,
                 p->Id, p->Name, CpuPowerToString(p->CpuLimit));
            limit = 0;
            break;
        }
    }

    auto subtree = Subtree();

    if (limit && (limit < CpuLimitCur || !CpuLimitCur)) {
        for (auto &ct: subtree)
            if (ct.get() != this && ct->State != EContainerState::Stopped &&
                    (ct->Controllers & CGROUP_CPU) && ct->CpuLimitCur > limit)
                ct->SetCpuLimit(limit);
    }

    error = SetCpuLimit(limit);
    if (error)
        return error;

    for (auto &ct: subtree) {
        if (ct.get() != this && ct->State != EContainerState::Stopped &&
                (ct->Controllers & CGROUP_CPU)) {
            uint64_t limit = ct->CpuLimit;
            for (auto *p = ct->Parent.get(); p && limit; p = p->Parent.get())
                if (p->CpuLimit && p->CpuLimit <= limit)
                    limit = 0;
            if (limit != ct->CpuLimitCur)
                ct->SetCpuLimit(limit);
        }
    }

    return OK;
}


TError TContainer::ApplyDynamicProperties() {
    auto memcg = GetCgroup(MemorySubsystem);
    auto blkcg = GetCgroup(BlkioSubsystem);
    TError error;

    if (TestClearPropDirty(EProperty::MEM_GUARANTEE)) {
        error = MemorySubsystem.SetGuarantee(memcg, MemGuarantee);
        if (error) {
            if (error.Errno != EINVAL)
                L_ERR("Cannot set {}: {}", P_MEM_GUARANTEE, error);
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::MEM_LIMIT)) {
        error = MemorySubsystem.SetLimit(memcg, MemLimit);
        if (error) {
            if (error.Errno == EBUSY)
                return TError(EError::Busy, "Memory limit is too low for current memory usage");
            if (error.Errno != EINVAL)
                L_ERR("Cannot set {}: {}", P_MEM_LIMIT, error);
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::ANON_LIMIT)) {
        error = MemorySubsystem.SetAnonLimit(memcg, AnonMemLimit);
        if (error) {
            if (error.Errno == EBUSY)
                return TError(EError::Busy, "Memory limit is too low for current memory usage");
            if (error.Errno != EINVAL)
                L_ERR("Cannot set {}: {}", P_ANON_LIMIT, error);
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::ANON_ONLY)) {
        error = MemorySubsystem.SetAnonOnly(memcg, AnonOnly);
        if (error) {
            if (error.Errno != EINVAL)
                L_ERR("Cannot set {}: {}", P_ANON_ONLY, error);
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::DIRTY_LIMIT)) {
        error = MemorySubsystem.SetDirtyLimit(memcg, DirtyMemLimit);
        if (error) {
            if (error.Errno != EINVAL)
                L_ERR("Can't set {}: {}", P_DIRTY_LIMIT, error);
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::RECHARGE_ON_PGFAULT)) {
        error = MemorySubsystem.RechargeOnPgfault(memcg, RechargeOnPgfault);
        if (error) {
            if (error.Errno != EINVAL)
                L_ERR("Cannot set {}: {}", P_RECHARGE_ON_PGFAULT, error);
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::PRESSURIZE_ON_DEATH)) {
        error = UpdateSoftLimit();
        if (error) {
            if (error.Errno != EINVAL)
                L_ERR("Can't set {}: {}", P_PRESSURIZE_ON_DEATH, error);
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::IO_LIMIT)) {
        if (IoBpsLimit.count("fs")) {
            error = MemorySubsystem.SetIoLimit(memcg, IoBpsLimit["fs"]);
            if (error) {
                if (error.Errno != EINVAL)
                    L_ERR("Can't set {}: {}", P_IO_LIMIT, error);
                return error;
            }
        }
        error = BlkioSubsystem.SetIoLimit(blkcg, RootPath, IoBpsLimit);
        if (error)
            return error;
    }

    if (TestClearPropDirty(EProperty::IO_OPS_LIMIT)) {
        if (IoOpsLimit.count("fs")) {
            error = MemorySubsystem.SetIopsLimit(memcg, IoOpsLimit["fs"]);
            if (error) {
                if (error.Errno != EINVAL)
                    L_ERR("Can't set {}: {}", P_IO_OPS_LIMIT, error);
                return error;
            }
        }
        error = BlkioSubsystem.SetIoLimit(blkcg, RootPath, IoOpsLimit, true);
        if (error)
            return error;
    }

    if (TestClearPropDirty(EProperty::IO_WEIGHT) |
            TestPropDirty(EProperty::IO_POLICY)) {
        if (Controllers & CGROUP_BLKIO) {
            error = BlkioSubsystem.SetIoWeight(blkcg, IoPolicy, IoWeight);
            if (error) {
                if (error.Errno != EINVAL)
                    L_ERR("Can't set {}: {}", P_IO_POLICY, error);
                return error;
            }
        }
    }

    if (TestClearPropDirty(EProperty::IO_POLICY)) {
        error = ApplyIoPolicy();
        if (error) {
            L_ERR("Cannot set io policy: {}", error);
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::HUGETLB_LIMIT)) {
        auto cg = GetCgroup(HugetlbSubsystem);
        error = HugetlbSubsystem.SetHugeLimit(cg, HugetlbLimit ?: -1);
        if (error) {
            if (error.Errno != EINVAL)
                L_ERR("Can't set {}: {}", P_HUGETLB_LIMIT, error);
            return error;
        }
        if (HugetlbSubsystem.SupportGigaPages()) {
            error = HugetlbSubsystem.SetGigaLimit(cg, 0);
            if (error)
                L_WRN("Cannot forbid 1GB pages: {}", error);
        }
    }

    if ((Controllers & CGROUP_CPU) &&
            (TestPropDirty(EProperty::CPU_PERIOD) |
             TestClearPropDirty(EProperty::CPU_GUARANTEE))) {
        for (auto ct = this; ct; ct = ct->Parent.get()) {
            error = ct->ApplyCpuGuarantee();
            if (error)
                return error;
            if (!config().container().propagate_cpu_guarantee())
                break;
        }
    }

    if (TestPropDirty(EProperty::CPU_LIMIT))
        PropagateCpuLimit();

    if ((Controllers & CGROUP_CPU) &&
            (TestPropDirty(EProperty::CPU_POLICY) |
             TestPropDirty(EProperty::CPU_WEIGHT) |
             TestClearPropDirty(EProperty::CPU_LIMIT) |
             TestClearPropDirty(EProperty::CPU_PERIOD))) {
        error = ApplyCpuLimit();
        if (error)
            return error;
    }

    if (TestClearPropDirty(EProperty::CPU_POLICY) ||
        TestClearPropDirty(EProperty::CPU_WEIGHT)) {
        error = ApplySchedPolicy();
        if (error) {
            L_ERR("Cannot set scheduler policy: {}", error);
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::CPU_SET) && Parent) {
        error = Parent->DistributeCpus();
        if (error)
            return error;
    }

    if (TestClearPropDirty(EProperty::NET_LIMIT) |
            TestClearPropDirty(EProperty::NET_GUARANTEE) |
            TestClearPropDirty(EProperty::NET_RX_LIMIT)) {
        if (Net) {
            error = Net->SetupClasses(NetClass);
            if (error)
                return error;
        }
    }


    if ((Controllers & CGROUP_NETCLS) &&
            TestClearPropDirty(EProperty::NET_TOS)) {
        auto netcls = GetCgroup(NetclsSubsystem);
        error = NetclsSubsystem.SetClass(netcls, NetClass.LeafHandle | NetClass.DefaultTos);
        if (error) {
            L_ERR("Can't set classid: {}", error);
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::ULIMIT)) {
        for (auto &ct: Subtree()) {
            if (ct->State == EContainerState::Stopped ||
                    ct->State == EContainerState::Dead)
                continue;
            error = ct->ApplyUlimits();
            if (error) {
                L_ERR("Cannot update ulimit: {}", error);
                return error;
            }
        }
    }

    if (TestClearPropDirty(EProperty::THREAD_LIMIT)) {
        auto cg = GetCgroup(PidsSubsystem);
        error = PidsSubsystem.SetLimit(cg, ThreadLimit);
        if (error) {
            L_ERR("Cannot set thread limit: {}", error);
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::RESOLV_CONF)) {
        error = ApplyResolvConf();
        if (error) {
            L_ERR("Cannot change /etc/resolv.conf contents: {}", error);
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::DEVICE_CONF)) {
        error = ApplyDeviceConf();
        if (error) {
            if (error != EError::Permission && error != EError::DeviceNotFound)
                L_WRN("Cannot change allowed devices: {}", error);
            return error;
        }
    }

    return OK;
}

void TContainer::ShutdownOom() {
    if (Source)
        EpollLoop->RemoveSource(Source->Fd);
    Source = nullptr;
    PORTO_ASSERT(OomEvent.Fd < 0 || OomEvent.Fd > 2);
    OomEvent.Close();
}

TError TContainer::PrepareOomMonitor() {

    if (IsRoot() || !(Controllers & CGROUP_MEMORY))
        return OK;

    TCgroup memoryCg = GetCgroup(MemorySubsystem);
    TError error;

    error = MemorySubsystem.SetupOOMEvent(memoryCg, OomEvent);
    if (error)
        return error;

    Source = std::make_shared<TEpollSource>(OomEvent.Fd, EPOLL_EVENT_OOM, shared_from_this());
    error = EpollLoop->AddSource(Source);
    if (error)
        ShutdownOom();

    return error;
}

TError TContainer::ApplyDeviceConf() const {
    TError error;

    if (IsRoot())
        return OK;

    if (Controllers & CGROUP_DEVICES) {
        TCgroup cg = GetCgroup(DevicesSubsystem);
        error = Devices.Apply(cg);
        if (error)
            return error;
    }

    if (State != EContainerState::Starting && Task.Pid && !RootPath.IsRoot()) {
        error = Devices.Makedev(fmt::format("/proc/{}/root", Task.Pid));
        if (error)
            return error;
    }

    return OK;
}

TError TContainer::SetSymlink(const TPath &symlink, const TPath &target) {
    TError error;

    if (WaitTask.Pid) {
        TMountNamespace mnt;
        mnt.Cwd = GetCwd();
        mnt.Root = Root;
        mnt.BindCred = TaskCred;

        error = mnt.Enter(Task.Pid);
        if (error)
            return error;

        error = mnt.CreateSymlink(symlink, target);

        TError error2 = mnt.Leave();
        PORTO_ASSERT(!error2);
    }

    if (!error) {
        if (target)
            Symlink[symlink] = target;
        else
            Symlink.erase(symlink);
        SetProp(EProperty::SYMLINK);
    }

    return error;
}

TError TContainer::PrepareCgroups() {
    TError error;

    if (JobMode) {
        Controllers = 0;
        if (RequiredControllers)
            return TError(EError::InvalidValue, "Cannot use cgroups in virt_mode=job");
        return OK;
    } else {
        Controllers |= CGROUP_FREEZER;
        RequiredControllers |= CGROUP_FREEZER;
    }

    if (!HasProp(EProperty::CPU_SET) && Parent) {
        auto lock = LockCpuAffinity();

        /* Create CPU set if some CPUs in parent are reserved */
        if (!Parent->CpuAffinity.IsEqual(Parent->CpuVacant)) {
            Controllers |= CGROUP_CPUSET;
            RequiredControllers |= CGROUP_CPUSET;
            L("Enable cpuset for CT{}:{} because parent has reserved cpus", Id, Name);
        } else {
            CpuAffinity.Clear();
            CpuAffinity.Set(Parent->CpuAffinity);
            CpuVacant.Clear();
            CpuVacant.Set(Parent->CpuAffinity);
        }
    }

    if (Controllers & CGROUP_CPUSET) {
        SetProp(EProperty::CPU_SET);
        SetProp(EProperty::CPU_SET_AFFINITY);
    }

    if (OsMode && Isolate && config().container().detect_systemd() &&
            SystemdSubsystem.Supported &&
            !(Controllers & CGROUP_SYSTEMD) &&
            !RootPath.IsRoot()) {
        TPath cmd = RootPath / Command;
        TPath dst;
        if (!cmd.ReadLink(dst) && dst.BaseName() == "systemd") {
            L("Enable systemd cgroup for CT{}:{}", Id, Name);
            Controllers |= CGROUP_SYSTEMD;
        }
    }

    auto missing = Controllers | RequiredControllers;

    for (auto hy: Hierarchies) {
        TCgroup cg = GetCgroup(*hy);

        if (!(Controllers & hy->Controllers))
            continue;

        if ((Controllers & hy->Controllers) != hy->Controllers) {
            Controllers |= hy->Controllers;
            SetProp(EProperty::CONTROLLERS);
        }

        missing &= ~hy->Controllers;

        if (cg.Exists())
            continue;

        error = cg.Create();
        if (error)
            return error;
    }

    if (missing) {
        std::string types;
        for (auto subsys: Subsystems)
            if (subsys->Kind & missing)
                types += " " + subsys->Type;
        return TError(EError::NotSupported, "Some cgroup controllers are not available:" + types);
    }

    if (!IsRoot() && (Controllers & CGROUP_MEMORY)) {
        error = GetCgroup(MemorySubsystem).SetBool(MemorySubsystem.USE_HIERARCHY, true);
        if (error)
            return error;
    }

    if (Controllers & CGROUP_DEVICES) {
        TCgroup devcg = GetCgroup(DevicesSubsystem);
        /* Nested cgroup makes a copy from parent at creation */
        if ((Level == 1 || TPath(devcg.Name).IsSimple()) && !HostMode) {
            /* at restore child cgroups blocks reset */
            error = RootContainer->Devices.Apply(devcg, State == EContainerState::Starting);
            if (error)
                return error;
        }
    }

    if (Controllers & CGROUP_NETCLS) {
        auto netcls = GetCgroup(NetclsSubsystem);
        error = NetclsSubsystem.SetClass(netcls, NetClass.LeafHandle | NetClass.DefaultTos);
        if (error)
            return error;
    }

    return OK;
}

TError TContainer::GetEnvironment(TEnv &env) const {
    env.ClearEnv();

    env.SetEnv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
    env.SetEnv("HOME", GetCwd().ToString());
    env.SetEnv("USER", TaskCred.User());

    env.SetEnv("container", "lxc");

    /* lock these */
    env.SetEnv("PORTO_NAME", Name, true, true);
    env.SetEnv("PORTO_HOST", GetHostName(), true, true);
    env.SetEnv("PORTO_USER", OwnerCred.User(), true, true);

    for (auto &extra: config().container().extra_env())
        env.SetEnv(extra.name(), extra.value(), true, false);

    /* Inherit environment from containts in isolation domain */
    bool overwrite = true;
    for (auto ct = this; ct; ct = ct->Parent.get()) {
        TError error = env.Parse(ct->EnvCfg, overwrite);
        if (error && overwrite)
            return error;
        overwrite = false;

        if (ct->Isolate)
            break;
    }

    return OK;
}

TError TContainer::ResolvePlace(TPath &place) const {
    TError error;

    if (StringStartsWith(place.ToString(), "///"))
        place = RootPath / place.ToString().substr(2);

    if (place.IsAbsolute()) {
        for (auto &policy: PlacePolicy) {
            if (policy[0] == '/' || policy == "***") {
                if (StringMatch(place.ToString(), policy))
                    goto found;
            } else {
                auto sep = policy.find('=');
                if (sep != std::string::npos &&
                        StringMatch(place.ToString(), policy.substr(sep + 1)))
                    goto found;
            }
        }
    } else {
        auto prefix = place.ToString() + "=";
        for (const auto &policy: PlacePolicy) {
            if (!place) {
                place = policy;
                goto found;
            }
            if (StringStartsWith(policy, prefix)) {
                place = policy.substr(prefix.size());
                goto found;
            }
        }
    }

    return TError(EError::Permission, "Place {} is not permitted", place);

found:
    if (!place.IsNormal())
        return TError(EError::InvalidPath, "Place path {} must be normalized", place);

    if (IsSystemPath(place))
        return TError(EError::InvalidPath, "Place path {} in system directory", place);

    return OK;
}

TError TContainer::PrepareTask(TTaskEnv &TaskEnv) {
    TError error;

    TaskEnv.CT = shared_from_this();
    TaskEnv.Client = CL;

    for (auto hy: Hierarchies)
        TaskEnv.Cgroups.push_back(GetCgroup(*hy));

    TaskEnv.Mnt.Cwd = GetCwd();

    TaskEnv.Mnt.Root = Root;
    TaskEnv.Mnt.HostRoot = RootPath;

    TaskEnv.Mnt.RootRo = RootRo;

    TaskEnv.Mnt.RunSize = GetMemLimit() / 2;

    TaskEnv.Mnt.BindCred = Parent->RootPath.IsRoot() ? CL->TaskCred : TCred(RootUser, RootGroup);

    if (OsMode && Isolate && (Controllers & CGROUP_SYSTEMD))
        TaskEnv.Mnt.Systemd = GetCgroup(SystemdSubsystem).Name;

    TaskEnv.Cred = TaskCred;

    TaskEnv.LoginUid = OsMode ? -1 : OwnerCred.Uid;

    error = GetEnvironment(TaskEnv.Env);
    if (error)
        return error;

    /* one more fork for creating nested pid-namespace */
    TaskEnv.TripleFork = Isolate && TaskEnv.PidFd.GetFd() >= 0 &&
        TaskEnv.PidFd.Inode() != TNamespaceFd::PidInode(getpid(), "ns/pid");

    TaskEnv.QuadroFork = !JobMode && !OsMode && !IsMeta();

    TaskEnv.Mnt.BindMounts = BindMounts;
    TaskEnv.Mnt.Symlink = Symlink;

    /* legacy kludge */
    if (BindDns && !TaskEnv.Mnt.Root.IsRoot()) {
        TBindMount bm;
        bm.Source = "/etc/hosts";
        bm.Target = "/etc/hosts";
        bm.MntFlags |= MS_RDONLY;
        TaskEnv.Mnt.BindMounts.push_back(bm);
    }

    /* Resolve paths in parent namespace and check volume ownership */
    for (auto &bm: TaskEnv.Mnt.BindMounts) {
        if (!bm.Source.IsAbsolute())
            bm.Source = Parent->GetCwd() / bm.Source;

        auto src = TVolume::ResolveOrigin(Parent->RootPath / bm.Source);
        bm.ControlSource = src && !CL->CanControl(src->Volume->VolumeOwner);

        if (!bm.Target.IsAbsolute())
            bm.Target = TaskEnv.Mnt.Cwd / bm.Target;

        auto dst = TVolume::ResolveOrigin(Parent->RootPath / TaskEnv.Mnt.Root / bm.Target);
        bm.ControlTarget = dst && !CL->CanControl(dst->Volume->VolumeOwner);

        /* allow suid inside by default */
        bm.MntFlags |= MS_ALLOW_SUID;

        /* this allows to inject suid binaries into host */
        if (Parent->RootPath.IsRoot() && !TaskEnv.Mnt.Root.IsRoot() &&
                bm.Source.IsDirectoryFollow()) {
            TStatFS stat;
            if (!bm.Source.StatFS(stat) && (stat.MntFlags & MS_ALLOW_SUID)) {
                L("Bindmount source {} allows suid in host", bm.Source);
                TaintFlags.BindWithSuid = true;
            }
        }

        if (bm.MntFlags & MS_ALLOW_DEV) {
            if (!OwnerCred.IsRootUser())
                return TError(EError::Permission, "Not enough permissions to allow devices at bind mount");
        } else
            bm.MntFlags |= MS_NODEV;
    }

    TaskEnv.Mnt.BindPortoSock = AccessLevel != EAccessLevel::None;

    if (IsMeta() || TaskEnv.TripleFork || TaskEnv.QuadroFork) {
        TPath path = TPath(PORTO_HELPERS_PATH) / "portoinit";
        TError error = TaskEnv.PortoInit.OpenRead(path);
        if (error) {
            TPath exe("/proc/self/exe");
            error = exe.ReadLink(path);
            if (error)
                return error;
            path = path.DirName() / "portoinit";
            error = TaskEnv.PortoInit.OpenRead(path);
            if (error)
                return error;
        }
    }

    TaskEnv.Mnt.IsolateRun = TaskEnv.Mnt.Root.IsRoot() && OsMode && Isolate;

    // Create new mount namespaces if we have to make any changes
    TaskEnv.NewMountNs = Isolate ||
                          TaskEnv.Mnt.IsolateRun ||
                          (Level == 1 && !HostMode) ||
                          TaskEnv.Mnt.BindMounts.size() ||
                          Hostname.size() ||
                          ResolvConf.size() ||
                          EtcHosts.size() ||
                          !TaskEnv.Mnt.Root.IsRoot() ||
                          TaskEnv.Mnt.RootRo ||
                          !TaskEnv.Mnt.Systemd.empty();

    if (TaskEnv.NewMountNs && (HostMode || JobMode))
        return TError(EError::InvalidValue, "Cannot change mount-namespace in this virt_mode");

    return OK;
}

void TContainer::SanitizeCapabilities() {
    if (OwnerCred.IsRootUser()) {
        if (HasProp(EProperty::CAPABILITIES))
            CapBound = CapLimit;
        else if (HostMode)
            CapBound = AllCapabilities;
        else
            CapBound = HostCapBound;
        CapAllowed = CapBound;
    } else {
        bool chroot = false;
        bool pidns = false;
        bool memcg = false;
        bool netns = false;
        bool netip = false;

        if (HostMode)
            CapBound = AllCapabilities;
        else
            CapBound = HostCapBound;

        for (auto ct = this; ct; ct = ct->Parent.get()) {
            chroot |= ct->Root != "/";
            pidns |= ct->Isolate;
            memcg |= ct->MemLimit && ct->HasProp(EProperty::MEM_LIMIT);
            netns |= ct->NetIsolate;
            netip |= ct->IpPolicy != "any";

            if (ct->HasProp(EProperty::CAPABILITIES))
                CapBound.Permitted &= ct->CapLimit.Permitted;
        }

        TCapabilities remove;
        if (!pidns)
            remove.Permitted |= PidNsCapabilities.Permitted;
        if (!memcg)
            remove.Permitted |= MemCgCapabilities.Permitted;
        if (!netns || netip)
            remove.Permitted |= NetNsCapabilities.Permitted;

        if (chroot) {
            CapBound.Permitted &= ChrootCapBound.Permitted & ~remove.Permitted;
            CapAllowed = CapBound;
        } else if (HostMode) {
            CapAllowed.Permitted = CapBound.Permitted & ~remove.Permitted;
        } else {
            CapAllowed.Permitted = HostCapAllowed.Permitted &
                                   CapBound.Permitted & ~remove.Permitted;
        }
    }

    if (!HasProp(EProperty::CAPABILITIES))
        CapLimit.Permitted = CapBound.Permitted;
}

void TContainer::SanitizeCapabilitiesAll() {
    for (auto &ct: Subtree())
        ct->SanitizeCapabilities();
}

TUlimit TContainer::GetUlimit() const {
    TUlimit res = Ulimit;

    for (auto p = Parent.get(); p; p = p->Parent.get())
        res.Merge(p->Ulimit, false);

    uint64_t memlock = 0;
    if (config().container().memlock_margin()) {
        memlock = GetMemLimit(false); /* limit must be set explicitly */
        memlock -= std::min(memlock, config().container().memlock_margin());
    }
    if (memlock < config().container().memlock_minimal())
        memlock = config().container().memlock_minimal();
    if (memlock)
        res.Set(RLIMIT_MEMLOCK, memlock, RLIM_INFINITY, false);

    return res;
}

TError TContainer::StartTask() {
    TTaskEnv TaskEnv;
    TError error;

    error = TNetwork::StartNetwork(*this, TaskEnv);
    if (error)
        return error;

    if (IsRoot())
        return OK;

    /* After restart apply all set dynamic properties */
    memcpy(PropDirty, PropSet, sizeof(PropDirty));

    /* Applied by starting task */
    TestClearPropDirty(EProperty::RESOLV_CONF);

    error = ApplyDynamicProperties();
    if (error)
        return error;

    error = TaskEnv.OpenNamespaces(*this);
    if (error)
        return error;

    error = PrepareTask(TaskEnv);
    if (error)
        return error;

    /* Meta container without namespaces don't need task */
    if (IsMeta() && !Isolate && NetInherit && !TaskEnv.NewMountNs)
        return OK;

    error = TaskEnv.Start();

    /* Always report OOM stuation if any */
    if (error && RecvOomEvents())
        error = TError(EError::ResourceNotAvailable, "OOM at container {} start: {}", Name, error);

    return error;
}

TError TContainer::StartParents() {
    TError error;

    if (ActionLocked >= 0 || CL->LockedContainer.get() != this) {
        L_ERR("Container is not locked");
        return TError(EError::Busy, "Caontiner is not locked");
    }

    if (!Parent)
        return OK;

    auto cg = Parent->GetCgroup(FreezerSubsystem);
    if (FreezerSubsystem.IsFrozen(cg))
        return TError(EError::InvalidState, "Parent container is frozen");

    if (Parent->State == EContainerState::Running ||
            Parent->State == EContainerState::Meta)
        return OK;

    auto current = CL->LockedContainer;

    std::shared_ptr<TContainer> target;
    do {
        target = Parent;
        while (target->Parent && target->Parent->State != EContainerState::Running &&
                target->Parent->State != EContainerState::Meta)
            target = target->Parent;

        CL->ReleaseContainer();

        error = CL->LockContainer(target);
        if (error)
            return error;

        error = target->Start();
        if (error)
            return error;
    } while (target != Parent);

    CL->ReleaseContainer();

    return CL->LockContainer(current);
}

TError TContainer::PrepareStart() {
    TError error;

    error = CL->CanControl(OwnerCred);
    if (error)
        return error;

    if (Parent) {
        CT = this;
        LockStateWrite();
        for (auto &knob: ContainerProperties) {
            error = knob.second->Start();
            if (error)
                break;
        }
        UnlockState();
        CT = nullptr;
        if (error)
            return error;
    }

    if (HasProp(EProperty::ROOT) && RootPath.IsRegularFollow()) {
        std::shared_ptr<TVolume> vol;
        rpc::TVolumeSpec spec;

        L("Emulate deprecated loop root={} for CT{}:{}", RootPath, Id, Name);
        TaintFlags.RootOnLoop = true;

        spec.set_backend("loop");
        spec.set_storage(RootPath.ToString());
        spec.set_read_only(RootRo);
        spec.add_links()->set_container(ROOT_PORTO_NAMESPACE + Name);

        auto current = CL->LockedContainer;
        CL->ReleaseContainer();

        error = TVolume::Create(spec, vol);
        if (error) {
            L_ERR("Cannot create root volume: {}", error);
            return error;
        }

        error = CL->LockContainer(current);
        if (error)
            return error;

        L("Replace root={} with volume {}", Root, vol->Path);
        LockStateWrite();
        RootPath = vol->Path;
        Root = vol->Path.ToString();
        UnlockState();
    }

    (void)TaskCred.InitGroups(TaskCred.User());

    SanitizeCapabilities();

    if (TaintFlags.TaintCounted && Statistics->ContainersTainted)
        Statistics->ContainersTainted--;

    memset(&TaintFlags, 0, sizeof(TaintFlags));

    /* Check target task credentials */
    error = CL->CanControl(TaskCred);
    if (!error && !OwnerCred.IsMemberOf(TaskCred.Gid) && !CL->IsSuperUser()) {
        TCred cred;
        cred.Init(TaskCred.User());
        if (!cred.IsMemberOf(TaskCred.Gid))
            error = TError(EError::Permission, "Cannot control group " + TaskCred.Group());
    }

    /*
     * Allow any user:group in chroot
     * FIXME: non-racy chroot validation is impossible for now
     */
    if (error && !RootPath.IsRoot())
        error = OK;

    /* Allow any user:group in sub-container if client can change uid/gid */
    if (error && CL->CanSetUidGid() && IsChildOf(*CL->ClientContainer))
        error = OK;

    if (error)
        return error;

    /* Even without capabilities user=root require chroot */
    if (RootPath.IsRoot() && TaskCred.IsRootUser() && !OwnerCred.IsRootUser())
        return TError(EError::Permission, "user=root requires chroot");

    if (HostMode && Level) {
        if (!Parent->HostMode)
            return TError(EError::Permission, "For virt_mode=host parent must be in the same mode");
        if (!CL->ClientContainer->HostMode)
            return TError(EError::Permission, "For virt_mode=host client must be in the same mode");
    }

    if (JobMode && Level == 1)
        return TError(EError::InvalidValue, "Container in virt_mode=job without parent");

    if (Parent && Parent->JobMode)
        return TError(EError::InvalidValue, "Parent container in virt_mode=job");

    if (CapLimit.Permitted & ~CapBound.Permitted) {
        TCapabilities cap = CapLimit;
        cap.Permitted &= ~CapBound.Permitted;
        return TError(EError::Permission, "Capabilities out of bounds: " + cap.Format());
    }

    if (CapAmbient.Permitted & ~CapAllowed.Permitted) {
        TCapabilities cap = CapAmbient;
        cap.Permitted &= ~CapAllowed.Permitted;
        return TError(EError::Permission, "Ambient capabilities out of bounds: " + cap.Format());
    }

    if (!Parent) {
        /* Root container */
    } else if (!HasProp(EProperty::PLACE)) {
        /* Inherit parent policy */
        PlacePolicy = Parent->PlacePolicy;
    } else {
        /* Enforce place restictions */
        for (const auto &policy: PlacePolicy) {
            TPath place = policy;
            if (!place.IsAbsolute()) {
                if (policy == "***" &&
                        std::find(Parent->PlacePolicy.begin(),
                            Parent->PlacePolicy.end(),
                            policy) == Parent->PlacePolicy.end())
                    return TError(EError::Permission, "Place {} is not allowed by parent container", policy);
                auto sep = policy.find('=');
                if (sep != std::string::npos && policy[sep + 1] == '/')
                    place = policy.substr(sep + 1);
            }
            if (Parent->ResolvePlace(place))
                return TError(EError::Permission, "Place {} is not allowed by parent container", policy);
        }
    }

    return OK;
}

TError TContainer::Start() {
    TError error;

    if (State != EContainerState::Stopped)
        return TError(EError::InvalidState, "Cannot start container {} in state {}", Name, StateName(State));

    error = StartParents();
    if (error)
        return error;

    StartError = OK;

    error = PrepareStart();
    if (error) {
        error = TError(error, "Cannot prepare start for container {}", Name);
        goto err_prepare;
    }

    L_ACT("Start CT{}:{}", Id, Name);

    SetState(EContainerState::Starting);

    StartTime = GetCurrentTimeMs();
    RealStartTime = time(nullptr);
    SetProp(EProperty::START_TIME);

    error = PrepareResources();
    if (error)
        goto err_prepare;

    error = PrepareRuntimeResources();
    if (error)
        goto err;

    /* Complain about insecure misconfiguration */
    for (auto &taint: Taint()) {
        L_TAINT(taint);
        if (!TaintFlags.TaintCounted) {
            TaintFlags.TaintCounted = true;
            Statistics->ContainersTainted++;
        }
    }

    DowngradeActionLock();

    error = StartTask();

    UpgradeActionLock();

    if (error) {
        SetState(EContainerState::Stopping);
        (void)Terminate(0);
        goto err;
    }

    if (IsMeta())
        SetState(EContainerState::Meta);
    else
        SetState(EContainerState::Running);

    SetProp(EProperty::ROOT_PID);

    error = Save();
    if (error) {
        L_ERR("Cannot save state after start {}", error);
        (void)Reap(false);
        goto err;
    }

    Statistics->ContainersStarted++;

    return OK;

err:
    TNetwork::StopNetwork(*this);
    FreeRuntimeResources();
    FreeResources();
err_prepare:
    StartError = error;
    SetState(EContainerState::Stopped);
    Statistics->ContainersFailedStart++;

    return error;
}

TError TContainer::PrepareResources() {
    TError error;

    if (IsRoot()) {
        error = DistributeCpus();
        if (error)
            return error;
    }

    error = CheckMemGuarantee();
    if (error) {
        Statistics->FailMemoryGuarantee++;
        return error;
    }

    error = CreateWorkDir();
    if (error)
        return error;

    TNetwork::InitClass(*this);

    error = PrepareCgroups();
    if (error) {
        L_ERR("Can't prepare task cgroups: {}", error);
        goto undo;
    }

    if (RequiredVolumes.size()) {
        error = TVolume::CheckRequired(*this);
        if (error)
            goto undo;
    }

    return OK;

undo:
    FreeResources();
    return error;
}

/* Some resources are not required in dead state */

TError TContainer::PrepareRuntimeResources() {
    TError error;

    error = PrepareOomMonitor();
    if (error) {
        L_ERR("Cannot prepare OOM monitor: {}", error);
        return error;
    }

    error = UpdateSoftLimit();
    if (error) {
        L_ERR("Cannot update memory soft limit: {}", error);
        return error;
    }

    PropagateCpuLimit();

    return OK;
}

void TContainer::FreeRuntimeResources() {
    TError error;

    CollectOomKills();
    ShutdownOom();

    error = UpdateSoftLimit();
    if (error)
        L_ERR("Cannot update memory soft limit: {}", error);

    if (Parent && CpuReserve.Weight()) {
        L_ACT("Release CPUs reserved for CT{}:{}", Id, Name);
        error = Parent->DistributeCpus();
        if (error)
            L_ERR("Cannot redistribute CPUs: {}", error);
    }

    PropagateCpuLimit();

    if (CpuGuarantee && config().container().propagate_cpu_guarantee()) {
        for (auto p = Parent; p; p = p->Parent)
            (void)p->ApplyCpuGuarantee();
    }
}

void TContainer::FreeResources() {
    TError error;

    if (IsRoot())
        return;

    OomKills = 0;
    OomKillsRaw = 0;
    ClearProp(EProperty::OOM_KILLS);

    for (auto hy: Hierarchies) {
        if (Controllers & hy->Controllers) {
            auto cg = GetCgroup(*hy);
            (void)cg.Remove(); //Logged inside
        }
    }

    RemoveWorkDir();

    Stdout.Remove(*this);
    Stderr.Remove(*this);
}

TError TContainer::Kill(int sig) {
    if (State != EContainerState::Running)
        return TError(EError::InvalidState, "invalid container state ");

    L_ACT("Kill task {} in CT{}:{}", Task.Pid, Id, Name);
    return Task.Kill(sig);
}

TError TContainer::Terminate(uint64_t deadline) {
    auto cg = GetCgroup(FreezerSubsystem);
    TError error;

    if (IsRoot())
        return TError(EError::Permission, "Cannot terminate root container");

    L_ACT("Terminate tasks in CT{}:{}", Id, Name);

    if (!(Controllers & CGROUP_FREEZER) && !JobMode) {
        if (Task.Pid)
            return TError(EError::NotSupported, "Cannot terminate without freezer");
        return OK;
    }

    if (cg.IsEmpty())
        return OK;

    if (FreezerSubsystem.IsFrozen(cg) && !JobMode)
        return cg.KillAll(SIGKILL);

    if (Task.Pid && deadline && !IsMeta()) {
        int sig = SIGTERM;

        if (OsMode && Isolate) {
            uint64_t mask = TaskHandledSignals(Task.Pid);
            if (mask & BIT(SIGPWR - 1))
                sig = SIGPWR;
            else if (!(mask & BIT(SIGTERM - 1)))
                sig = 0;
        }

        if (sig) {
            if (JobMode)
                error = Task.KillPg(sig);
            else
                error = Task.Kill(sig);
            if (!error) {
                L_ACT("Wait task {} after signal {} in CT{}:{}", Task.Pid, sig, Id, Name);
                while (Task.Exists() && !Task.IsZombie() &&
                        !WaitDeadline(deadline));
            }
        }
    }

    if (JobMode)
        return WaitTask.KillPg(SIGKILL);

    if (WaitTask.Pid && Isolate) {
        error = WaitTask.Kill(SIGKILL);
        if (error)
            return error;
    }

    if (cg.IsEmpty())
        return OK;

    error = cg.KillAll(SIGKILL);
    if (error)
        return error;

    return OK;
}

void TContainer::ForgetPid() {
    Task.Pid = 0;
    TaskVPid = 0;
    WaitTask.Pid = 0;
    ClearProp(EProperty::ROOT_PID);
    SeizeTask.Pid = 0;
    ClearProp(EProperty::SEIZE_PID);
}

TError TContainer::Stop(uint64_t timeout) {
    uint64_t deadline = timeout ? GetCurrentTimeMs() + timeout : 0;
    auto freezer = GetCgroup(FreezerSubsystem);
    TError error;

    if (State == EContainerState::Stopped)
        return OK;

    if (!(Controllers & CGROUP_FREEZER) && !JobMode) {
        if (Task.Pid)
            return TError(EError::NotSupported, "Cannot stop without freezer");
    } else if (FreezerSubsystem.IsParentFreezing(freezer))
            return TError(EError::InvalidState, "Parent container is paused");

    auto subtree = Subtree();

    /* Downgrade explusive lock if we are going to wait. */
    if (timeout)
        CL->LockedContainer->DowngradeActionLock();

    if (!timeout) {
        L_ACT("Killing spree");
        for (auto it = subtree.rbegin(); it != subtree.rend(); ++it)
            if ((*it)->Isolate && (*it)->WaitTask.Pid)
                (void)(*it)->WaitTask.Kill(SIGKILL);
    }

    for (auto &ct : subtree) {
        auto cg = ct->GetCgroup(FreezerSubsystem);

        if (ct->IsRoot() || ct->State == EContainerState::Stopped)
            continue;

        ct->SetState(EContainerState::Stopping);
        error = ct->Terminate(deadline);
        if (error)
            L_ERR("Cannot terminate tasks in CT{}:{}: {}", ct->Id, ct->Name, error);

        if (FreezerSubsystem.IsSelfFreezing(cg) && !JobMode) {
            L_ACT("Thaw terminated paused CT{}:{}", ct->Id, ct->Name);
            error = FreezerSubsystem.Thaw(cg, false);
            if (error)
                L_ERR("Cannot thaw CT{}:{}: {}", ct->Id, ct->Name, error);
        }
    }

    if (timeout)
        CL->LockedContainer->UpgradeActionLock();

    for (auto &ct: subtree) {
        if (ct->State == EContainerState::Stopped)
            continue;

        L_ACT("Stop CT{}:{}", Id, Name);

        ct->LockStateWrite();

        ct->ForgetPid();

        ct->StartTime = 0;
        ct->RealStartTime = 0;
        ct->ClearProp(EProperty::START_TIME);

        ct->DeathTime = 0;
        ct->RealDeathTime = 0;
        ct->ClearProp(EProperty::DEATH_TIME);

        ct->ExitStatus = 0;
        ct->ClearProp(EProperty::EXIT_STATUS);

        ct->OomEvents = 0;
        ct->OomKilled = false;
        ct->ClearProp(EProperty::OOM_KILLED);

        ct->UnlockState();

        (void)ct->Save();

        TNetwork::StopNetwork(*ct);
        ct->FreeRuntimeResources();
        ct->FreeResources();

        ct->SetState(EContainerState::Stopped);

        error = ct->Save();
        if (error)
            return error;
    }

    return OK;
}

void TContainer::Reap(bool oomKilled) {
    TError error;

    error = Terminate(0);
    if (error)
        L_WRN("Cannot terminate CT{}:{} : {}", Id, Name, error);

    LockStateWrite();

    if (!HasProp(EProperty::DEATH_TIME)) {
        DeathTime = GetCurrentTimeMs();
        RealDeathTime = time(nullptr);
        SetProp(EProperty::DEATH_TIME);
    }

    if (oomKilled) {
        OomKilled = oomKilled;
        SetProp(EProperty::OOM_KILLED);
    }

    ForgetPid();

    UnlockState();

    Stdout.Rotate(*this);
    Stderr.Rotate(*this);

    SetState(EContainerState::Dead);

    FreeRuntimeResources();

    error = Save();
    if (error)
        L_WRN("Cannot save container state after exit: {}", error);
}

void TContainer::Exit(int status, bool oomKilled) {

    if (State == EContainerState::Stopped)
        return;

    /*
     * SIGKILL could be delivered earlier than OOM event.
     * Any non-zero exit code or signal might be casuesd by OOM.
     */
    if (RecvOomEvents() && status)
        oomKilled = true;

    /* Detect fatal signals: portoinit cannot kill itself */
    if (WaitTask.Pid != Task.Pid && WIFEXITED(status) &&
            WEXITSTATUS(status) > 128 &&
            WEXITSTATUS(status) < 128 + SIGRTMIN * 2)
        status = WEXITSTATUS(status) - ((WEXITSTATUS(status) > 128 + SIGRTMIN) ? SIGRTMIN : 128);

    L_EVT("Exit CT{}:{} {} {}", Id, Name, FormatExitStatus(status),
          (oomKilled ? "invoked by OOM" : ""));

    LockStateWrite();
    ExitStatus = status;
    SetProp(EProperty::EXIT_STATUS);
    if (oomKilled) {
        OomKilled = true;
        SetProp(EProperty::OOM_KILLED);
    }
    UnlockState();

    for (auto &ct: Subtree()) {
        if (ct->State != EContainerState::Stopped &&
                ct->State != EContainerState::Dead)
            ct->Reap(oomKilled);
    }
}

TError TContainer::Pause() {
    if (State != EContainerState::Running && State != EContainerState::Meta)
        return TError(EError::InvalidState, "Contaner not running");

    if (!(Controllers & CGROUP_FREEZER))
        return TError(EError::NotSupported, "Cannot pause without freezer");

    auto cg = GetCgroup(FreezerSubsystem);
    TError error = FreezerSubsystem.Freeze(cg);
    if (error)
        return error;

    for (auto &ct: Subtree()) {
        if (ct->State == EContainerState::Running ||
                ct->State == EContainerState::Meta) {
            ct->SetState(EContainerState::Paused);
            ct->PropagateCpuLimit();
            error = ct->Save();
            if (error)
                L_ERR("Cannot save state after pause: {}", error);
        }
    }

    return OK;
}

TError TContainer::Resume() {
    auto cg = GetCgroup(FreezerSubsystem);
    if (!(Controllers & CGROUP_FREEZER))
        return TError(EError::NotSupported, "Cannot resume without freezer");

    if (FreezerSubsystem.IsParentFreezing(cg))
        return TError(EError::InvalidState, "Parent container is paused");

    if (!FreezerSubsystem.IsSelfFreezing(cg))
        return TError(EError::InvalidState, "Container not paused");

    TError error = FreezerSubsystem.Thaw(cg);
    if (error)
        return error;

    for (auto &ct: Subtree()) {
        auto cg = ct->GetCgroup(FreezerSubsystem);
        if (FreezerSubsystem.IsSelfFreezing(cg))
            FreezerSubsystem.Thaw(cg, false);
        if (ct->State == EContainerState::Paused) {
            ct->SetState(IsMeta() ? EContainerState::Meta : EContainerState::Running);
            ct->PropagateCpuLimit();
        }
        error = ct->Save();
        if (error)
            L_ERR("Cannot save state after resume: {}", error);
    }

    return OK;
}

TError TContainer::MayRespawn() {
    if (State != EContainerState::Dead &&
            State != EContainerState::Respawning)
        return TError(EError::InvalidState, "Cannot respawn: container in state={}", TContainer::StateName(State));

    if (Parent->State != EContainerState::Running &&
            Parent->State != EContainerState::Meta &&
            Parent->State != EContainerState::Respawning)
        return TError(EError::InvalidState, "Cannot respawn: parent container in state={}", TContainer::StateName(Parent->State));

    if (RespawnLimit >= 0 && RespawnCount >= RespawnLimit)
        return TError(EError::ResourceNotAvailable, "Cannot respawn: reached max_respawns={}", RespawnLimit);

    return OK;
}

TError TContainer::ScheduleRespawn() {
    TError error = MayRespawn();
    if (!error) {
        L_ACT("Change CT{}:{} state {} -> {}", Id, Name, StateName(State), StateName(EContainerState::Respawning));
        State = EContainerState::Respawning;
        if (Parent->State == EContainerState::Respawning) {
            L_ACT("Respawn CT{}:{} after respawning parent", Id, Name);
        } else {
            L_ACT("Respawn CT{}:{} after {} ms", Id, Name, RespawnDelay / 1000000);
            TEvent e(EEventType::Respawn, shared_from_this());
            EventQueue->Add(RespawnDelay / 1000000, e);
        }
    }
    return error;
}

TError TContainer::Respawn() {
    TError error;

    error = MayRespawn();
    if (error) {
        if (State == EContainerState::Respawning)
            SetState(EContainerState::Dead);
        return error;
    }

    if (Parent->State == EContainerState::Respawning) {
        if (State != EContainerState::Respawning)
            SetState(EContainerState::Respawning);
        L_ACT("Respawn CT{}:{} after respawning parent", Id, Name);
        return OK;
    }

    LockStateWrite();
    RespawnCount++;
    SetProp(EProperty::RESPAWN_COUNT);
    UnlockState();

    L_ACT("Respawn CT{}:{} try {}", Id, Name, RespawnCount);

    TNetwork::StopNetwork(*this);

    StartError = OK;

    error = PrepareStart();
    if (error) {
        error = TError(error, "Cannot prepare respawn for container {}", Name);
        goto err_prepare;
    }

    StartTime = GetCurrentTimeMs();
    RealStartTime = time(nullptr);
    SetProp(EProperty::START_TIME);

    error = PrepareRuntimeResources();
    if (error)
        goto err;

    CL->LockedContainer->DowngradeActionLock();

    error = StartTask();

    CL->LockedContainer->UpgradeActionLock();

    if (error) {
        (void)Terminate(0);
        goto err;
    }

    if (IsMeta())
        SetState(EContainerState::Meta);
    else
        SetState(EContainerState::Running);

    SetProp(EProperty::ROOT_PID);

    error = Save();
    if (error) {
        L_ERR("Cannot save state after respawn {}", error);
        (void)Reap(false);
        goto err;
    }

    Statistics->ContainersStarted++;

    for (auto &child: Childs()) {
        child->LockStateWrite();
        if (child->State == EContainerState::Respawning || child->AutoRespawn)
            child->ScheduleRespawn();
        child->UnlockState();
    }

    return OK;

err:
    TNetwork::StopNetwork(*this);
    FreeRuntimeResources();

err_prepare:
    StartError = error;
    DeathTime = GetCurrentTimeMs();
    RealDeathTime = time(nullptr);
    SetProp(EProperty::DEATH_TIME);

    Statistics->ContainersFailedStart++;
    L("Cannot respawn CT{}:{} - {}", Id, Name, error);
    SetState(EContainerState::Dead);
    (void)Save();
    return error;
}

void TContainer::SyncProperty(const std::string &name) {
    PORTO_ASSERT(IsStateLockedRead());
    if (StringStartsWith(name, "net_") && Net)
        Net->SyncStat();
    if (StringStartsWith(name, "oom_kills"))
        CollectOomKills();
}

void TContainer::SyncPropertiesAll() {
    TNetwork::SyncAllStat();
    RootContainer->CollectOomKills();
}

/* return true if index specified for property */
static bool ParsePropertyName(std::string &name, std::string &idx) {
    if (name.size() && name.back() == ']') {
        auto lb = name.find('[');

        if (lb != std::string::npos) {
            idx = name.substr(lb + 1);
            idx.pop_back();
            name = name.substr(0, lb);

            return true;
        }
    }

    return false;
}

TError TContainer::HasProperty(const std::string &property) const {
    std::string name = property, index;
    TError error;

    if (!ParsePropertyName(name, index)) {
        auto dot = name.find('.');
        if (dot != std::string::npos) {
            std::string type = property.substr(0, dot);

            if (type.find_first_not_of(PORTO_LABEL_PREFIX_CHARS) == std::string::npos) {
                auto lock = LockContainers();
                return GetLabel(property, type);
            }

            if (State == EContainerState::Stopped)
                return TError(EError::InvalidState, "Not available in stopped state");

            for (auto subsys: Subsystems) {
                if (subsys->Type != type)
                    continue;
                if (subsys->Kind & Controllers)
                    return OK;
                return TError(EError::NoValue, "Controllers is disabled");
            }
            return TError(EError::InvalidProperty, "Unknown controller");
        }
    }

    auto it = ContainerProperties.find(name);
    if (it == ContainerProperties.end())
        return TError(EError::InvalidProperty, "Unknown property");

    auto prop = it->second;

    if (!prop->IsSupported)
        return TError(EError::NotSupported, "Not supported");

    if (prop->Prop != EProperty::NONE && !HasProp(prop->Prop))
        return TError(EError::NoValue, "Property not set");

    if (prop->RequireControllers) {
        if (State == EContainerState::Stopped)
            return TError(EError::InvalidState, "Not available in stopped state");
        if (!(prop->RequireControllers & Controllers))
            return TError(EError::NoValue, "Controllers is disabled");
    }

    CT = const_cast<TContainer *>(this);
    error = prop->Has();
    CT = nullptr;

    return error;
}

TError TContainer::GetProperty(const std::string &origProperty, std::string &value) const {
    TError error;
    std::string property = origProperty;
    std::string idx;

    if (!ParsePropertyName(property, idx)) {
        auto dot = property.find('.');

        if (dot != std::string::npos) {
            std::string type = property.substr(0, dot);

            if (type.find_first_not_of(PORTO_LABEL_PREFIX_CHARS) == std::string::npos) {
                auto lock = LockContainers();
                return GetLabel(property, value);
            }

            if (State == EContainerState::Stopped)
                return TError(EError::InvalidState,
                        "Not available in stopped state: " + property);
            for (auto subsys: Subsystems) {
                if (subsys->Type == type) {
                    auto cg = GetCgroup(*subsys);
                    if (!cg.Has(property))
                        break;
                    return cg.Get(property, value);
                }
            }
            return TError(EError::InvalidProperty,
                    "Unknown cgroup attribute: " + property);
        }
    } else if (!idx.length()) {
        return TError(EError::InvalidProperty, "Empty property index");
    }

    auto it = ContainerProperties.find(property);
    if (it == ContainerProperties.end())
        return TError(EError::InvalidProperty,
                              "Unknown container property: " + property);
    auto prop = it->second;

    CT = const_cast<TContainer *>(this);
    error = prop->CanGet();
    if (!error) {
        if (idx.length())
            error = prop->GetIndexed(idx, value);
        else
            error = prop->Get(value);
    }
    CT = nullptr;

    return error;
}

TError TContainer::SetProperty(const std::string &origProperty,
                               const std::string &origValue) {
    if (IsRoot())
        return TError(EError::Permission, "System containers are read only");

    std::string property = origProperty;
    std::string idx;

    if (ParsePropertyName(property, idx) && !idx.length())
        return TError(EError::InvalidProperty, "Empty property index");

    std::string value = StringTrim(origValue);
    TError error;

    auto it = ContainerProperties.find(property);
    if (it == ContainerProperties.end()) {
        auto dot = property.find('.');

        if (dot != std::string::npos) {
            std::string type = property.substr(0, dot);

            if (type.find_first_not_of(PORTO_LABEL_PREFIX_CHARS) == std::string::npos) {
                error = TContainer::ValidLabel(property, value);
                if (error)
                    return error;
                auto lock = LockContainers();
                SetLabel(property, value);
                lock.unlock();
                TContainerWaiter::ReportAll(*this, property, value);
                return Save();
            }
        }

        return TError(EError::InvalidProperty, "Invalid property " + property);
    }
    auto prop = it->second;

    CT = this;

    error = prop->CanSet();

    if (!error && prop->RequireControllers)
        error = EnableControllers(prop->RequireControllers);

    std::string oldValue;
    if (!error)
        error = prop->Get(oldValue);

    if (!error) {
        if (idx.length())
            error = prop->SetIndexed(idx, value);
        else
            error = prop->Set(value);
    }

    if (!error && HasResources()) {
        error = ApplyDynamicProperties();
        if (error) {
            (void)prop->Set(oldValue);
            (void)TestClearPropDirty(prop->Prop);
        }
    }

    CT = nullptr;

    if (!error)
        error = Save();

    return error;
}

TError TContainer::Save(void) {
    TKeyValue node(ContainersKV / std::to_string(Id));
    TError error;

    ChangeTime = time(nullptr);

    /* These are not properties */
    node.Set(P_RAW_ID, std::to_string(Id));
    node.Set(P_RAW_NAME, Name);

    auto prev_ct = CT;
    CT = this;

    for (auto knob : ContainerProperties) {
        std::string value;

        /* Skip knobs without a value */
        if (knob.second->Prop == EProperty::NONE || !HasProp(knob.second->Prop))
            continue;

        error = knob.second->Get(value);
        if (error)
            break;

        /* Temporary hack for backward migration */
        if (knob.second->Prop == EProperty::STATE &&
                State == EContainerState::Respawning)
            value = "dead";

        node.Set(knob.first, value);
    }

    CT = prev_ct;

    if (error)
        return error;

    return node.Save();
}

TError TContainer::Load(const TKeyValue &node) {
    EContainerState state = EContainerState::Destroyed;
    uint64_t controllers = 0;
    TError error;

    CT = this;
    LockStateWrite();

    OwnerCred = CL->Cred;

    for (auto &kv: node.Data) {
        std::string key = kv.first;
        std::string value = kv.second;

        if (key == P_STATE) {
            /*
             * We need to set state at the last moment
             * because properties depends on the current value
             */
            state = ParseState(value);
            continue;
        }

        if (key == P_RAW_ID || key == P_RAW_NAME)
            continue;

        auto it = ContainerProperties.find(key);
        if (it == ContainerProperties.end()) {
            L_WRN("Unknown property: {}, skipped", key);
            continue;
        }
        auto prop = it->second;

        controllers |= prop->RequireControllers;

        error = prop->Set(value);

        if (error.Error == EError::NotSupported) {
            L_WRN("Unsupported property: {}, skipped", key);
            continue;
        }

        if (error) {
            L_ERR("Cannot load {} : {}", key, error);
            state = EContainerState::Dead;
            break;
        }

        SetProp(prop->Prop);
    }

    if (state != EContainerState::Destroyed) {
        UnlockState();
        SetState(state);
        LockStateWrite();
        SetProp(EProperty::STATE);
    } else
        error = TError("Container has no state");

    if (!node.Has(P_CONTROLLERS) && State != EContainerState::Stopped)
        Controllers = RootContainer->Controllers;

    if (Level == 1 && CpusetSubsystem.Supported &&
            !(Controllers & CGROUP_CPUSET))
        Controllers |= CGROUP_CPUSET;

    if (controllers & ~Controllers)
        L_WRN("Missing cgroup controllers {}", TSubsystem::Format(controllers & ~Controllers));

    if (!node.Has(P_OWNER_USER) || !node.Has(P_OWNER_GROUP))
        OwnerCred = TaskCred;

    SanitizeCapabilities();

    UnlockState();
    CT = nullptr;

    return error;
}

TError TContainer::Seize() {
    if (SeizeTask.Pid) {
        if (GetTaskName(SeizeTask.Pid) == "portoinit") {
            pid_t ppid = SeizeTask.GetPPid();
            if (ppid == getpid() || ppid == getppid())
                return OK;
            while(!kill(SeizeTask.Pid, SIGKILL))
                usleep(100000);
        }
        SeizeTask.Pid = 0;
    }

    auto pidStr = std::to_string(WaitTask.Pid);
    const char * argv[] = {
        "portoinit",
        "--container",
        Name.c_str(),
        "--seize",
        pidStr.c_str(),
        NULL,
    };

    auto cg = GetCgroup(FreezerSubsystem);
    TPath path = TPath(PORTO_HELPERS_PATH) / "portoinit";

    TError error = SeizeTask.Fork(true);
    if (error)
        return error;

    if (SeizeTask.Pid) {
        SetProp(EProperty::SEIZE_PID);
        return OK;
    }

    if (cg.Attach(GetPid()))
        _exit(EXIT_FAILURE);

    execv(path.c_str(), (char *const *)argv);

    TPath exe("/proc/self/exe");
    error = exe.ReadLink(path);
    if (error)
        _exit(EXIT_FAILURE);

    path = path.DirName() / "portoinit";

    execv(path.c_str(), (char *const *)argv);

    _exit(EXIT_FAILURE);
}

void TContainer::SyncState() {
    TCgroup taskCg, freezerCg = GetCgroup(FreezerSubsystem);
    TError error;

    L_ACT("Sync CT{}:{} state {}", Id, Name, StateName(State));

    if (!freezerCg.Exists()) {
        if (State != EContainerState::Stopped &&
                State != EContainerState::Stopping)
            L_WRN("Freezer not found");
        ForgetPid();
        SetState(EContainerState::Stopped);
        return;
    }

    if (State == EContainerState::Starting ||
            State == EContainerState::Respawning)
        SetState(IsMeta() ? EContainerState::Meta : EContainerState::Running);

    if (FreezerSubsystem.IsFrozen(freezerCg)) {
        if (State != EContainerState::Paused)
            FreezerSubsystem.Thaw(freezerCg);
    } else if (State == EContainerState::Paused)
        SetState(IsMeta() ? EContainerState::Meta : EContainerState::Running);

    if (State == EContainerState::Stopped || State == EContainerState::Stopping) {
        if (State == EContainerState::Stopped)
            L("Found unexpected freezer");
        Stop(0);
    } else if (State == EContainerState::Meta && !WaitTask.Pid && !Isolate) {
        /* meta container */
    } else if (!WaitTask.Exists()) {
        if (State != EContainerState::Dead)
            L("Task not found");
        Reap(false);
    } else if (WaitTask.IsZombie()) {
        L("Task is zombie");
        Task.Pid = 0;
    } else if (FreezerSubsystem.TaskCgroup(WaitTask.Pid, taskCg)) {
        L("Cannot check freezer");
        Reap(false);
    } else if (taskCg != freezerCg) {
        L("Task in wrong freezer");
        if (WaitTask.GetPPid() == getppid()) {
            if (Task.Pid != WaitTask.Pid && Task.GetPPid() == WaitTask.Pid)
                Task.Kill(SIGKILL);
            WaitTask.Kill(SIGKILL);
        }
        Reap(false);
    } else {
        pid_t ppid = WaitTask.GetPPid();
        if (ppid != getppid()) {
            L("Task reparented to {} ({}). Seize.", ppid, GetTaskName(ppid));
            error = Seize();
            if (error) {
                L("Cannot seize reparented task: {}", error);
                Reap(false);
            }
        }
    }

    switch (Parent ? Parent->State : EContainerState::Meta) {
        case EContainerState::Stopped:
            if (State != EContainerState::Stopped)
                Stop(0); /* Also stop paused */
            break;
        case EContainerState::Dead:
            if (State != EContainerState::Dead && State != EContainerState::Stopped)
                Reap(false);
            break;
        case EContainerState::Running:
        case EContainerState::Meta:
        case EContainerState::Starting:
        case EContainerState::Stopping:
        case EContainerState::Respawning:
            /* Any state is ok */
            break;
        case EContainerState::Paused:
            if (State == EContainerState::Running || State == EContainerState::Meta)
                SetState(EContainerState::Paused);
            break;
        case EContainerState::Destroyed:
            L_ERR("Destroyed parent?");
            break;
    }

    if (State != EContainerState::Stopped && !HasProp(EProperty::START_TIME)) {
        StartTime = GetCurrentTimeMs();
        RealStartTime = time(nullptr);
        SetProp(EProperty::START_TIME);
    }

    if (State == EContainerState::Dead && !HasProp(EProperty::DEATH_TIME)) {
        DeathTime = GetCurrentTimeMs();
        RealDeathTime = time(nullptr);
        SetProp(EProperty::DEATH_TIME);
    }
}

TError TContainer::SyncCgroups() {
    TError error;

    /* Sync by parent container */
    if (JobMode)
        return OK;

    if (!(Controllers & CGROUP_FREEZER))
        return TError(EError::NotSupported, "Cannot sync cgroups without freezer");

    auto freezer = GetCgroup(FreezerSubsystem);
    for (auto hy: Hierarchies) {
        if (hy->Controllers & CGROUP_FREEZER)
            continue;
        auto cg = GetCgroup(*hy);
        error = cg.AttachAll(freezer);
        if (error)
            break;
    }
    return error;
}

TCgroup TContainer::GetCgroup(const TSubsystem &subsystem) const {
    if (IsRoot())
        return subsystem.RootCgroup();

    if (subsystem.Controllers & CGROUP_FREEZER) {
        if (JobMode)
            return subsystem.Cgroup(std::string(PORTO_CGROUP_PREFIX) + "/" + Parent->Name);
        return subsystem.Cgroup(std::string(PORTO_CGROUP_PREFIX) + "/" + Name);
    }

    if (subsystem.Controllers & CGROUP_SYSTEMD) {
        if (Controllers & CGROUP_SYSTEMD)
            return subsystem.Cgroup(std::string(PORTO_CGROUP_PREFIX) + "%" +
                                    StringReplaceAll(Name, "/", "%"));
        return SystemdSubsystem.PortoService;
    }

    std::string cg;
    for (auto ct = this; !ct->IsRoot(); ct = ct->Parent.get()) {
        auto enabled = ct->Controllers & subsystem.Controllers;

        if (!cg.empty())
            cg = (enabled ? "/" : "%") + cg;
        if (!cg.empty() || enabled)
            cg = ct->FirstName + cg;
    }

    if (cg.empty())
        return subsystem.RootCgroup();

    return subsystem.Cgroup(std::string(PORTO_CGROUP_PREFIX) + "%" + cg);
}

TError TContainer::EnableControllers(uint64_t controllers) {
    if (State == EContainerState::Stopped) {
        Controllers |= controllers;
        RequiredControllers |= controllers;
    } else if ((Controllers & controllers) != controllers)
        return TError(EError::NotSupported, "Cannot enable controllers in runtime");
    return OK;
}

bool TContainer::RecvOomEvents() {
    uint64_t val;

    if (OomEvent && read(OomEvent.Fd, &val, sizeof(val)) == sizeof(val) && val) {
        OomEvents += val;
        Statistics->ContainersOOM += val;
        L_EVT("OOM Event in CT{}:{}", Id, Name);
        CollectOomKills(true);
        return true;
    }

    return false;
}

void TContainer::Event(const TEvent &event) {
    TError error;

    auto ct = event.Container.lock();

    switch (event.Type) {

    case EEventType::OOM:
    {
        if (ct && !CL->LockContainer(ct) && ct->OomIsFatal)
            ct->Exit(SIGKILL, true);
        break;
    }

    case EEventType::Respawn:
    {
        if (ct && !CL->LockContainer(ct))
            ct->Respawn();
        break;
    }

    case EEventType::Exit:
    case EEventType::ChildExit:
    {
        bool delivered = false;

        auto lock = LockContainers();
        for (auto &it: Containers) {
            if (it.second->WaitTask.Pid != event.Exit.Pid &&
                    it.second->SeizeTask.Pid != event.Exit.Pid)
                continue;
            ct = it.second;
            break;
        }
        lock.unlock();

        if (ct && !CL->LockContainer(ct)) {
            if (ct->WaitTask.Pid == event.Exit.Pid ||
                    ct->SeizeTask.Pid == event.Exit.Pid) {
                ct->Exit(event.Exit.Status, false);
                delivered = true;
            }
            CL->ReleaseContainer();
        }

        if (event.Type == EEventType::Exit) {
            AckExitStatus(event.Exit.Pid);
        } else {
            if (!delivered)
                L("Unknown zombie {} {}", event.Exit.Pid, event.Exit.Status);
            (void)waitpid(event.Exit.Pid, NULL, 0);
        }

        break;
    }

    case EEventType::WaitTimeout:
    {
        auto waiter = event.WaitTimeout.Waiter.lock();
        if (waiter)
            waiter->Timeout();
        break;
    }

    case EEventType::DestroyAgedContainer:
    {
        if (ct && !CL->LockContainer(ct)) {
            std::list<std::shared_ptr<TVolume>> unlinked;

            if (ct->State == EContainerState::Dead &&
                    GetCurrentTimeMs() >= ct->DeathTime + ct->AgingTime) {
                Statistics->RemoveDead++;
                ct->Destroy(unlinked);
            }
            CL->ReleaseContainer();

            TVolume::DestroyUnlinked(unlinked);
        }
        break;
    }

    case EEventType::DestroyWeakContainer:
        if (ct && !CL->LockContainer(ct)) {
            std::list<std::shared_ptr<TVolume>> unlinked;

            if (ct->IsWeak)
                ct->Destroy(unlinked);
            CL->ReleaseContainer();

            TVolume::DestroyUnlinked(unlinked);
        }
        break;

    case EEventType::RotateLogs:
    {
        for (auto &ct: RootContainer->Subtree()) {
            if (ct->State == EContainerState::Dead &&
                    GetCurrentTimeMs() >= ct->DeathTime + ct->AgingTime) {
                TEvent ev(EEventType::DestroyAgedContainer, ct);
                EventQueue->Add(0, ev);
            }
            if (ct->State == EContainerState::Running) {
                ct->Stdout.Rotate(*ct);
                ct->Stderr.Rotate(*ct);
            }
        }

        struct stat st;
        if (!StdLog && LogFile && !LogFile.Stat(st) && !st.st_nlink)
            ReopenMasterLog();

        CheckPortoSocket();

        EventQueue->Add(config().daemon().log_rotate_ms(), event);
        break;
    }
    }
}

std::string TContainer::GetPortoNamespace(bool write) const {
    std::string ns;
    for (auto ct = this; ct && !ct->IsRoot() ; ct = ct->Parent.get()) {
        if (ct->AccessLevel == EAccessLevel::Isolate ||
                ct->AccessLevel == EAccessLevel::ReadIsolate ||
                ct->AccessLevel == EAccessLevel::SelfIsolate ||
                (write && ct->AccessLevel == EAccessLevel::ChildOnly))
            return ct->Name + "/" + ns;
        ns = ct->NsName + ns;
    }
    return ns;
}

TTuple TContainer::Taint() {
    TTuple taint;

    if (OwnerCred.IsRootUser() && Level && !HasProp(EProperty::CAPABILITIES))
        taint.push_back("Container owned by root has unrestricted capabilities.");

    if (NetIsolate && Hostname == "")
        taint.push_back("Container with network namespace without hostname is confusing.");

    if (BindDns)
        taint.push_back("Property bind_dns is deprecated and will be removed soon.");

    if (!OomIsFatal)
        taint.push_back("Containers with oom_is_fatal=false oftern stuck in broken state after OOM, you have been warned.");

    if (OsMode && Isolate && HasProp(EProperty::COMMAND) && Command != "/sbin/init")
        taint.push_back("Containers virt_mode=os and custom command often infected with zombies, use virt_mode=app user=root group=root.");

    if (CpuPolicy == "rt" && CpuLimit)
        taint.push_back("RT scheduler works really badly when usage hits cpu_limit, use cpu_policy=high");

    if (Level == 1) {
        if (!MemLimit || !HasProp(EProperty::MEM_LIMIT))
            taint.push_back("First level container without memory_limit.");
        if (!CpuLimit)
            taint.push_back("First level container without cpu_limit.");
        if (!Isolate)
            taint.push_back("First level container without pid namespace.");
        if (!(Controllers & CGROUP_DEVICES))
            taint.push_back("First level container without devices cgroup.");
    }

    if (Root != "/") {
        if (!(Controllers & CGROUP_DEVICES))
            taint.push_back("Container with chroot and without devices cgroup.");
    }

    if (AccessLevel >= EAccessLevel::Normal) {
        if (Root != "/")
            taint.push_back("Container could escape chroot with enable_porto=true.");
        if (Isolate)
            taint.push_back("Container could escape pid namespace with enable_porto=true.");
        if (NetIsolate)
            taint.push_back("Container could escape network namespace with enable_porto=true.");
    }

    if (AccessLevel > EAccessLevel::ReadOnly) {
        if (NetIsolate && IpPolicy == "any")
            taint.push_back("Container could escape network namespace without ip_limit.");
    }

    if (TaintFlags.RootOnLoop)
        taint.push_back("Container with deprecated root=loop.img");

    if (TaintFlags.BindWithSuid)
        taint.push_back("Container with bind mount source which allows suid in host");

    return taint;
}
