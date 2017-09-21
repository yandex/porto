#include <sstream>
#include <fstream>
#include <memory>
#include <csignal>
#include <cstdlib>
#include <algorithm>
#include <condition_variable>

#include "portod.hpp"
#include "statistics.hpp"
#include "container.hpp"
#include "config.hpp"
#include "task.hpp"
#include "cgroup.hpp"
#include "device.hpp"
#include "property.hpp"
#include "event.hpp"
#include "network.hpp"
#include "epoll.hpp"
#include "kvalue.hpp"
#include "volume.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/cred.hpp"
#include "util/unix.hpp"
#include "client.hpp"
#include "filesystem.hpp"
#include "rpc.hpp"

extern "C" {
#include <sys/types.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/reboot.h>
#include <fcntl.h>
#include <sys/fsuid.h>
#include <sys/stat.h>
#include <sys/wait.h>
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

TError TContainer::ValidName(const std::string &name) {

    if (name.length() == 0)
        return TError(EError::InvalidValue, "container path too short");

    if (name.length() > CONTAINER_PATH_MAX)
        return TError(EError::InvalidValue, "container path too long, limit is " +
                                            std::to_string(CONTAINER_PATH_MAX));

    if (name[0] == '/') {
        if (name == ROOT_CONTAINER)
            return TError::Success();
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

    return TError::Success();
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
        return TError::Success();
    return TError(EError::ContainerDoesNotExist, "container " + name + " not found");
}

TError TContainer::FindTaskContainer(pid_t pid, std::shared_ptr<TContainer> &ct) {
    TError error;
    TCgroup cg;

    error = FreezerSubsystem.TaskCgroup(pid, cg);
    if (error)
        return error;

    std::string prefix = std::string(PORTO_CGROUP_PREFIX) + "/";
    std::string name = cg.Name;
    std::replace(name.begin(), name.end(), '%', '/');

    auto containers_lock = LockContainers();

    if (!StringStartsWith(name, prefix))
        return TContainer::Find(ROOT_CONTAINER, ct);

    return TContainer::Find(name.substr(prefix.length()), ct);
}

/* lock subtree for read or write */
TError TContainer::Lock(TScopedLock &lock, bool for_read, bool try_lock) {
    if (Debug)
        L("{} {} CT{}:{}",
          (try_lock ? "TryLock" : "Lock"),
          (for_read ? "read" : "write"),
          Id, Name);

    while (1) {
        if (State == EContainerState::Destroyed) {
            if (Debug)
                L("Lock failed, container CT{}:{} was destroyed", Id, Name);
            return TError(EError::ContainerDoesNotExist, "Container was destroyed");
        }
        bool busy;
        if (for_read)
            busy = Locked < 0 || PendingWrite || SubtreeWrite;
        else
            busy = Locked || SubtreeRead || SubtreeWrite;
        for (auto ct = Parent.get(); !busy && ct; ct = ct->Parent.get())
            busy = ct->PendingWrite || (for_read ? ct->Locked < 0 : ct->Locked);
        if (!busy)
            break;
        if (try_lock) {
            if (Debug)
                L("TryLock {} Failed CT{}:{}", (for_read ? "read" : "write"), Id, Name);
            return TError(EError::Busy, "Container is busy: " + Name);
        }
        if (!for_read)
            PendingWrite = true;
        ContainersCV.wait(lock);
    }
    PendingWrite = false;
    Locked += for_read ? 1 : -1;
    LastOwner = GetTid();
    for (auto ct = Parent.get(); ct; ct = ct->Parent.get()) {
        if (for_read)
            ct->SubtreeRead++;
        else
            ct->SubtreeWrite++;
    }
    return TError::Success();
}

void TContainer::DowngradeLock() {
    auto lock = LockContainers();
    PORTO_ASSERT(Locked == -1);

    if (Debug)
        L("Downgrading write to read CT{}:{}", Id, Name);

    for (auto ct = Parent.get(); ct; ct = ct->Parent.get()) {
        ct->SubtreeRead++;
        ct->SubtreeWrite--;
    }

    Locked = 1;
    ContainersCV.notify_all();
}

void TContainer::UpgradeLock() {
    auto lock = LockContainers();

    if (Debug)
        L("Upgrading read back to write CT{}:{}", Id, Name);

    PendingWrite = true;

    for (auto ct = Parent.get(); ct; ct = ct->Parent.get()) {
        ct->SubtreeRead--;
        ct->SubtreeWrite++;
    }

    while (Locked != 1)
        ContainersCV.wait(lock);

    Locked = -1;
    LastOwner = GetTid();

    PendingWrite = false;
}

void TContainer::Unlock(bool locked) {
    if (Debug)
        L("Unlock {} CT{}:{}", (Locked > 0 ? "read" : "write"), Id, Name);
    if (!locked)
        ContainersMutex.lock();
    for (auto ct = Parent.get(); ct; ct = ct->Parent.get()) {
        if (Locked > 0) {
            PORTO_ASSERT(ct->SubtreeRead > 0);
            ct->SubtreeRead--;
        } else {
            PORTO_ASSERT(ct->SubtreeWrite > 0);
            ct->SubtreeWrite--;
        }
    }
    PORTO_ASSERT(Locked);
    Locked += (Locked > 0) ? -1 : 1;
    /* not so effective and fair but simple */
    ContainersCV.notify_all();
    if (!locked)
        ContainersMutex.unlock();
}

void TContainer::DumpLocks() {
    auto lock = LockContainers();
    for (auto &it: Containers) {
        auto &ct = it.second;
        if (ct->Locked || ct->PendingWrite || ct->SubtreeRead || ct->SubtreeWrite)
            L("CT{}:{} Locked {} by {} Read {} Write {}{}", ct->Id, ct->Name, ct->Locked,
                ct->LastOwner, ct->SubtreeRead, ct->SubtreeWrite,
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

    std::fill(PropSet, PropSet + sizeof(PropSet), false);
    std::fill(PropDirty, PropDirty + sizeof(PropDirty), false);


    Stdin.SetOutside("/dev/null");
    Stdout.SetOutside("stdout");
    Stderr.SetOutside("stderr");
    Stdout.Limit = config().container().stdout_limit();
    Stderr.Limit = config().container().stdout_limit();
    Root = "/";
    RootPath = TPath("/");
    RootRo = false;
    Umask = 0002;
    Isolate = true;
    BindDns = config().container().default_bind_dns();
    VirtMode = VIRT_MODE_APP;

    NetProp = { { "inherited" } };
    NetIsolate = false;
    NetInherit = true;

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
        Place = { PORTO_PLACE, "***" };
    else
        Place = Parent->Place;

    CpuPolicy = "normal";
    ChooseSchedPolicy();

    CpuLimit = GetNumCores();
    CpuPeriod = config().container().cpu_period();

    if (IsRoot()) {
        SetProp(EProperty::CPU_LIMIT);
        SetProp(EProperty::MEM_LIMIT);
    }

    IoPolicy = "";
    IoPrio = 0;

    PressurizeOnDeath = config().container().pressurize_on_death();

    Controllers = RequiredControllers = CGROUP_FREEZER;

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

    if (Level == 1 && PidsSubsystem.Supported) {
        Controllers |= CGROUP_PIDS;

        if (config().container().default_thread_limit()) {
            ThreadLimit = config().container().default_thread_limit();
            SetProp(EProperty::THREAD_LIMIT);
        }
    }

    SetProp(EProperty::CONTROLLERS);

    NetClass.Prio["default"] = NET_DEFAULT_PRIO;
    ToRespawn = false;
    MaxRespawns = -1;
    RespawnCount = 0;
    Private = "";
    AgingTime = config().container().default_aging_time_s() * 1000;

    if (Parent && Parent->AccessLevel == EAccessLevel::None)
        AccessLevel = EAccessLevel::None;
    else if (Parent && Parent->AccessLevel <= EAccessLevel::ReadOnly)
        AccessLevel = EAccessLevel::ReadOnly;
    else
        AccessLevel = EAccessLevel::Normal;
}

TContainer::~TContainer() {
    PORTO_ASSERT(Net == nullptr);
    PORTO_ASSERT(!NetClass.Registered);
    Statistics->ContainersCount--;
}

TError TContainer::Create(const std::string &name, std::shared_ptr<TContainer> &ct) {
    auto nrMax = config().container().max_total();
    TError error;
    int id = -1;

    error = ValidName(name);
    if (error)
        return error;

    auto lock = LockContainers();

    auto parent = TContainer::Find(TContainer::ParentName(name));
    if (parent) {
        if (parent->Level == CONTAINER_LEVEL_MAX)
            return TError(EError::InvalidValue, "You shall not go deeper! Maximum level is " + std::to_string(CONTAINER_LEVEL_MAX));
        error = parent->LockRead(lock);
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

    if (Containers.size() >= nrMax + NR_SERVICE_CONTAINERS) {
        error = TError(EError::ResourceNotAvailable,
                "number of containers reached limit: " + std::to_string(nrMax));
        goto err;
    }

    error = ContainerIdMap.Get(id);
    if (error)
        goto err;

    L_ACT("Create container {}:{}", id, name);

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
        parent->Unlock(true);

    return TError::Success();

err:
    if (parent)
        parent->Unlock(true);
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

    L_ACT("Restore container {}:{}", id, kv.Name);

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

    ct->RootPath = parent->RootPath / ct->Root;

    ct->SyncState();

    TNetwork::InitClass(*ct);

    /* Restore cgroups only for running containers */
    if (ct->State != EContainerState::Stopped &&
            ct->State != EContainerState::Dead) {

        error = TNetwork::RestoreNetwork(*ct);
        if (error)
            goto err;

        error = ct->PrepareCgroups();
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

        error = ct->SyncCgroups();
        if (error)
            goto err;
    }

    if (ct->MayRespawn())
        ct->ScheduleRespawn();

    error = ct->Save();
    if (error)
        goto err;

    SystemClient.ReleaseContainer();

    return TError::Success();

err:
    TNetwork::StopNetwork(*ct);
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
    case EContainerState::Starting:
        return "starting";
    case EContainerState::Running:
        return "running";
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
    if (name == "starting")
        return EContainerState::Starting;
    if (name == "running")
        return EContainerState::Running;
    if (name == "paused")
        return EContainerState::Paused;
    if (name == "meta")
        return EContainerState::Meta;
    return EContainerState::Destroyed;
}

/* Working directory in host namespace */
TPath TContainer::WorkPath() const {
    if (IsRoot())
        return TPath("/");
    return TPath(PORTO_WORKDIR) / Name;
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

    return WorkPath();
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

    return TError::Success();
}

void TContainer::SetState(EContainerState next) {
    if (State == next)
        return;

    L_ACT("Change container CT{}:{} state {} -> {}", Id, Name, StateName(State), StateName(next));

    auto lock = LockContainers();
    auto prev = State;
    State = next;

    if (prev == EContainerState::Starting || next == EContainerState::Starting) {
        for (auto p = Parent; p; p = p->Parent)
            p->StartingChildren += next == EContainerState::Starting ? 1 : -1;
    }

    if (prev == EContainerState::Running || next == EContainerState::Running) {
        for (auto p = Parent; p; p = p->Parent) {
            p->RunningChildren += next == EContainerState::Running ? 1 : -1;
            if (!p->RunningChildren && p->State == EContainerState::Meta)
                p->NotifyWaiters();
        }
    }

    if (next != EContainerState::Running &&
            next != EContainerState::Meta &&
            next != EContainerState::Starting)
        NotifyWaiters();
}

TError TContainer::Destroy() {
    TError error;

    L_ACT("Destroy container CT{}:{}", Id, Name);

    if (State != EContainerState::Stopped) {
        error = Stop(0);
        if (error)
            return error;
    }

    if (!Children.empty()) {
        for (auto &ct: Subtree()) {
            if (ct.get() != this) {
                error = ct->Destroy();
                if (error)
                    return error;
            }
        }
    }

    while (!LinkedVolumes.empty()) {
        std::shared_ptr<TVolume> volume = LinkedVolumes.back();
        if (!volume->UnlinkContainer(*this) && volume->IsDying)
            volume->Destroy();
    }

    if (!OwnedVolumes.empty() && Parent) {
        auto lock = LockVolumes();
        for (auto &vol: OwnedVolumes) {
            vol->VolumeOwnerContainer = Parent;
            Parent->OwnedVolumes.push_back(vol);
        }
        OwnedVolumes.clear();
    }

    auto lock = LockContainers();

    Unregister();

    TPath path(ContainersKV / std::to_string(Id));
    error = path.Unlink();
    if (error)
        L_ERR("Can't remove key-value node {}: {}", path, error);

    return TError::Success();
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

    return IsChildOf(*ns);
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
        else if (VirtMode == VIRT_MODE_OS || IsMeta())
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

uint64_t TContainer::GetTotalMemGuarantee(bool locked) const {
    uint64_t sum = 0lu;

    // FIXME ugly
    if (!locked)
        ContainersMutex.lock();

    for (auto &child : Children)
        sum += child->GetTotalMemGuarantee(true);

    sum = std::max(NewMemGuarantee, sum);

    if (!locked)
        ContainersMutex.unlock();

    return sum;
}

uint64_t TContainer::GetTotalMemLimit(const TContainer *base) const {
    uint64_t lim = 0;

    // FIXME ugly
    if (!base)
        ContainersMutex.lock();

    /* Container without load limited with total limit of childrens */
    if (IsMeta() && VirtMode == VIRT_MODE_APP) {
        for (auto &child : Children) {
            auto child_lim = child->GetTotalMemLimit(this);
            if (!child_lim || child_lim > UINT64_MAX - lim) {
                lim = 0;
                break;
            }
            lim += child_lim;
        }
    }

    for (auto p = this; p && p != base; p = p->Parent.get()) {
        if (p->MemLimit && (p->MemLimit < lim || !lim))
            lim = p->MemLimit;
    }

    if (!base)
        ContainersMutex.unlock();

    return lim;
}

TError TContainer::ApplyUlimits() {
    auto cg = GetCgroup(FreezerSubsystem);
    std::map<int, struct rlimit> map;
    std::vector<pid_t> prev, pids;
    TError error;
    bool retry;

    auto ulimit = GetUlimit();

    for (auto &it: ulimit) {
        int res;
        struct rlimit lim;

        error = ParseUlimit(it.first, it.second, res, lim);
        if (error)
            return error;
        map[res] = lim;
    }

    L_ACT("Apply ulimits");
    do {
        error = cg.GetTasks(pids);
        if (error)
            return error;
        retry = false;
        for (auto pid: pids) {
            if (std::find(prev.begin(), prev.end(), pid) != prev.end())
                continue;
            for (auto &it: map) {
                if (prlimit(pid, (enum __rlimit_resource)it.first,
                            &it.second, NULL) && errno != ESRCH)
                    return TError(EError::Unknown, errno, "prlimit");
            }
            retry = true;
        }
        prev = pids;
    } while (retry);

    return TError::Success();
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
            /* x2 weight is +1 rt priority */
            SchedPrio += std::log2(CpuWeight);
            SchedPrio = std::max(SchedPrio, sched_get_priority_min(SCHED_RR));
            SchedPrio = std::min(SchedPrio, sched_get_priority_max(SCHED_RR));
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
                return TError(EError::Unknown, errno, "setpriority");
            if (sched_setscheduler(pid, SchedPolicy, &param) &&
                    errno != ESRCH)
                return TError(EError::Unknown, errno, "sched_setscheduler");
            retry = true;
        }
        prev = pids;
    } while (retry);

    return TError::Success();
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
                return TError(EError::Unknown, errno, "ioprio");
            retry = true;
        }
        prev = pids;
    } while (retry);

    return TError::Success();
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
        return TError(EError::ResourceNotAvailable, "Not enough cpus in " + Name);
    }

    return TError::Success();
}

TError TContainer::DistributeCpus() {
    auto lock = LockCpuAffinity();
    TError error;

    if (IsRoot()) {
        error = CpuAffinity.Load("/sys/devices/system/cpu/online");
        if (error)
            return error;

        CoreThreads.clear();
        CoreThreads.resize(CpuAffinity.Size());

        for (unsigned cpu = 0; cpu < CpuAffinity.Size(); cpu++) {
            if (!CpuAffinity.Get(cpu))
                continue;
            error = CoreThreads[cpu].Load(StringFormat("/sys/devices/system/cpu/cpu%u/topology/thread_siblings_list", cpu));
            if (error)
                return error;
        }

        error = NumaNodes.Load("/sys/devices/system/node/online");
        if (error)
            return error;

        NodeThreads.clear();
        NodeThreads.resize(NumaNodes.Size());

        for (unsigned node = 0; node < NumaNodes.Size(); node++) {
            if (!NumaNodes.Get(node))
                continue;
            error = NodeThreads[node].Load(StringFormat("/sys/devices/system/node/node%u/cpulist", node));
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

        if (Verbose)
            L("Distribute CPUs {} in {}", parent->CpuVacant.Format(), parent->Name);

        double vacantGuarantee = 0;

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
                        return TError(EError::ResourceNotAvailable, "Numa node not found for " + ct->Name);
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
                    return TError(EError::ResourceNotAvailable, "Not enough cpus for " + ct->Name);

                if (!ct->CpuAffinity.IsEqual(affinity)) {
                    ct->CpuAffinity.Clear();
                    ct->CpuAffinity.Set(affinity);
                    ct->SetProp(EProperty::CPU_SET_AFFINITY);
                }

                if (ct->CpuReserve.Weight())
                    L_ACT("Reserve CPUs {} for {}", ct->CpuReserve.Format(), ct->Name);
                else
                    vacantGuarantee += std::max(ct->CpuGuarantee, ct->CpuGuaranteeSum);

                if (Verbose)
                    L("Assign CPUs {} for {}", ct->CpuAffinity.Format(), ct->Name);

                ct->CpuVacant.Set(ct->CpuAffinity);
            }
        }

        if (vacantGuarantee > parent->CpuVacant.Weight()) {
            if (!parent->CpuVacant.IsEqual(parent->CpuAffinity))
                return TError(EError::ResourceNotAvailable, "Not enough cpus for cpu_guarantee in " + parent->Name);
            L("CPU guarantee overcommit in {}", parent->Name);
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

    return TError::Success();
}

TError TContainer::PropagateCpuGuarantee() {
    if (!config().container().propagate_cpu_guarantee())
        return TError::Success();

    auto cpu_lock = LockCpuAffinity();
    TError error;

    CpuGuaranteeSum = 0;

    auto ct_lock = LockContainers();
    for (auto child: Children) {
        if (child->State == EContainerState::Running ||
                child->State == EContainerState::Meta ||
                child->State == EContainerState::Starting)
            CpuGuaranteeSum += std::max(child->CpuGuarantee,
                                        child->CpuGuaranteeSum);
    }
    ct_lock.unlock();

    auto cur = std::max(CpuGuarantee, CpuGuaranteeSum);
    if (!IsRoot() && (Controllers & CGROUP_CPU) && cur != CpuGuaranteeCur) {
        L_ACT("Propagate cpu guarantee CT{}:{} {}c -> {}c",
                Id, Name, CpuGuaranteeCur, cur);
        auto cpucg = GetCgroup(CpuSubsystem);
        error = CpuSubsystem.SetCpuLimit(cpucg, CpuPolicy, CpuWeight,
                                         CpuPeriod, cur, CpuLimit);
        if (error) {
            L_ERR("Cannot propagate cpu guarantee: {}", error);
            return error;
        }
        CpuGuaranteeCur = cur;
    }

    return TError::Success();
}

TError TContainer::ApplyDynamicProperties() {
    auto memcg = GetCgroup(MemorySubsystem);
    auto blkcg = GetCgroup(BlkioSubsystem);
    TError error;

    if (TestClearPropDirty(EProperty::MEM_GUARANTEE)) {
        error = MemorySubsystem.SetGuarantee(memcg, MemGuarantee);
        if (error) {
            if (error.GetErrno() != EINVAL)
                L_ERR("Can't set {}: {}", P_MEM_GUARANTEE, error);
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::MEM_LIMIT)) {
        error = MemorySubsystem.SetLimit(memcg, MemLimit);
        if (error) {
            if (error.GetErrno() == EBUSY)
                return TError(EError::InvalidValue, std::to_string(MemLimit) + " is too low");

            if (error.GetErrno() != EINVAL)
                L_ERR("Can't set {}: {}", P_MEM_LIMIT, error);
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::ANON_LIMIT)) {
        error = MemorySubsystem.SetAnonLimit(memcg, AnonMemLimit);
        if (error) {
            if (error.GetErrno() != EINVAL && error.GetErrno() != EBUSY)
                L_ERR("Can't set {}: {}", P_ANON_LIMIT, error);
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::DIRTY_LIMIT)) {
        error = MemorySubsystem.SetDirtyLimit(memcg, DirtyMemLimit);
        if (error) {
            if (error.GetErrno() != EINVAL)
                L_ERR("Can't set {}: {}", P_DIRTY_LIMIT, error);
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::RECHARGE_ON_PGFAULT)) {
        error = MemorySubsystem.RechargeOnPgfault(memcg, RechargeOnPgfault);
        if (error) {
            if (error.GetErrno() != EINVAL)
                L_ERR("Can't set {}: {}", P_RECHARGE_ON_PGFAULT, error);
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::PRESSURIZE_ON_DEATH)) {
        error = UpdateSoftLimit();
        if (error) {
            if (error.GetErrno() != EINVAL)
                L_ERR("Can't set {}: {}", P_PRESSURIZE_ON_DEATH, error);
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::IO_LIMIT)) {
        if (IoBpsLimit.count("fs")) {
            error = MemorySubsystem.SetIoLimit(memcg, IoBpsLimit["fs"]);
            if (error) {
                if (error.GetErrno() != EINVAL)
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
                if (error.GetErrno() != EINVAL)
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
                if (error.GetErrno() != EINVAL)
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
        error = HugetlbSubsystem.SetHugeLimit(cg, HugetlbLimit);
        if (error) {
            if (error.GetErrno() != EINVAL)
                L_ERR("Can't set {}: {}", P_HUGETLB_LIMIT, error);
            return error;
        }
        if (HugetlbSubsystem.SupportGigaPages()) {
            error = HugetlbSubsystem.SetGigaLimit(cg, 0);
            if (error)
                L_WRN("Cannot forbid 1GB pages: {}", error);
        }
    }

    if (TestPropDirty(EProperty::CPU_GUARANTEE)) {
        for (auto parent = Parent; parent; parent = parent->Parent) {
            error = parent->PropagateCpuGuarantee();
            if (error)
                return error;
        }
    }

    if ((Controllers & CGROUP_CPU) &&
            (TestPropDirty(EProperty::CPU_POLICY) |
             TestPropDirty(EProperty::CPU_WEIGHT) |
             TestClearPropDirty(EProperty::CPU_LIMIT) |
             TestClearPropDirty(EProperty::CPU_PERIOD) |
             TestClearPropDirty(EProperty::CPU_GUARANTEE))) {
        auto cpucg = GetCgroup(CpuSubsystem);
        error = CpuSubsystem.SetCpuLimit(cpucg, CpuPolicy, CpuWeight,
                CpuPeriod, std::max(CpuGuarantee, CpuGuaranteeSum), CpuLimit);
        if (error) {
            if (error.GetErrno() != EINVAL)
                L_ERR("Cannot set cpu policy: {}", error);
            return error;
        }
        CpuGuaranteeCur = std::max(CpuGuarantee, CpuGuaranteeSum);
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

    if (TestClearPropDirty(EProperty::NET_PRIO) |
        TestClearPropDirty(EProperty::NET_LIMIT) |
        TestClearPropDirty(EProperty::NET_GUARANTEE) |
        TestClearPropDirty(EProperty::NET_RX_LIMIT)) {
        if (Net) {
            error = Net->SetupClasses(NetClass);
            if (error)
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

    return TError::Success();
}

std::shared_ptr<TContainer> TContainer::FindRunningParent() const {
    auto p = Parent;
    while (p) {
        if (p->Task.Pid)
            return p;
        p = p->Parent;
    }

    return nullptr;
}

void TContainer::ShutdownOom() {
    if (Source)
        EpollLoop->RemoveSource(Source->Fd);
    Source = nullptr;
    PORTO_ASSERT(OomEvent.Fd < 0 || OomEvent.Fd > 2);
    OomEvent.Close();
}

TError TContainer::PrepareOomMonitor() {
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

TError TContainer::ConfigureDevices(std::vector<TDevice> &devices) {
    auto cg = GetCgroup(DevicesSubsystem);
    TDevice device;
    TError error;

    if (IsRoot())
        return TError::Success();

    if (Parent->IsRoot() && (Controllers & CGROUP_DEVICES)) {
        error = DevicesSubsystem.ApplyDefault(cg);
        if (error)
            return error;
    }

    for (auto &cfg: Devices) {
        error = device.Parse(cfg);
        if (error)
            return TError(error, "device: " + MergeEscapeStrings(cfg, ' '));

        error = device.Permitted(OwnerCred);
        if (error)
            return TError(error, "device: " + MergeEscapeStrings(cfg, ' '));

        error = DevicesSubsystem.ApplyDevice(cg, device);
        if (error)
            return TError(error, "device: " + MergeEscapeStrings(cfg, ' '));

        devices.push_back(device);
    }

    TMultiTuple extra = SplitEscapedString(config().container().extra_devices(), ' ', ';');
    for (auto &cfg: extra) {
        error = device.Parse(cfg);
        if (error)
            return TError(error, "device: " + MergeEscapeStrings(cfg, ' '));

        bool found = false;
        for (auto &dev: devices)
            found |= dev.Name == device.Name;
        if (found)
            continue;

        if (Level == 1) {
            error = DevicesSubsystem.ApplyDevice(cg, device);
            if (error)
                return TError(error, "device: " + MergeEscapeStrings(cfg, ' '));
        }

        if (!RootPath.IsRoot())
            devices.push_back(device);
    }

    return TError::Success();
}

TError TContainer::PrepareCgroups() {
    TError error;

    if (!HasProp(EProperty::CPU_SET) && Parent) {
        auto lock = LockCpuAffinity();

        /* Create CPU set if some CPUs in parent are reserved */
        if (!Parent->CpuAffinity.IsEqual(Parent->CpuVacant)) {
            Controllers |= CGROUP_CPUSET;
            RequiredControllers |= CGROUP_CPUSET;
            L("Enable cpuset for {} because parent has reserved cpus", Name);
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

    if (VirtMode == VIRT_MODE_OS &&
            config().container().detect_systemd() &&
            SystemdSubsystem.Supported &&
            !(Controllers & CGROUP_SYSTEMD) &&
            !RootPath.IsRoot()) {
        TPath cmd = RootPath / Command;
        TPath dst;
        if (!cmd.ReadLink(dst) && dst.BaseName() == "systemd") {
            L("Enable systemd cgroup for {}", Name);
            Controllers |= CGROUP_SYSTEMD;
        }
    }

    auto missing = Controllers;

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

        error = PrepareOomMonitor();
        if (error) {
            L_ERR("Can't prepare OOM monitoring: {}", error);
            return error;
        }
    }

    if (Controllers & CGROUP_NETCLS) {
        auto netcls = GetCgroup(NetclsSubsystem);
        error = netcls.Set("net_cls.classid", std::to_string(NetClass.Leaf));
        if (error) {
            L_ERR("Can't set classid: {}", error);
            return error;
        }
    }

    error = UpdateSoftLimit();
    if (error) {
        L_ERR("Cannot update memory soft limit: {}", error);
        return error;
    }

    return TError::Success();
}

TError TContainer::GetEnvironment(TEnv &env) {
    env.ClearEnv();

    env.SetEnv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
    env.SetEnv("HOME", GetCwd().ToString());
    env.SetEnv("USER", TaskCred.User());

    env.SetEnv("container", "lxc");

    /* lock these */
    env.SetEnv("PORTO_NAME", Name, true, true);
    env.SetEnv("PORTO_HOST", GetHostName(), true, true);
    env.SetEnv("PORTO_USER", OwnerCred.User(), true, true);

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

    return TError::Success();
}

TError TContainer::PrepareTask(TTaskEnv &TaskEnv) {
    auto parent = FindRunningParent();
    TError error;

    TaskEnv.CT = shared_from_this();
    TaskEnv.Client = CL;

    for (auto hy: Hierarchies)
        TaskEnv.Cgroups.push_back(GetCgroup(*hy));

    TaskEnv.Mnt.Cwd = GetCwd();

    if (RootVolume)
        TaskEnv.Mnt.Root = Parent->RootPath.InnerPath(RootVolume->Path);
    else
        TaskEnv.Mnt.Root = Root;

    TaskEnv.Mnt.RootRo = RootRo;

    TaskEnv.Mnt.RunSize = (GetTotalMemLimit() ?: GetTotalMemory()) / 2;

    TaskEnv.Mnt.BindCred = Parent->RootPath.IsRoot() ? CL->TaskCred : TCred(RootUser, RootGroup);

    if (Controllers & CGROUP_SYSTEMD)
        TaskEnv.Mnt.Systemd = GetCgroup(SystemdSubsystem).Name;

    TaskEnv.Cred = TaskCred;

    TaskEnv.LoginUid = (VirtMode == VIRT_MODE_APP) ? OwnerCred.Uid : -1;

    error = GetEnvironment(TaskEnv.Env);
    if (error)
        return error;

    TaskEnv.TripleFork = false;
    TaskEnv.QuadroFork = (VirtMode == VIRT_MODE_APP) && !IsMeta();

    TaskEnv.Mnt.BindMounts = BindMounts;

    /* legacy kludge */
    if (BindDns && !TaskEnv.Mnt.Root.IsRoot()) {
        TBindMount bm;
        bm.Source = "/etc/hosts";
        bm.Target = "/etc/hosts";
        bm.ReadOnly = true;
        TaskEnv.Mnt.BindMounts.push_back(bm);
    }

    /* Resolve paths in parent namespace and check volume ownership */
    for (auto &bm: TaskEnv.Mnt.BindMounts) {
        if (!bm.Source.IsAbsolute())
            bm.Source = Parent->GetCwd() / bm.Source;

        auto src = TVolume::Locate(Parent->RootPath / bm.Source);
        bm.ControlSource = src && !CL->CanControl(src->VolumeOwner);

        if (bm.Target.IsAbsolute())
            bm.Target = TaskEnv.Mnt.Root / bm.Target;
        else
            bm.Target = TaskEnv.Mnt.Root / TaskEnv.Mnt.Cwd / bm.Target;

        auto dst = TVolume::Locate(Parent->RootPath / bm.Target);
        bm.ControlTarget = dst && !CL->CanControl(dst->VolumeOwner);
    }

    TaskEnv.Mnt.BindPortoSock = AccessLevel != EAccessLevel::None;

    error = ConfigureDevices(TaskEnv.Devices);
    if (error) {
        L_ERR("Cannot configure devices: {}", error);
        return error;
    }

    if (parent) {
        pid_t pid = parent->Task.Pid;

        error = TaskEnv.IpcFd.Open(pid, "ns/ipc");
        if (error)
            return error;

        error = TaskEnv.UtsFd.Open(pid, "ns/uts");
        if (error)
            return error;

        if (NetInherit) {
            error = TaskEnv.NetFd.Open(pid, "ns/net");
            if (error)
                return error;
        }

        error = TaskEnv.PidFd.Open(pid, "ns/pid");
        if (error)
            return error;

        error = TaskEnv.MntFd.Open(pid, "ns/mnt");
        if (error)
            return error;

        error = TaskEnv.RootFd.Open(pid, "root");
        if (error)
            return error;

        error = TaskEnv.CwdFd.Open(pid, "cwd");
        if (error)
            return error;

        /* one more fork for creating nested pid-namespace */
        if (Isolate && TaskEnv.PidFd.Inode() != TNamespaceFd::PidInode(getpid(), "ns/pid"))
            TaskEnv.TripleFork = true;
    }

    if (IsMeta() || TaskEnv.TripleFork || TaskEnv.QuadroFork) {
        TPath exe("/proc/self/exe");
        TPath path;
        TError error = exe.ReadLink(path);
        if (error)
            return error;
        path = path.DirName() / "portoinit";
        error = TaskEnv.PortoInit.OpenRead(path);
        if (error)
            return error;
    }

    // Create new mount namespaces if we have to make any changes
    TaskEnv.NewMountNs = Isolate || Parent->IsRoot() ||
                          TaskEnv.Mnt.BindMounts.size() ||
                          Hostname.size() ||
                          ResolvConf.size() ||
                          !TaskEnv.Mnt.Root.IsRoot() ||
                          TaskEnv.Mnt.RootRo ||
                          !TaskEnv.Mnt.Systemd.empty();

    return TError::Success();
}

void TContainer::SanitizeCapabilities() {
    if (OwnerCred.IsRootUser()) {
        if (HasProp(EProperty::CAPABILITIES))
            CapBound.Permitted = CapLimit.Permitted;
        else
            CapBound.Permitted = HostCapBound.Permitted;
        CapAllowed.Permitted = CapBound.Permitted;
    } else {
        bool chroot = false;
        bool pidns = false;
        bool memcg = false;
        bool netns = false;

        CapBound = HostCapBound;

        for (auto ct = this; ct; ct = ct->Parent.get()) {
            chroot |= ct->Root != "/";
            pidns |= ct->Isolate;
            memcg |= ct->MemLimit;
            netns |= ct->NetIsolate;

            if (ct->HasProp(EProperty::CAPABILITIES))
                CapBound.Permitted &= ct->CapLimit.Permitted;
        }

        TCapabilities remove;
        if (!pidns)
            remove.Permitted |= PidNsCapabilities.Permitted;
        if (!memcg)
            remove.Permitted |= MemCgCapabilities.Permitted;
        if (!netns)
            remove.Permitted |= NetNsCapabilities.Permitted;

        if (chroot) {
            CapBound.Permitted &= ChrootCapBound.Permitted & ~remove.Permitted;
            CapAllowed.Permitted = CapBound.Permitted;
        } else
            CapAllowed.Permitted = HostCapAllowed.Permitted &
                                   CapBound.Permitted & ~remove.Permitted;
    }

    if (!HasProp(EProperty::CAPABILITIES))
        CapLimit.Permitted = CapBound.Permitted;
}

TStringMap TContainer::GetUlimit() const {
    TStringMap map = Ulimit;
    for (auto p = Parent.get(); p; p = p->Parent.get()) {
        for (const auto &it: p->Ulimit) {
            if (map.find(it.first) == map.end())
                map[it.first] = it.second;
        }
    }
    return map;
}

TError TContainer::StartTask() {
    TTaskEnv TaskEnv;
    TError error;

    error = TNetwork::StartNetwork(*this, TaskEnv);
    if (error)
        return error;

    if (!IsRoot()) {
        /* After restart apply all set dynamic properties */
        memcpy(PropDirty, PropSet, sizeof(PropDirty));

        error = ApplyDynamicProperties();
        if (error)
            return error;
    }

    /* Meta container without namespaces don't need task */
    if (IsMeta() && !Isolate && NetInherit)
        return TError::Success();

    error = PrepareTask(TaskEnv);
    if (error)
        return error;

    error = TaskEnv.Start();

    /* Always report OOM stuation if any */
    if (error && RecvOomEvents()) {
        if (error)
            L("Start error: {}", error);
        return TError(EError::InvalidValue, ENOMEM, "OOM, memory limit too low");
    }

    return error;
}

TError TContainer::Start() {
    TError error;

    for (auto p = Parent; p && p->State == EContainerState::Stopped; p = p->Parent) {
        error = CL->WriteContainer(ROOT_PORTO_NAMESPACE + p->Name, p);
        if (error)
            return error;
    }

    if (State != EContainerState::Stopped)
        return TError(EError::InvalidState, "Cannot start, container is not stopped: " + Name);

    if (Parent) {

        /* Automatically start parent container */
        if (Parent->State == EContainerState::Stopped) {
            error = Parent->Start();
            if (error)
                return error;
        }

        if (Parent->State == EContainerState::Paused)
            return TError(EError::InvalidState, "Parent container is paused: " + Parent->Name);

        if (Parent->State != EContainerState::Running &&
                Parent->State != EContainerState::Meta)
            return TError(EError::InvalidState, "Parent container is not running: " + Parent->Name);

        auto cg = Parent->GetCgroup(FreezerSubsystem);
        if (FreezerSubsystem.IsFrozen(cg))
            return TError(EError::InvalidState, "Parent container is frozen");
    } else {
        error = DistributeCpus();
        if (error)
            return error;
    }

    /* Extra check */
    error = CL->CanControl(OwnerCred);
    if (error)
        return TError(error, "Cannot start container " + Name);

    /* Normalize root path */
    if (Parent) {
        TPath path(Root);

        path = path.NormalPath();
        if (path.IsDotDot())
            return TError(EError::Permission, "root path with ..");

        RootPath = Parent->RootPath / path;
    }

    if (Parent) {
        CT = this;
        for (auto &knob: ContainerProperties) {
            error = knob.second->Start();
            if (error)
                break;
        }
        CT = nullptr;
        if (error)
            return error;
    }

    (void)TaskCred.LoadGroups(TaskCred.User());

    SanitizeCapabilities();

    /* Check target task credentials */
    error = CL->CanControl(TaskCred);
    if (!error && !OwnerCred.IsMemberOf(TaskCred.Gid) && !CL->IsSuperUser()) {
        TCred cred;
        cred.Load(TaskCred.User());
        if (!cred.IsMemberOf(TaskCred.Gid))
            error = TError(EError::Permission, "Cannot control group " + TaskCred.Group());
    }

    /*
     * Allow any user:group in chroot
     * FIXME: non-racy chroot validation is impossible for now
     */
    if (error && !RootPath.IsRoot())
        error = TError::Success();

    /* Allow any user:group in sub-container if client can change uid/gid */
    if (error && CL->CanSetUidGid() && IsChildOf(*CL->ClientContainer))
        error = TError::Success();

    if (error)
        return TError(error, "Cannot start container " + Name);

    /* Even without capabilities user=root require chroot */
    if (RootPath.IsRoot() && TaskCred.IsRootUser() && !OwnerCred.IsRootUser())
        return TError(EError::Permission, "user=root requires chroot");

    if ((CapAmbient.Permitted & ~CapAllowed.Permitted) ||
            (CapAmbient.Permitted & ~CapBound.Permitted))
        return TError(EError::Permission, "Ambient capabilities out of bounds");

    /* Enforce place restictions */
    if (HasProp(EProperty::PLACE) && Parent) {
        for (auto &place: Place) {
            bool allowed = false;
            for (auto &pp: Parent->Place)
                allowed |= StringMatch(place, pp);
            if (!allowed)
                return TError(EError::Permission, "Place " + place + " is not allowed by parent container");
        }
    } else if (Parent) {
        Place = Parent->Place;
        if (Root != "/")
            Place = {PORTO_PLACE};
    }

    error = StartOne();
    if (error)
        Statistics->ContainersFailedStart++;

    return error;
}

TError TContainer::StartOne() {
    TError error;

    L_ACT("Start container CT{}:{}", Id, Name);

    SetState(EContainerState::Starting);

    StartTime = GetCurrentTimeMs();
    RealStartTime = time(nullptr);
    SetProp(EProperty::START_TIME);

    error = PrepareResources();
    if (error) {
        SetState(EContainerState::Stopped);
        return error;
    }

    CL->LockedContainer->DowngradeLock();

    error = StartTask();

    CL->LockedContainer->UpgradeLock();

    if (error) {
        (void)Terminate(0);
        SetState(EContainerState::Stopped);
        FreeResources();
        return error;
    }

    if (IsMeta())
        SetState(EContainerState::Meta);
    else
        SetState(EContainerState::Running);

    SetProp(EProperty::ROOT_PID);

    Statistics->ContainersStarted++;

    error = Save();
    if (error) {
        L_ERR("Cannot save state after start {}", error);
        (void)Reap(false);
    }

    return error;
}

TError TContainer::PrepareResources() {
    TError error;

    if (!IsRoot()) {
        TPath work = WorkPath();

        error = work.Mkdir(0755);
        if (!error) {
            error = work.Chown(TaskCred);

            if (error)
                (void)work.Rmdir();
        }

        if (error) {
            if (error.GetErrno() == ENOSPC || error.GetErrno() == EROFS)
                L("Cannot create working dir: {}", error);
            else
                L_ERR("Cannot create working dir: {}", error);

            return error;
        }
    }

    TNetwork::InitClass(*this);

    error = PrepareCgroups();
    if (error) {
        L_ERR("Can't prepare task cgroups: {}", error);
        FreeResources();
        return error;
    }

    if (HasProp(EProperty::ROOT) && RootPath.IsRegularFollow()) {
        TStringMap cfg;

        cfg[V_BACKEND] = "loop";
        cfg[V_STORAGE] = RootPath.ToString();
        cfg[V_READ_ONLY] = BoolToString(RootRo);
        cfg[V_CONTAINERS] = ROOT_PORTO_NAMESPACE + Name;

        error = TVolume::Create(cfg, RootVolume);
        if (error) {
            L_ERR("Cannot create root volume: {}", error);
            FreeResources();
            return error;
        }

        RootPath = RootVolume->Path;
    }

    return TError::Success();
}

/* Some resources are not required in dead state */
void TContainer::FreeRuntimeResources() {
    TError error;

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

    if (CpuGuarantee) {
        for (auto parent = Parent; parent; parent = parent->Parent)
            (void)parent->PropagateCpuGuarantee();
    }
}

void TContainer::FreeResources() {
    TError error;

    FreeRuntimeResources();

    TNetwork::StopNetwork(*this);

    if (IsRoot())
        return;

    for (auto hy: Hierarchies) {
        if (Controllers & hy->Controllers) {
            auto cg = GetCgroup(*hy);
            (void)cg.Remove(); //Logged inside
        }
    }

    /* Legacy non-volume root on loop device */
    if (LoopDev >= 0) {
        error = PutLoopDev(LoopDev);
        if (error)
            L_ERR("Can't put loop device {}: {}", LoopDev, error);
        LoopDev = -1;
        SetProp(EProperty::LOOP_DEV);

        TPath tmp = TPath(PORTO_WORKDIR) / std::to_string(Id);
        if (tmp.Exists()) {
            error = tmp.RemoveAll();
            if (error)
                L_ERR("Can't remove {}: {}", tmp, error);
        }
    }

    if (RootVolume) {
        RootVolume->UnlinkContainer(*this);
        RootVolume->Destroy();
        RootVolume = nullptr;
    }

    TPath work_path = WorkPath();
    if (work_path.Exists()) {
        error = work_path.RemoveAll();
        if (error)
            L_ERR("Cannot remove working dir: {}", error);
    }

    Stdout.Remove(*this);
    Stderr.Remove(*this);
}

TError TContainer::Kill(int sig) {
    if (State != EContainerState::Running)
        return TError(EError::InvalidState, "invalid container state ");

    L_ACT("Kill task {} in container CT{}:{}", Task.Pid, Id, Name);
    return Task.Kill(sig);
}

TError TContainer::Terminate(uint64_t deadline) {
    auto cg = GetCgroup(FreezerSubsystem);
    TError error;

    if (IsRoot())
        return TError(EError::Permission, "Cannot terminate root container");

    L_ACT("Terminate tasks in container CT{}:{}", Id, Name);

    if (!(Controllers & CGROUP_FREEZER)) {
        if (Task.Pid)
            return TError(EError::NotSupported, "Cannot terminate without freezer");
        return TError::Success();
    }

    if (cg.IsEmpty())
        return TError::Success();

    if (FreezerSubsystem.IsFrozen(cg))
        return cg.KillAll(SIGKILL);

    if (Task.Pid && deadline && State != EContainerState::Meta) {
        int sig = SIGTERM;

        if (Isolate && VirtMode == VIRT_MODE_OS) {
            uint64_t mask = TaskHandledSignals(Task.Pid);
            if (mask & BIT(SIGPWR - 1))
                sig = SIGPWR;
            else if (!(mask & BIT(SIGTERM - 1)))
                sig = 0;
        }

        if (sig) {
            error = Task.Kill(sig);
            if (!error) {
                L_ACT("Wait task {} after signal {} in CT{}:{}", Task.Pid, sig, Id, Name);
                while (Task.Exists() && !Task.IsZombie() &&
                        !WaitDeadline(deadline));
            }
        }
    }

    if (WaitTask.Pid && Isolate) {
        error = WaitTask.Kill(SIGKILL);
        if (error)
            return error;
    }

    if (cg.IsEmpty())
        return TError::Success();

    error = cg.KillAll(SIGKILL);
    if (error)
        return error;

    return TError::Success();
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
        return TError::Success();

    if (!(Controllers & CGROUP_FREEZER)) {
        if (Task.Pid)
            return TError(EError::NotSupported, "Cannot stop without freezer");
    } else if (FreezerSubsystem.IsParentFreezing(freezer))
            return TError(EError::InvalidState, "Parent container is paused");

    auto subtree = Subtree();

    DowngradeLock();

    for (auto &ct : subtree) {
        auto cg = ct->GetCgroup(FreezerSubsystem);

        if (ct->IsRoot() || ct->State == EContainerState::Stopped)
            continue;

        error = ct->Terminate(deadline);
        if (error) {
            L_ERR("Cannot terminate tasks in container CT{}:{}: {}", ct->Id, ct->Name, error);
            return error;
        }

        if (FreezerSubsystem.IsSelfFreezing(cg)) {
            L_ACT("Thaw terminated paused container CT{}:{}", ct->Id, ct->Name);
            error = FreezerSubsystem.Thaw(cg, false);
            if (error)
                return error;
        }
    }

    UpgradeLock();

    for (auto &ct: subtree) {
        if (ct->State == EContainerState::Stopped)
            continue;

        L_ACT("Stop container CT{}:{}", Id, Name);

        ct->ForgetPid();

        ct->DeathTime = 0;
        ct->ClearProp(EProperty::DEATH_TIME);

        ct->ExitStatus = 0;
        ct->ClearProp(EProperty::EXIT_STATUS);

        ct->OomEvents = 0;
        ct->OomKilled = false;
        ct->ClearProp(EProperty::OOM_KILLED);

        ct->SetState(EContainerState::Stopped);
        ct->FreeResources();

        error = Save();
        if (error)
            return error;
    }

    return TError::Success();
}

void TContainer::Reap(bool oomKilled) {
    TError error;

    error = Terminate(0);
    if (error)
        L_WRN("Cannot terminate container {} : {}", Name, error);

    DeathTime = GetCurrentTimeMs();
    SetProp(EProperty::DEATH_TIME);

    if (oomKilled) {
        OomKilled = oomKilled;
        SetProp(EProperty::OOM_KILLED);
    }

    ForgetPid();

    Stdout.Rotate(*this);
    Stderr.Rotate(*this);

    SetState(EContainerState::Dead);

    FreeRuntimeResources();

    error = Save();
    if (error)
        L_WRN("Cannot save container state after exit: {}", error);

    if (MayRespawn())
        ScheduleRespawn();
}

void TContainer::Exit(int status, bool oomKilled) {

    if (State == EContainerState::Stopped)
        return;

    /* SIGKILL could be delivered earlier than OOM event */
    if (!oomKilled && RecvOomEvents())
        oomKilled = true;

    /* Detect fatal signals: portoinit cannot kill itself */
    if (Isolate && VirtMode == VIRT_MODE_APP && WIFEXITED(status) &&
            WEXITSTATUS(status) > 128 && WEXITSTATUS(status) < 128 + SIGRTMIN)
        status = WEXITSTATUS(status) - 128;

    L_EVT("Exit {} {} {}", Name, FormatExitStatus(status),
          (oomKilled ? "invoked by OOM" : ""));

    ExitStatus = status;
    SetProp(EProperty::EXIT_STATUS);

    /* Detect memory shortage that happened in syscalls */
    auto cg = GetCgroup(MemorySubsystem);
    if (!oomKilled && OomIsFatal && MemorySubsystem.GetOomEvents(cg)) {
        L("Container {} hit memory limit", Name);
        oomKilled = true;
    }

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
            error = ct->Save();
            if (error)
                L_ERR("Cannot save state after pause: {}", error);
        }
    }

    return TError::Success();
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
        if (ct->State == EContainerState::Paused)
            ct->SetState(IsMeta() ? EContainerState::Meta : EContainerState::Running);
        error = ct->Save();
        if (error)
            L_ERR("Cannot save state after resume: {}", error);
    }

    return TError::Success();
}

void TContainer::SyncProperty(const std::string &name) {
    if (StringStartsWith(name, "net_") && Net)
        Net->SyncStat();
}

void TContainer::SyncPropertiesAll() {
    TNetwork::SyncAllStat();
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
            if (State == EContainerState::Stopped)
                return TError(EError::InvalidState, "Not available in stopped state");
            std::string type = property.substr(0, dot);
            for (auto subsys: Subsystems) {
                if (subsys->Type != type)
                    continue;
                if (subsys->Kind & Controllers)
                    return TError::Success();
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

    if (!prop->IsSupported)
        return TError(EError::NotSupported, "Not supported: " + property);

    CT = const_cast<TContainer *>(this);
    if (idx.length())
        error = prop->GetIndexed(idx, value);
    else
        error = prop->Get(value);
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
    if (it == ContainerProperties.end())
        return TError(EError::InvalidProperty, "Invalid property " + property);
    auto prop = it->second;

    if (!prop->IsSupported)
        return TError(EError::NotSupported, property + " is not supported");

    CT = this;

    std::string oldValue;
    error = prop->Get(oldValue);

    if (!error) {
        if (idx.length())
            error = prop->SetIndexed(idx, value);
        else
            error = prop->Set(value);
    }

    if (!error && (State == EContainerState::Running ||
                   State == EContainerState::Meta ||
                   State == EContainerState::Paused)) {
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

    /* These are not properties */
    node.Set(P_RAW_ID, std::to_string(Id));
    node.Set(P_RAW_NAME, Name);

    CT = this;

    for (auto knob : ContainerProperties) {
        std::string value;

        /* Skip knobs without a value */
        if (knob.second->Prop == EProperty::NONE || !HasProp(knob.second->Prop))
            continue;

        error = knob.second->GetToSave(value);
        if (error)
            break;

        node.Set(knob.first, value);
    }

    CT = nullptr;

    if (error)
        return error;

    return node.Save();
}

TError TContainer::Load(const TKeyValue &node) {
    EContainerState state = EContainerState::Destroyed;
    TError error;

    CT = this;

    OwnerCred = CL->Cred;

    for (auto &kv: node.Data) {
        std::string key = kv.first;
        std::string value = kv.second;

        if (key == D_STATE) {
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

        error = prop->SetFromRestore(value);
        if (error) {
            L_ERR("Cannot load {} : {}", key, error);
            state = EContainerState::Dead;
            break;
        }

        SetProp(prop->Prop);
    }

    if (state != EContainerState::Destroyed) {
        SetState(state);
        SetProp(EProperty::STATE);
    } else
        error = TError(EError::Unknown, "Container has no state");

    if (!node.Has(P_CONTROLLERS) && State != EContainerState::Stopped)
        Controllers = RootContainer->Controllers;

    if (Level == 1 && CpusetSubsystem.Supported &&
            !(Controllers & CGROUP_CPUSET))
        Controllers |= CGROUP_CPUSET;

    if (!node.Has(P_OWNER_USER) || !node.Has(P_OWNER_GROUP))
        OwnerCred = TaskCred;

    SanitizeCapabilities();

    if (state == EContainerState::Running) {
        auto now = GetCurrentTimeMs();

        if (!HasProp(EProperty::START_TIME)) {
            StartTime = now;
            SetProp(EProperty::START_TIME);
        }

        RealStartTime = time(nullptr) - (now - StartTime) / 1000;
    }

    CT = nullptr;

    return error;
}

TError TContainer::Seize() {
    if (SeizeTask.Pid) {
        if (GetTaskName(SeizeTask.Pid) == "portoinit") {
            pid_t ppid = SeizeTask.GetPPid();
            if (ppid == getpid() || ppid == getppid())
                return TError::Success();
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
    TPath exe("/proc/self/exe");
    TPath path;
    TError error;
    error = exe.ReadLink(path);
    if (error)
        return error;
    path = path.DirName() / "portoinit";
    auto cg = GetCgroup(FreezerSubsystem);

    error = SeizeTask.Fork(true);
    if (error)
        return error;

    if (SeizeTask.Pid) {
        SetProp(EProperty::SEIZE_PID);
        return TError::Success();
    }

    if (cg.Attach(GetPid()))
        _exit(EXIT_FAILURE);
    execv(path.c_str(), (char *const *)argv);
    _exit(EXIT_FAILURE);
}

void TContainer::SyncState() {
    TCgroup taskCg, freezerCg = GetCgroup(FreezerSubsystem);
    TError error;

    L_ACT("Sync container CT{}:{} state {}", Id, Name, StateName(State));

    if (!freezerCg.Exists()) {
        if (State != EContainerState::Stopped)
            L_WRN("Freezer not found");
        ForgetPid();
        State = EContainerState::Stopped;
        return;
    }

    if (State == EContainerState::Starting)
        State = IsMeta() ? EContainerState::Meta : EContainerState::Running;

    if (FreezerSubsystem.IsFrozen(freezerCg)) {
        if (State != EContainerState::Paused)
            FreezerSubsystem.Thaw(freezerCg);
    } else if (State == EContainerState::Paused)
        State = IsMeta() ? EContainerState::Meta : EContainerState::Running;

    if (State == EContainerState::Stopped) {
        L("Found unexpected freezer");
        Reap(false);
    } else if (State == EContainerState::Meta && !WaitTask.Pid && !Isolate) {
        /* meta container */
    } else if (!WaitTask.Exists()) {
        if (State != EContainerState::Dead)
            L("Task no found");
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
            /* Any state is ok */
            break;
        case EContainerState::Paused:
            if (State == EContainerState::Running || State == EContainerState::Meta)
                State = EContainerState::Paused;
            break;
        case EContainerState::Destroyed:
            L_ERR("Destroyed parent?");
            break;
    }
}

TError TContainer::SyncCgroups() {
    TError error;

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

    if (subsystem.Controllers & CGROUP_FREEZER)
        return subsystem.Cgroup(std::string(PORTO_CGROUP_PREFIX) + "/" + Name);

    if (subsystem.Controllers & CGROUP_SYSTEMD) {
        if (Controllers & CGROUP_SYSTEMD)
            return subsystem.Cgroup(std::string(PORTO_CGROUP_PREFIX) + "%" +
                                    StringReplaceAll(Name, "/", "%"));
        return subsystem.RootCgroup();
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

bool TContainer::MayRespawn() {
    if (State != EContainerState::Dead)
        return false;

    if (!ToRespawn)
        return false;

    if (Parent->State != EContainerState::Running &&
        Parent->State != EContainerState::Meta) {

        /*FIXME: respawn for hierarchies is broken */

        return false;
    }

    return MaxRespawns < 0 || RespawnCount < (uint64_t)MaxRespawns;
}

bool TContainer::RecvOomEvents() {
    uint64_t val;

    if (OomEvent.Fd >= 0 &&
            read(OomEvent.Fd, &val, sizeof(val)) == sizeof(val) &&
            val) {
        OomEvents += val;
        Statistics->ContainersOOM += val;
        L_EVT("OOM in {}", Name);
        return true;
    }

    return false;
}

void TContainer::ScheduleRespawn() {
    TEvent e(EEventType::Respawn, shared_from_this());
    EventQueue->Add(config().container().respawn_delay_ms(), e);
}

TError TContainer::Respawn() {
    TError error;

    error = Stop(config().container().kill_timeout_ms());
    if (error)
        return error;

    RespawnCount++;
    SetProp(EProperty::RESPAWN_COUNT);

    // FIXME
    CL->LockedContainer = shared_from_this();
    error = Start();
    CL->LockedContainer = nullptr;

    return error;
}

void TContainer::Event(const TEvent &event) {
    TError error;

    auto lock = LockContainers();
    auto ct = event.Container.lock();

    switch (event.Type) {
    case EEventType::OOM:
    {
        if (ct) {
            error = ct->Lock(lock);
            lock.unlock();
            if (!error) {
                if (ct->OomIsFatal)
                    ct->Exit(SIGKILL, true);
                ct->Unlock();
            }
        }
        break;
    }
    case EEventType::Respawn:
    {
        if (ct && ct->MayRespawn()) {
            error = ct->Lock(lock);
            lock.unlock();
            if (!error) {
                if (ct->MayRespawn())
                    ct->Respawn();
                ct->Unlock();
            }
        }
        break;
    }
    case EEventType::Exit:
    case EEventType::ChildExit:
    {
        bool delivered = false;
        for (auto &it: Containers) {
            auto ct = it.second;
            if (ct->WaitTask.Pid != event.Exit.Pid &&
                    ct->SeizeTask.Pid != event.Exit.Pid)
                continue;
            error = ct->Lock(lock);
            lock.unlock();
            if (!error) {
                if (ct->WaitTask.Pid == event.Exit.Pid ||
                        ct->SeizeTask.Pid == event.Exit.Pid) {
                    ct->Exit(event.Exit.Status, false);
                    delivered = true;
                }
                ct->Unlock();
            }
            break;
        }
        if (event.Type == EEventType::Exit)
            AckExitStatus(event.Exit.Pid);
        else {
            if (!delivered)
                L("Unknown zombie {} {}", event.Exit.Pid, event.Exit.Status);
            (void)waitpid(event.Exit.Pid, NULL, 0);
        }
        break;
    }
    case EEventType::WaitTimeout:
    {
        auto w = event.WaitTimeout.Waiter.lock();
        if (w)
            w->WakeupWaiter(nullptr);
        break;
    }

    case EEventType::DestroyAgedContainer:
        if (ct) {
            error = ct->Lock(lock);
            lock.unlock();
            if (!error) {
                if (ct->State == EContainerState::Dead &&
                        GetCurrentTimeMs() >= ct->DeathTime + ct->AgingTime) {
                    Statistics->RemoveDead++;
                    ct->Destroy();
                }
                ct->Unlock();
            }
        }
        break;

    case EEventType::DestroyWeakContainer:
        if (ct) {
            error = ct->Lock(lock);
            lock.unlock();
            if (!error) {
                if (ct->IsWeak)
                    ct->Destroy();
                ct->Unlock();
            }
        }
        break;

    case EEventType::RotateLogs:
        lock.unlock();
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
        EventQueue->Add(config().daemon().log_rotate_ms(), event);
        break;
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

void TContainer::AddWaiter(std::shared_ptr<TContainerWaiter> waiter) {
    CleanupWaiters();
    Waiters.push_back(waiter);
}

void TContainer::NotifyWaiters() {
    CleanupWaiters();
    for (auto &w : Waiters) {
        auto waiter = w.lock();
        if (waiter)
            waiter->WakeupWaiter(this);
    }
    if (!IsRoot())
        TContainerWaiter::WakeupWildcard(this);
}

void TContainer::CleanupWaiters() {
    for (auto iter = Waiters.begin(); iter != Waiters.end();) {
        if (iter->expired()) {
            iter = Waiters.erase(iter);
            continue;
        }
        iter++;
    }
}

TContainerWaiter::TContainerWaiter(std::shared_ptr<TClient> client) : Client(client) { }

void TContainerWaiter::WakeupWaiter(const TContainer *who, bool wildcard) {
    std::shared_ptr<TClient> client = Client.lock();
    if (client) {
        std::string name;

        if (who && client->ComposeName(who->Name, name))
            return;

        if (who && wildcard && !MatchWildcard(name))
            return;

        SendWaitResponse(*client, name);

        Client.reset();
        client->Waiter = nullptr;
    }
}

std::mutex TContainerWaiter::WildcardLock;
std::list<std::weak_ptr<TContainerWaiter>> TContainerWaiter::WildcardWaiters;

void TContainerWaiter::WakeupWildcard(const TContainer *who) {
    WildcardLock.lock();
    for (auto &w : WildcardWaiters) {
        auto waiter = w.lock();
        if (waiter)
            waiter->WakeupWaiter(who, true);
    }
    WildcardLock.unlock();
}

void TContainerWaiter::AddWildcard(std::shared_ptr<TContainerWaiter> &waiter) {
    WildcardLock.lock();
    for (auto iter = WildcardWaiters.begin(); iter != WildcardWaiters.end();) {
        if (iter->expired()) {
            iter = WildcardWaiters.erase(iter);
            continue;
        }
        iter++;
    }
    WildcardWaiters.push_back(waiter);
    WildcardLock.unlock();
}

bool TContainerWaiter::MatchWildcard(const std::string &name) {
    for (const auto &wildcard: Wildcards)
        if (StringMatch(name, wildcard))
            return true;
    return false;
}
