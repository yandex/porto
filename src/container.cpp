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
#include "util/loop.hpp"
#include "client.hpp"
#include "filesystem.hpp"

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
    if (Verbose)
        L() << (try_lock ? "TryLock " : "Lock ")
            << (for_read ? "read " : "write ") << Name << std::endl;
    while (1) {
        if (State == EContainerState::Destroyed) {
            if (Verbose)
                L() << "Lock failed, container was destroyed: " << Name << std::endl;
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
            if (Verbose)
                L() << "TryLock " << (for_read ? "read" : "write") << " Failed" << Name << std::endl;
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

    if (Verbose)
        L() << "Downgrading write to read " << Name << std::endl;

    for (auto ct = Parent.get(); ct; ct = ct->Parent.get()) {
        ct->SubtreeRead++;
        ct->SubtreeWrite--;
    }

    Locked = 1;
    ContainersCV.notify_all();
}

void TContainer::UpgradeLock() {
    auto lock = LockContainers();

    if (Verbose)
        L() << "Upgrading read back to write " << Name << std::endl;

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
    if (Verbose)
        L() << "Unlock " << (Locked > 0 ? "read " : "write ") << Name << std::endl;
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
            L() << ct->Name << " Locked " << ct->Locked << " by " << ct->LastOwner
                << " Read " << ct->SubtreeRead << " Write " << ct->SubtreeWrite
                << (ct->PendingWrite ? " PendingWrite" : "") << std::endl;
    }
}

void TContainer::Register() {
    PORTO_LOCKED(ContainersMutex);
    Containers[Name] = shared_from_this();
    if (Parent)
        Parent->Children.emplace_back(shared_from_this());
    Statistics->ContainersCreated++;
}

TContainer::TContainer(std::shared_ptr<TContainer> parent, const std::string &name) :
    Parent(parent), Name(name),
    FirstName(!parent ? "" : parent->IsRoot() ? name : name.substr(parent->Name.length() + 1)),
    Level(parent ? parent->Level + 1 : 0),
    Stdin(0), Stdout(1), Stderr(2),
    ClientsCount(0), ContainerRequests(0)
{
    Statistics->ContainersCount++;
    RealCreationTime = time(nullptr);

    std::fill(PropSet, PropSet + sizeof(PropSet), false);
    std::fill(PropDirty, PropDirty + sizeof(PropDirty), false);

    if (IsRoot())
        Cwd = "/";
    else
        Cwd = WorkPath().ToString();

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
    BindDns = true;
    VirtMode = VIRT_MODE_APP;
    NetProp = { { "inherited" } };
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

    SchedPolicy = SCHED_OTHER;
    SchedPrio = 0;
    SchedNice = 0;

    CpuLimit = GetNumCores();
    CpuGuarantee = IsRoot() ? GetNumCores() : 0;
    IoPolicy = "normal";

    Controllers = RequiredControllers = CGROUP_FREEZER;
    if (CpuacctSubsystem.Controllers == CGROUP_CPUACCT)
        Controllers |= CGROUP_CPUACCT;
    if (!Parent || Parent->IsRoot() || config().container().all_controllers())
        Controllers |= CGROUP_MEMORY | CGROUP_CPU | CGROUP_CPUACCT |
                       CGROUP_NETCLS | CGROUP_BLKIO | CGROUP_DEVICES;
    SetProp(EProperty::CONTROLLERS);

    if ((Controllers & CGROUP_MEMORY) && HugetlbSubsystem.Supported)
        Controllers |= CGROUP_HUGETLB;

    if (Parent && Parent->IsRoot() && PidsSubsystem.Supported)
        Controllers |= CGROUP_PIDS;

    NetPriority["default"] = NET_DEFAULT_PRIO;
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
    // so call them explicitly in Tcontainer::Destroy()
    PORTO_ASSERT(Net == nullptr);
    Statistics->ContainersCount--;
}

TError TContainer::Create(const std::string &name, std::shared_ptr<TContainer> &ct) {
    auto nrMax = config().container().max_total();
    TError error;

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

    L_ACT() << "Create " << name << std::endl;

    ct = std::make_shared<TContainer>(parent, name);

    error = ContainerIdMap.Get(ct->Id);
    if (error)
        goto err;

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
    if (ct && ct->Id)
        ContainerIdMap.Put(ct->Id);
    ct = nullptr;
    return error;
}

TError TContainer::Restore(const TKeyValue &kv, std::shared_ptr<TContainer> &ct) {
    TError error;
    int id;

    error = StringToInt(kv.Get(P_RAW_ID), id);
    if (error)
        return error;

    L_ACT() << "Restore container " << kv.Name << std::endl;

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

    ct = std::make_shared<TContainer>(parent, kv.Name);

    lock.unlock();

    error = ct->Load(kv);
    if (error)
        goto err;

    ct->Id = id;
    ct->RootPath = parent->RootPath / ct->Root;

    /* SyncState might stop container, take lock for it */
    error = SystemClient.LockContainer(ct);
    if (error)
        return error;

    ct->SyncState();

    SystemClient.ReleaseContainer();

    if (ct->Task.Pid) {
        error = ct->RestoreNetwork();
        if (error && !ct->WaitTask.IsZombie()) {
            L_WRN() << "Cannot restore network: " << error << std::endl;
            ct->Reap(false);
        }
    }

    /* Restore cgroups only for running containers */
    if (ct->State != EContainerState::Stopped &&
            ct->State != EContainerState::Dead) {

        error = ct->PrepareCgroups();
        if (error)
            goto err;

        /* Kernel without group rt forbids moving RT tasks in to cpu cgroup */
        if (ct->Task.Pid && (!CpuSubsystem.HasRtGroup || CpuSubsystem.HasSmart)) {
            auto cpuCg = ct->GetCgroup(CpuSubsystem);
            TCgroup cg;
            bool smart;

            if (!CpuSubsystem.TaskCgroup(ct->Task.Pid, cg) && cg != cpuCg) {
                auto freezerCg = ct->GetCgroup(FreezerSubsystem);

                /* Disable smart if we're moving tasks into another cgroup */
                if (CpuSubsystem.HasSmart && !cg.GetBool("cpu.smart", smart) && smart) {
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
                        L_WRN() << "Cannot move to corrent cpu cgroup: " << error << std::endl;
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

    lock.lock();
    ct->Register();
    return TError::Success();

err:
    ct->Net = nullptr;
    ContainerIdMap.Put(id);
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
    return TPath(PORTO_WORKDIR) / Name;
}

std::string TContainer::GetCwd() const {
    for (auto ct = shared_from_this(); ct; ct = ct->Parent) {
        if (ct->HasProp(EProperty::CWD))
            return ct->Cwd;
        if (ct->Root != "/")
            return "/";
    }
    return Cwd;
}

TError TContainer::GetNetStat(ENetStat kind, TUintMap &stat) {
    if (Net) {
        auto lock = Net->ScopedLock();
        return Net->GetTrafficStat(ContainerTC, kind, stat);
    } else
        return TError(EError::NotSupported, "Network statistics is not available");
}

TError TContainer::UpdateSoftLimit() {
    if (IsRoot())
        return TError::Success();

    if (Parent)
        Parent->UpdateSoftLimit();

    if (State == EContainerState::Meta) {
        uint64_t defaultLimit;

        auto rootCg = MemorySubsystem.RootCgroup();
        TError error = MemorySubsystem.GetSoftLimit(rootCg, defaultLimit);
        if (error)
            return error;

        uint64_t limit = RunningChildren ? defaultLimit : 1 * 1024 * 1024;
        uint64_t currentLimit;

        auto cg = GetCgroup(MemorySubsystem);
        error = MemorySubsystem.GetSoftLimit(cg, currentLimit);
        if (error)
            return error;

        if (currentLimit != limit) {
            error = MemorySubsystem.SetSoftLimit(cg, limit);
            if (error)
                return error;
        }
    }

    return TError::Success();
}

void TContainer::SetState(EContainerState next) {
    if (State == next)
        return;

    L_ACT() << Name << ": change state " << StateName(State) << " -> " << StateName(next) << std::endl;

    auto lock = LockContainers();
    auto prev = State;
    State = next;

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

    L_ACT() << "Destroy " << Name << std::endl;

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

    while (!Volumes.empty()) {
        std::shared_ptr<TVolume> volume = Volumes.back();
        if (!volume->UnlinkContainer(*this) && volume->IsDying)
            volume->Destroy();
    }

    if (Net) {
        auto lock = Net->ScopedLock();
        Net = nullptr;
    }

    auto lock = LockContainers();

    error = ContainerIdMap.Put(Id);
    if (error)
        L_WRN() << "Cannot put container id : " << error << std::endl;

    Containers.erase(Name);
    if (Parent)
        Parent->Children.remove(shared_from_this());
    State = EContainerState::Destroyed;

    TPath path(ContainersKV / std::to_string(Id));
    error = path.Unlink();
    if (error)
        L_ERR() << "Can't remove key-value node " << path << ": " << error << std::endl;

    return TError::Success();
}

bool TContainer::IsChildOf(const TContainer &ct) const {
    for (auto ptr = Parent.get(); ptr; ptr = ptr->Parent.get()) {
        if (ptr == &ct)
            return true;
    }
    return false;
}

std::list<std::shared_ptr<TContainer>> TContainer::Subtree() {
    std::list<std::shared_ptr<TContainer>> subtree {shared_from_this()};
    auto lock = LockContainers();
    for (auto it = subtree.rbegin(); it != subtree.rend(); ++it) {
        for (auto &child: (*it)->Children)
            subtree.emplace_front(child);
    }
    return subtree;
}

std::shared_ptr<TContainer> TContainer::GetParent() const {
    return Parent;
}

TError TContainer::GetPidFor(pid_t pidns, pid_t &pid) const {
    TError error;

    if (IsRoot()) {
        pid = 1;
    } else if (!Task.Pid) {
        error = TError(EError::InvalidState, "container isn't running");
    } else if (InPidNamespace(pidns, getpid())) {
        pid = Task.Pid;
    } else if (WaitTask.Pid != Task.Pid && InPidNamespace(pidns, WaitTask.Pid)) {
        pid = TaskVPid;
    } else if (InPidNamespace(pidns, Task.Pid)) {
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

TError TContainer::OpenNetns(TNamespaceFd &netns) const {
    if (Task.Pid)
        return netns.Open(Task.Pid, "ns/net");
    if (Net == HostNetwork)
        return netns.Open(GetTid(), "ns/net");
    return TError(EError::InvalidValue, "Cannot open netns: container not running");
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

    L_ACT() << "Apply ulimits" << std::endl;
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

TError TContainer::ApplySchedPolicy() const {
    auto cg = GetCgroup(FreezerSubsystem);
    struct sched_param param;
    param.sched_priority = SchedPrio;
    TError error;

    std::vector<pid_t> prev, pids;
    bool retry;

    L_ACT() << "Set " << cg << " scheduler policy " << CpuPolicy << std::endl;
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

TError TContainer::ApplyDynamicProperties() {
    auto memcg = GetCgroup(MemorySubsystem);
    auto blkcg = GetCgroup(BlkioSubsystem);
    TError error;

    if (TestClearPropDirty(EProperty::MEM_GUARANTEE)) {
        error = MemorySubsystem.SetGuarantee(memcg, MemGuarantee);
        if (error) {
            if (error.GetErrno() != EINVAL)
                L_ERR() << "Can't set " << P_MEM_GUARANTEE << ": " << error << std::endl;
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::MEM_LIMIT)) {
        error = MemorySubsystem.SetLimit(memcg, MemLimit);
        if (error) {
            if (error.GetErrno() == EBUSY)
                return TError(EError::InvalidValue, std::to_string(MemLimit) + " is too low");

            if (error.GetErrno() != EINVAL)
                L_ERR() << "Can't set " << P_MEM_LIMIT << ": " << error << std::endl;
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::ANON_LIMIT)) {
        error = MemorySubsystem.SetAnonLimit(memcg, AnonMemLimit);
        if (error) {
            if (error.GetErrno() != EINVAL && error.GetErrno() != EBUSY)
                L_ERR() << "Can't set " << P_ANON_LIMIT << ": " << error << std::endl;
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::DIRTY_LIMIT)) {
        error = MemorySubsystem.SetDirtyLimit(memcg, DirtyMemLimit);
        if (error) {
            if (error.GetErrno() != EINVAL)
                L_ERR() << "Can't set " << P_DIRTY_LIMIT << ": " << error << std::endl;
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::RECHARGE_ON_PGFAULT)) {
        error = MemorySubsystem.RechargeOnPgfault(memcg, RechargeOnPgfault);
        if (error) {
            if (error.GetErrno() != EINVAL)
                L_ERR() << "Can't set " << P_RECHARGE_ON_PGFAULT << ": " << error << std::endl;
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::IO_LIMIT)) {
        if (IoBpsLimit.count("fs")) {
            error = MemorySubsystem.SetIoLimit(memcg, IoBpsLimit["fs"]);
            if (error) {
                if (error.GetErrno() != EINVAL)
                    L_ERR() << "Can't set " << P_IO_LIMIT << ": " << error << std::endl;
                return error;
            }
        }
        error = BlkioSubsystem.SetIoLimit(blkcg, IoBpsLimit);
        if (error)
            return error;
    }

    if (TestClearPropDirty(EProperty::IO_OPS_LIMIT)) {
        if (IoOpsLimit.count("fs")) {
            error = MemorySubsystem.SetIopsLimit(memcg, IoOpsLimit["fs"]);
            if (error) {
                if (error.GetErrno() != EINVAL)
                    L_ERR() << "Can't set " << P_IO_OPS_LIMIT << ": " << error << std::endl;
                return error;
            }
        }
        error = BlkioSubsystem.SetIoLimit(blkcg, IoOpsLimit, true);
        if (error)
            return error;
    }

    if (TestClearPropDirty(EProperty::IO_POLICY)) {
        error = BlkioSubsystem.SetIoPolicy(blkcg, IoPolicy);
        if (error) {
            if (error.GetErrno() != EINVAL)
                L_ERR() << "Can't set " << P_IO_POLICY << ": " << error << std::endl;
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::HUGETLB_LIMIT)) {
        auto cg = GetCgroup(HugetlbSubsystem);
        error = HugetlbSubsystem.SetHugeLimit(cg, HugetlbLimit);
        if (error) {
            if (error.GetErrno() != EINVAL)
                L_ERR() << "Cannot set " << P_HUGETLB_LIMIT << ": " << error << std::endl;
            return error;
        }
        if (HugetlbSubsystem.SupportGigaPages()) {
            error = HugetlbSubsystem.SetGigaLimit(cg, 0);
            if (error)
                L_WRN() << "Cannot forbid 1GB pages: " << error << std::endl;
        }
    }

    if ((Controllers & CGROUP_CPU) &&
            (TestPropDirty(EProperty::CPU_POLICY) |
             TestClearPropDirty(EProperty::CPU_LIMIT) |
             TestClearPropDirty(EProperty::CPU_GUARANTEE))) {
        auto cpucg = GetCgroup(CpuSubsystem);
        error = CpuSubsystem.SetCpuLimit(cpucg, CpuPolicy,
                                          CpuGuarantee, CpuLimit);
        if (error) {
            if (error.GetErrno() != EINVAL)
                L_ERR() << "Cannot set cpu policy: " << error << std::endl;
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::CPU_POLICY)) {
        error = ApplySchedPolicy();
        if (error) {
            L_ERR() << "Cannot set scheduler policy: " << error << std::endl;
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::CPU_SET)) {
        auto cg = GetCgroup(CpusetSubsystem);
        error = CpusetSubsystem.SetCpus(cg, CpuSet);
        if (error) {
            L() << "Cannot set cpuset " << CpuSet << " : " << error << std::endl;
            return error;
        }
        error = CpusetSubsystem.SetMems(cg, "");
        if (error) {
            L() << "Cannot set mems: " << error << std::endl;
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::NET_PRIO) |
            TestClearPropDirty(EProperty::NET_LIMIT) |
            TestClearPropDirty(EProperty::NET_GUARANTEE)) {
        error = UpdateTrafficClasses();
        if (error) {
            L_ERR() << "Cannot update tc : " << error << std::endl;
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::NET_RX_LIMIT)) {
        error = CreateIngressQdisc();
        if (error)
            return error;
    }

    if (TestClearPropDirty(EProperty::ULIMIT)) {
        for (auto &ct: Subtree()) {
            if (ct->State == EContainerState::Stopped ||
                    ct->State == EContainerState::Dead)
                continue;
            error = ct->ApplyUlimits();
            if (error) {
                L_ERR() << "Cannot update ulimit: " << error << std::endl;
                return error;
            }
        }
    }

    if (TestClearPropDirty(EProperty::THREAD_LIMIT)) {
        auto cg = GetCgroup(PidsSubsystem);
        error = PidsSubsystem.SetLimit(cg, ThreadLimit);
        if (error) {
            L_ERR() << "Cannot set thread limit: " << error << std::endl;
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

    if (IsRoot() || !(Controllers & CGROUP_DEVICES))
        return TError::Success();

    if (Parent->IsRoot() && HasProp(EProperty::DEVICES)) {
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

    return TError::Success();
}

TError TContainer::PrepareCgroups() {
    TError error;

    for (auto hy: Hierarchies) {
        TCgroup cg = GetCgroup(*hy);

        if (!(Controllers & hy->Controllers))
            continue;

        if (cg.Exists())
            continue;

        error = cg.Create();
        if (error)
            return error;
    }

    if (Parent && Parent->IsRoot()) {
        error = GetCgroup(MemorySubsystem).SetBool(MemorySubsystem.USE_HIERARCHY, true);
        if (error)
            return error;
    }

    if (!IsRoot() && (Controllers & CGROUP_MEMORY)) {
        error = PrepareOomMonitor();
        if (error) {
            L_ERR() << "Can't prepare OOM monitoring: " << error << std::endl;
            return error;
        }
    }

    if (Controllers & CGROUP_NETCLS) {
        auto netcls = GetCgroup(NetclsSubsystem);
        error = netcls.Set("net_cls.classid", std::to_string(LeafTC));
        if (error) {
            L_ERR() << "Can't set classid: " << error << std::endl;
            return error;
        }
    }

    return TError::Success();
}

TError TContainer::ParseNetConfig(struct TNetCfg &NetCfg) {
    TError error;

    NetCfg.Parent = Parent;
    NetCfg.Id = Id;
    NetCfg.Hostname = Hostname;
    NetCfg.NetUp = VirtMode != VIRT_MODE_OS;
    NetCfg.OwnerCred = OwnerCred;

    error = NetCfg.ParseNet(NetProp);
    if (error)
        return error;

    error = NetCfg.ParseIp(IpList);
    if (error)
        return error;

    error = NetCfg.ParseGw(DefaultGw);
    if (error)
        return error;

    if (Parent)
        NetCfg.ParentNet = Parent->Net;

    if (Net)
        NetCfg.Net = Net;

    return TError::Success();
}

TError TContainer::CheckIpLimit(struct TNetCfg &NetCfg) {

    if (NetCfg.IpVec.empty() && NetCfg.L3Only)
        return TError::Success();

    for (auto ct = Parent; ct; ct = ct->Parent) {

        /* empty means no limit */
        if (ct->IpLimit.empty() || ct->IpLimit.size() == 1 && ct->IpLimit[0] == "any")
            continue;

        if (ct->IpLimit.size() == 1 && ct->IpLimit[0] == "none")
            return TError(EError::Permission, "Parent container " + ct->Name + " forbid ip changing");

        if (!NetCfg.L3Only)
            return TError(EError::Permission, "Parent container " + ct->Name + " allows only L3 network");

        for (auto &v: NetCfg.IpVec) {
            bool allow = false;
            for (auto &str: ct->IpLimit) {
                TNlAddr mask;
                if (mask.Parse(AF_UNSPEC, str) || mask.Family() != v.Addr.Family())
                    continue;
                if (mask.IsMatch(v.Addr)) {
                    allow = true;
                    break;
                }
            }
            if (!allow)
                return TError(EError::Permission, "Parent container " + ct->Name +
                                                  " forbid address: " + v.Addr.Format());
        }
    }

    return TError::Success();
}

TError TContainer::PrepareNetwork(struct TNetCfg &NetCfg) {
    TError error;

    error = NetCfg.PrepareNetwork();
    if (error)
        return error;

    if (NetCfg.SaveIp)
        NetCfg.FormatIp(IpList);

    Net = NetCfg.Net;

    error = UpdateTrafficClasses();
    if (error) {
        L_ACT() << "Cleanup stale classes" << std::endl;

        auto lock = Net->ScopedLock();
        Net->DestroyTC(ContainerTC, LeafTC);
        lock.unlock();

        error = UpdateTrafficClasses();
        if (!error)
            return TError::Success();

        L_ACT() << "Refresh network" << std::endl;

        lock.lock();
        Net->DestroyTC(ContainerTC, LeafTC);
        Net->RefreshDevices();
        Net->NewManagedDevices = false;
        lock.unlock();

        Net->RefreshClasses();

        error = UpdateTrafficClasses();
        if (!error)
            return TError::Success();

        L_ACT() << "Recreate network" << std::endl;

        lock.lock();
        Net->RefreshDevices(true);
        Net->NewManagedDevices = false;
        Net->MissingClasses = 0;
        lock.unlock();

        Net->RefreshClasses();

        error = UpdateTrafficClasses();
        if (error) {
            L_ERR() << "Network recreation failed:" << error << std::endl;
            return error;
        }
    }

    return TError::Success();
}

TError TContainer::GetEnvironment(TEnv &env) {
    env.ClearEnv();

    env.SetEnv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
    env.SetEnv("HOME", GetCwd());
    env.SetEnv("USER", TaskCred.User());

    env.SetEnv("container", "lxc");

    /* lock these */
    env.SetEnv("PORTO_NAME", Name, true, true);
    env.SetEnv("PORTO_HOST", GetHostName(), true, true);
    env.SetEnv("PORTO_USER", OwnerCred.User(), true, true);

    /* Inherit environment from containts in isolation domain */
    bool overwrite = true;
    for (auto ct = this; ct; ct = ct->Parent.get()) {
        TError error = env.Parse(EnvCfg, overwrite);
        if (error && overwrite)
            return error;
        overwrite = false;

        if (ct->Isolate)
            break;
    }

    return TError::Success();
}

TError TContainer::PrepareTask(struct TTaskEnv *taskEnv,
                               struct TNetCfg *NetCfg) {
    auto parent = FindRunningParent();
    TError error;

    taskEnv->CT = shared_from_this();
    taskEnv->Client = CL;

    for (auto hy: Hierarchies)
        taskEnv->Cgroups.push_back(GetCgroup(*hy));

    taskEnv->Mnt.Cwd = GetCwd();
    taskEnv->Mnt.ParentCwd = Parent->GetCwd();

    if (RootVolume)
        taskEnv->Mnt.Root = Parent->RootPath.InnerPath(RootVolume->Path);
    else
        taskEnv->Mnt.Root = Root;

    taskEnv->Mnt.RootRo = RootRo;

    taskEnv->Mnt.RunSize = (GetTotalMemLimit() ?: GetTotalMemory()) / 2;

    taskEnv->Mnt.BindCred = Parent->RootPath.IsRoot() ? CL->TaskCred : TCred(RootUser, RootGroup);

    taskEnv->Cred = TaskCred;

    error = GetEnvironment(taskEnv->Env);
    if (error)
        return error;

    taskEnv->TripleFork = false;
    taskEnv->QuadroFork = (VirtMode == VIRT_MODE_APP) && !IsMeta();

    taskEnv->Mnt.BindMounts = BindMounts;
    taskEnv->Mnt.BindPortoSock = AccessLevel != EAccessLevel::None;
    taskEnv->Mnt.BindResolvConf = BindDns && ResolvConf.empty();

    error = ConfigureDevices(taskEnv->Devices);
    if (error) {
        L_ERR() << "Cannot configure devices: " << error << std::endl;
        return error;
    }

    if (parent) {
        pid_t parent_pid = parent->Task.Pid;

        error = taskEnv->ParentNs.Open(parent_pid);
        if (error)
            return error;

        /* one more fork for creating nested pid-namespace */
        if (Isolate && !InPidNamespace(parent_pid, getpid()))
            taskEnv->TripleFork = true;
    }

    if (NetCfg && NetCfg->NetNs.IsOpened())
        taskEnv->ParentNs.Net.EatFd(NetCfg->NetNs);

    if (NetCfg) {
        taskEnv->Autoconf = NetCfg->Autoconf;
        taskEnv->NewNetNs = NetCfg->NewNetNs;
    }

    if (IsMeta() || taskEnv->TripleFork || taskEnv->QuadroFork) {
        TPath exe("/proc/self/exe");
        TPath path;
        TError error = exe.ReadLink(path);
        if (error)
            return error;
        path = path.DirName() / "portoinit";
        error = taskEnv->PortoInit.OpenRead(path);
        if (error)
            return error;
    }

    // Create new mount namespaces if we have to make any changes
    taskEnv->NewMountNs = Isolate || Parent->IsRoot() ||
                          taskEnv->Mnt.BindMounts.size() ||
                          Hostname.size() ||
                          ResolvConf.size() ||
                          !taskEnv->Mnt.Root.IsRoot() ||
                          taskEnv->Mnt.RootRo ||
                          !NetCfg->Inherited;

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
    struct TTaskEnv TaskEnv;
    struct TNetCfg NetCfg;
    TError error;

    error = ParseNetConfig(NetCfg);
    if (error)
        return error;

    error = CheckIpLimit(NetCfg);
    if (error)
        return error;

    error = PrepareNetwork(NetCfg);
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
    if (IsMeta() && !Isolate && NetCfg.Inherited)
        return TError::Success();

    error = PrepareTask(&TaskEnv, &NetCfg);
    if (error)
        return error;

    error = TaskEnv.Start();

    /* Always report OOM stuation if any */
    if (error && HasOomReceived()) {
        if (error)
            L() << "Start error: " << error << std::endl;
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
    }

    /* Extra check */
    error = CL->CanControl(OwnerCred);
    if (error)
        return error;

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
        return error;

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

    L_ACT() << "Start " << Name << std::endl;

    SetState(EContainerState::Starting);

    StartTime = GetCurrentTimeMs();
    RealStartTime = time(nullptr);
    SetProp(EProperty::START_TIME);

    error = PrepareResources();
    if (error)
        return error;

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

    L() << Name << " started " << std::to_string(Task.Pid) << std::endl;

    SetProp(EProperty::ROOT_PID);

    Statistics->ContainersStarted++;
    error = UpdateSoftLimit();
    if (error)
        L_ERR() << "Can't update meta soft limit: " << error << std::endl;

    error = Save();
    if (error) {
        L_ERR() << "Cannot save state after start " << error << std::endl;
        (void)Reap(false);
    }

    return error;
}

TError TContainer::PrepareWorkDir() {
    if (IsRoot())
        return TError::Success();

    TPath work = WorkPath();
    if (work.Exists())
        return TError::Success(); /* FIXME kludge for restore */

    TError error = work.Mkdir(0755);
    if (!error)
        error = work.Chown(TaskCred);
    return error;
}

TError TContainer::PrepareResources() {
    TError error;

    error = PrepareWorkDir();
    if (error) {
        if (error.GetErrno() == ENOSPC)
            L() << "Cannot create working dir: " << error << std::endl;
        else
            L_ERR() << "Cannot create working dir: " << error << std::endl;
        FreeResources();
        return error;
    }

    ChooseTrafficClasses();

    error = PrepareCgroups();
    if (error) {
        L_ERR() << "Can't prepare task cgroups: " << error << std::endl;
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
            L_ERR() << "Cannot create root volume: " << error << std::endl;
            FreeResources();
            return error;
        }

        RootPath = RootVolume->Path;
    }

    return TError::Success();
}

void TContainer::FreeResources() {
    TError error;

    ShutdownOom();

    if (!IsRoot()) {
        for (auto hy: Hierarchies) {
            if (Controllers & hy->Controllers) {
                auto cg = GetCgroup(*hy);
                (void)cg.Remove(); //Logged inside
            }
        }
    }

    if (Net) {
        struct TNetCfg NetCfg;

        error = ParseNetConfig(NetCfg);
        if (!error)
            error = NetCfg.DestroyNetwork();
        if (NetCfg.SaveIp) {
            TMultiTuple ip_settings;
            NetCfg.FormatIp(IpList);
        }
        if (error)
            L_ERR() << "Cannot free network resources: " << error << std::endl;

        if (Controllers & CGROUP_NETCLS) {
            auto net_lock = Net->ScopedLock();
            error = Net->DestroyTC(ContainerTC, LeafTC);
            if (error)
                L_ERR() << "Can't remove traffic class: " << error << std::endl;
            net_lock.unlock();

            if (Net != HostNetwork) {
                net_lock = HostNetwork->ScopedLock();
                error = HostNetwork->DestroyTC(ContainerTC, LeafTC);
                if (error)
                    L_ERR() << "Can't remove traffic class: " << error << std::endl;
            }
        }
    }

    if (Net && IsRoot()) {
        error = Net->Destroy();
        if (error)
            L_ERR() << "Cannot destroy network: " << error << std::endl;
    }
    Net = nullptr;

    if (IsRoot())
        return;

    /* Legacy non-volume root on loop device */
    if (LoopDev >= 0) {
        error = PutLoopDev(LoopDev);
        if (error)
            L_ERR() << "Can't put loop device " << LoopDev << ": " << error << std::endl;
        LoopDev = -1;
        SetProp(EProperty::LOOP_DEV);

        TPath tmp = TPath(PORTO_WORKDIR) / std::to_string(Id);
        if (tmp.Exists()) {
            error = tmp.RemoveAll();
            if (error)
                L_ERR() << "Can't remove " << tmp << ": " << error << std::endl;
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
            L_ERR() << "Cannot remove working dir: " << error << std::endl;
    }

    Stdout.Remove(*this);
    Stderr.Remove(*this);
}

TError TContainer::Kill(int sig) {
    if (State != EContainerState::Running)
        return TError(EError::InvalidState, "invalid container state ");

    L_ACT() << "Kill " << Name << " pid " << Task.Pid << std::endl;
    return Task.Kill(sig);
}

TError TContainer::Terminate(uint64_t deadline) {
    auto cg = GetCgroup(FreezerSubsystem);
    TError error;

    if (IsRoot())
        return TError(EError::Permission, "Cannot terminate root container");

    L_ACT() << "Terminate tasks in " << Name << std::endl;

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
                L_ACT() << "Wait task " << Task.Pid << " after signal "
                        << sig << " in " << Name << std::endl;
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

    while (!cg.IsEmpty() && !WaitDeadline(deadline));

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
            L_ERR() << "Cannot terminate tasks in container: " << error << std::endl;
            return error;
        }

        if (FreezerSubsystem.IsSelfFreezing(cg)) {
            L_ACT() << "Thaw terminated paused container " << ct->Name << std::endl;
            error = FreezerSubsystem.Thaw(cg, false);
            if (error)
                return error;
        }
    }

    UpgradeLock();

    for (auto &ct: subtree) {
        if (ct->State == EContainerState::Stopped)
            continue;

        L_ACT() << "Stop " << Name << std::endl;

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

    error = UpdateSoftLimit();
    if (error)
        L_ERR() << "Can't update meta soft limit: " << error << std::endl;

    return TError::Success();
}

void TContainer::Reap(bool oomKilled) {
    TError error;

    error = Terminate(0);
    if (error)
        L_WRN() << "Cannot terminate container " << Name << " : " << error << std::endl;

    ShutdownOom();

    DeathTime = GetCurrentTimeMs();
    SetProp(EProperty::DEATH_TIME);

    if (oomKilled) {
        OomEvents++;
        Statistics->ContainersOOM++;
        OomKilled = oomKilled;
        SetProp(EProperty::OOM_KILLED);
    }

    ForgetPid();

    Stdout.Rotate(*this);
    Stderr.Rotate(*this);

    SetState(EContainerState::Dead);

    error = Save();
    if (error)
        L_WRN() << "Cannot save container state after exit: " << error << std::endl;

    if (MayRespawn())
        ScheduleRespawn();
}

void TContainer::Exit(int status, bool oomKilled) {

    if (State == EContainerState::Stopped)
        return;

    /* SIGKILL could be delivered earlier than OOM event */
    if (!oomKilled && HasOomReceived())
        oomKilled = true;

    /* Detect fatal signals: portoinit cannot kill itself */
    if (Isolate && VirtMode == VIRT_MODE_APP && WIFEXITED(status) &&
            WEXITSTATUS(status) > 128 && WEXITSTATUS(status) < 128 + SIGRTMIN)
        status = WEXITSTATUS(status) - 128;

    L_EVT() << "Exit " << Name << " " << FormatExitStatus(status)
            << (oomKilled ? " invoked by OOM" : "") << std::endl;

    ExitStatus = status;
    SetProp(EProperty::EXIT_STATUS);

    /* Detect memory shortage that happened in syscalls */
    auto cg = GetCgroup(MemorySubsystem);
    if (!oomKilled && MemorySubsystem.GetOomEvents(cg)) {
        L() << "Container " << Name << " hit memory limit" << std::endl;
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
                L_ERR() << "Cannot save state after pause: " << error << std::endl;
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
            L_ERR() << "Cannot save state after resume: " << error << std::endl;
    }

    return TError::Success();
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

TError TContainer::RestoreNetwork() {
    TNamespaceFd netns;
    TError error;

    error = OpenNetns(netns);
    if (error)
        return error;

    Net = TNetwork::GetNetwork(netns.GetInode());

    /* Create a new one */
    if (!Net) {
        Net = std::make_shared<TNetwork>();
        PORTO_ASSERT(Net);

        error = Net->ConnectNetns(netns);
        if (error)
            return error;

        TNetwork::AddNetwork(netns.GetInode(), Net);
    }

    ChooseTrafficClasses();

    error = UpdateTrafficClasses();
    if (error)
        return error;

    return TError::Success();
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
            L_WRN() << "Unknown property: " << key << ", skipped" << std::endl;
            continue;
        }
        auto prop = it->second;

        error = prop->SetFromRestore(value);
        if (error) {
            L_ERR() << "Cannot load " << key << ": " << error << std::endl;
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

    if (!node.Has(P_OWNER_USER) || !node.Has(P_OWNER_GROUP))
        OwnerCred = TaskCred;

    if (!node.Has(P_PORTO_NAMESPACE))
        NsName = "";

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

    L_ACT() << "Sync " << Name << " state " << StateName(State) << std::endl;

    if (!freezerCg.Exists()) {
        if (State != EContainerState::Stopped)
            L_WRN() << "Freezer not found" << std::endl;
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
        L() << "Found unexpected freezer" << std::endl;
        Reap(false);
    } else if (State == EContainerState::Meta && !WaitTask.Pid && !Isolate) {
        /* meta container */
    } else if (!WaitTask.Exists()) {
        if (State != EContainerState::Dead)
            L() << "Task no found" << std::endl;
        Reap(false);
    } else if (WaitTask.IsZombie()) {
        L() << "Task is zombie" << std::endl;
        Task.Pid = 0;
    } else if (FreezerSubsystem.TaskCgroup(WaitTask.Pid, taskCg)) {
        L() << "Cannot check freezer" << std::endl;
        Reap(false);
    } else if (taskCg != freezerCg) {
        L() << "Task in wrong freezer" << std::endl;
        if (WaitTask.GetPPid() == getppid()) {
            if (Task.Pid != WaitTask.Pid && Task.GetPPid() == WaitTask.Pid)
                Task.Kill(SIGKILL);
            WaitTask.Kill(SIGKILL);
        }
        Reap(false);
    } else {
        pid_t ppid = WaitTask.GetPPid();
        if (ppid != getppid()) {
            L() << "Task reparented to " << ppid << " (" << GetTaskName(ppid) << "). Seize." << std::endl;
            error = Seize();
            if (error) {
                L() << "Cannot seize reparented task: " << error << std::endl;
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
            L_ERR() << "Destroyed parent?" << std::endl;
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

bool TContainer::MayReceiveOom(int fd) {
    if (OomEvent.Fd != fd)
        return false;

    if (!Task.Pid)
        return false;

    if (State == EContainerState::Dead)
        return false;

    return true;
}

// Works only once
bool TContainer::HasOomReceived() {
    uint64_t val;

    return read(OomEvent.Fd, &val, sizeof(val)) == sizeof(val) && val != 0;
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

    SystemClient.StartRequest();
    SystemClient.LockedContainer = shared_from_this();
    error = Start();
    SystemClient.LockedContainer = nullptr;
    SystemClient.FinishRequest();

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
                if (ct->OomIsFatal) {
                    ct->Exit(SIGKILL, true);
                } else {
                    L_EVT() << "Non fatal OOM in " << ct->Name << std::endl;
                    ct->OomEvents++;
                    Statistics->ContainersOOM++;
                }
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
                L() << "Unknown zombie " << event.Exit.Pid << " " << event.Exit.Status << std::endl;
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

    case EEventType::NetworkWatchdog:
        lock.unlock();
        TNetwork::RefreshNetworks();
        EventQueue->Add(config().network().watchdog_ms(), event);
        break;

    }
}

std::string TContainer::GetPortoNamespace(bool write) const {
    std::string ns;
    for (auto ct = this; ct && !ct->IsRoot() ; ct = ct->Parent.get()) {
        if (ct->AccessLevel == EAccessLevel::Isolate ||
                ct->AccessLevel == EAccessLevel::ReadIsolate ||
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

void TContainer::ChooseTrafficClasses() {
    if (IsRoot()) {
        ContainerTC = TcHandle(ROOT_TC_MAJOR, ROOT_CONTAINER_ID);
        ParentTC = TcHandle(ROOT_TC_MAJOR, ROOT_TC_MINOR);
        LeafTC = TcHandle(ROOT_TC_MAJOR, DEFAULT_TC_MINOR);
    } else if (Controllers & CGROUP_NETCLS) {
        ContainerTC = TcHandle(ROOT_TC_MAJOR, Id);
        ParentTC = Parent->ContainerTC;
        LeafTC = TcHandle(ROOT_TC_MAJOR, Id + CONTAINER_ID_MAX);
    } else {
        ContainerTC = Parent->ContainerTC;
        ParentTC = Parent->ParentTC;
        LeafTC = Parent->LeafTC;
    }
}

TError TContainer::UpdateTrafficClasses() {
    TError error;

    if (!(Controllers & CGROUP_NETCLS))
        return TError::Success();

    auto net_lock = HostNetwork->ScopedLock();
    error = HostNetwork->CreateTC(ContainerTC, ParentTC, LeafTC,
                                  NetPriority, NetGuarantee, NetLimit);
    if (error)
        return error;
    net_lock.unlock();

    if (Net && Net != HostNetwork) {
        uint32_t parent = ParentTC;
        if (Net != Parent->Net)
            parent = TcHandle(ROOT_TC_MAJOR, ROOT_CONTAINER_ID);
        auto net_lock = Net->ScopedLock();
        error = Net->CreateTC(ContainerTC, parent, LeafTC,
                              NetPriority, NetGuarantee, NetLimit);
    }

    return error;
}

TError TContainer::CreateIngressQdisc() {
    if (!NetIsolate || Net == HostNetwork)
        return TError(EError::InvalidValue, "Net rx limit requires isolated network");

    auto net_lock = Net->ScopedLock();
    return Net->CreateIngressQdisc(NetRxLimit);
}

TContainerWaiter::TContainerWaiter(std::shared_ptr<TClient> client,
                                   std::function<void (std::shared_ptr<TClient>,
                                                       TError, std::string)> callback) :
    Client(client), Callback(callback) {
}

void TContainerWaiter::WakeupWaiter(const TContainer *who, bool wildcard) {
    std::shared_ptr<TClient> client = Client.lock();
    if (client) {
        std::string name;
        TError err;
        if (who)
            err = client->ComposeName(who->Name, name);
        if (wildcard && (err || !MatchWildcard(name)))
            return;
        Callback(client, err, name);
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
