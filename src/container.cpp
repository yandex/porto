#include <sstream>
#include <fstream>
#include <memory>
#include <csignal>
#include <cstdlib>
#include <algorithm>

#include "statistics.hpp"
#include "container.hpp"
#include "config.hpp"
#include "task.hpp"
#include "cgroup.hpp"
#include "subsystem.hpp"
#include "property.hpp"
#include "data.hpp"
#include "event.hpp"
#include "holder.hpp"
#include "qdisc.hpp"
#include "context.hpp"
#include "container_value.hpp"
#include "task.hpp"
#include "epoll.hpp"
#include "kvalue.hpp"
#include "volume.hpp"
#include "util/log.hpp"
#include "util/file.hpp"
#include "util/string.hpp"
#include "util/netlink.hpp"
#include "util/cred.hpp"
#include "util/unix.hpp"
#include "client.hpp"

#include "portod.hpp"

extern "C" {
#include <sys/types.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/reboot.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/fsuid.h>
#include <sys/stat.h>
}

using std::string;
using std::vector;
using std::shared_ptr;
using std::unique_ptr;
using std::map;

int64_t BootTime = 0;

TContainer::TContainer(std::shared_ptr<TContainerHolder> holder,
                       std::shared_ptr<TKeyValueStorage> storage,
                       const std::string &name, std::shared_ptr<TContainer> parent,
                       uint16_t id, std::shared_ptr<TNetwork> net) :
    Holder(holder), Name(StripParentName(name)), Parent(parent),
    Storage(storage), Id(id), Net(net) { }

TContainer::~TContainer() {
    // Tclass destructor should be called with TNetwork locked,
    // so call them explicitly in Tcontainer::Destroy()
    PORTO_ASSERT(Tclass == nullptr);
};

std::string TContainer::ContainerStateName(EContainerState state) {
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

TPath TContainer::GetTmpDir() const {
    return TPath(config().container().tmp_dir()) / std::to_string(Id);
}

/* Returns normalized root path in host namespace */
TPath TContainer::RootPath() const {

    if (IsRoot() || IsPortoRoot())
        return TPath("/");

    TPath path(Prop->Get<std::string>(P_ROOT));
    if (!path.IsRoot()) {
        if (path.GetType() == EFileType::Regular)
            path = GetTmpDir();
        path = path.NormalPath();
    }

    return path;
}

TPath TContainer::DefaultStdFile(const std::string prefix) const {
    TPath root(Prop->Get<std::string>(P_ROOT));
    std::string cwd(Prop->Get<std::string>(P_CWD));
    std::string name(prefix + "." + GetName(true, "_"));

    if (root.GetType() == EFileType::Regular)
        return GetTmpDir() / name;

    return root / cwd / name;
}

EContainerState TContainer::GetState() const {
    return State;
}

bool TContainer::IsLostAndRestored() const {
    return LostAndRestored;
}

void TContainer::SyncStateWithCgroup(TScopedLock &holder_lock) {
    if (LostAndRestored && State == EContainerState::Running &&
        (!Task || Processes().empty())) {
        L() << "Lost and restored container " << GetName() << " is empty"
                      << ", mark them dead." << std::endl;
        ExitTree(holder_lock, -1, false);
    }
}

TError TContainer::GetStat(ETclassStat stat, std::map<std::string, uint64_t> &m) {
    auto lock = Net->ScopedLock();
    return Tclass->GetStat(stat, m);
}

void TContainer::UpdateRunningChildren(size_t diff) {
    RunningChildren += diff;

    if (Parent)
        Parent->UpdateRunningChildren(diff);
}

TError TContainer::UpdateSoftLimit() {
    if (IsRoot() || IsPortoRoot())
        return TError::Success();

    if (Parent)
        Parent->UpdateSoftLimit();

    if (GetState() == EContainerState::Meta) {
        uint64_t defaultLimit;

        TError error = memorySubsystem->GetSoftLimit(memorySubsystem->GetRootCgroup(), defaultLimit);
        if (error)
            return error;

        uint64_t limit = RunningChildren ? defaultLimit : 1 * 1024 * 1024;
        uint64_t currentLimit;

        auto cg = GetLeafCgroup(memorySubsystem);
        error = memorySubsystem->GetSoftLimit(cg, currentLimit);
        if (error)
            return error;

        if (currentLimit != limit) {
            error = memorySubsystem->SetSoftLimit(cg, limit);
            if (error)
                return error;
        }
    }

    return TError::Success();
}

void TContainer::SetState(EContainerState newState) {
    if (newState == EContainerState::Running && IsMeta)
        newState = EContainerState::Meta;

    if (State == newState)
        return;

    L_ACT() << GetName() << ": change state " << ContainerStateName(State) << " -> " << ContainerStateName(newState) << std::endl;
    if (newState == EContainerState::Running) {
        UpdateRunningChildren(+1);
    } else if (State == EContainerState::Running) {
        UpdateRunningChildren(-1);
    }

    State = newState;
    Data->Set<std::string>(D_STATE, ContainerStateName(State));

    NotifyWaiters();
}

const string TContainer::StripParentName(const string &name) const {
    if (name == ROOT_CONTAINER)
        return ROOT_CONTAINER;
    else if (name == PORTO_ROOT_CONTAINER)
        return PORTO_ROOT_CONTAINER;

    std::string::size_type n = name.rfind('/');
    if (n == std::string::npos)
        return name;
    else
        return name.substr(n + 1);
}

void TContainer::RemoveKvs() {
    if (IsRoot() || IsPortoRoot())
        return;

    auto kvnode = Storage->GetNode(Id);
    TError error = kvnode->Remove();
    if (error)
        L_ERR() << "Can't remove key-value node " << kvnode->GetName() << ": " << error << std::endl;
}

void TContainer::DestroyVolumes(TScopedLock &holder_lock) {
    if (!VolumeHolder)
        return;

    TScopedUnlock holder_unlock(holder_lock);
    TScopedLock vholder_lock = VolumeHolder->ScopedLock();

    for (auto volume: Volumes) {
        if (!volume->UnlinkContainer(Name))
            continue; /* Still linked to somebody */
        vholder_lock.unlock();
        auto volume_lock = volume->ScopedLock();
        if (!volume->IsReady()) {
            volume_lock.unlock();
            vholder_lock.lock();
            continue;
        }
        vholder_lock.lock();
        TError error = volume->SetReady(false);
        vholder_lock.unlock();
        error = volume->Destroy();
        vholder_lock.lock();
        VolumeHolder->Unregister(volume);
        VolumeHolder->Remove(volume);
        volume_lock.unlock();
    }

    Volumes.clear();
}

void TContainer::Destroy(TScopedLock &holder_lock) {
    L_ACT() << "Destroy " << GetName() << " " << Id << std::endl;

    SetState(EContainerState::Unknown);
    DestroyVolumes(holder_lock);
    RemoveKvs();
}

const string TContainer::GetName(bool recursive, const std::string &sep) const {
    if (!recursive)
        return Name;

    if (IsRoot() || IsPortoRoot() || Parent->IsPortoRoot())
        return Name;
    else
        return Parent->GetName(recursive, sep) + sep + Name;
}

bool TContainer::IsRoot() const {
    return Id == ROOT_CONTAINER_ID;
}

bool TContainer::IsPortoRoot() const {
    return Id == PORTO_ROOT_CONTAINER_ID;
}

std::shared_ptr<const TContainer> TContainer::GetRoot() const {
    if (Parent)
        return Parent->GetRoot();
    else
        return shared_from_this();
}

std::shared_ptr<TContainer> TContainer::GetParent() const {
    return Parent;
}

bool TContainer::ValidLink(const std::string &name) const {
    auto lock = Net->ScopedLock();

    if (Net->Empty())
        return false;

    std::shared_ptr<TNl> nl = Net->GetNl();
    return nl->ValidLink(name);
}

std::shared_ptr<TNlLink> TContainer::GetLink(const std::string &name) const {
    auto lock = Net->ScopedLock();

    for (auto &link : Net->GetLinks())
        if (link->GetAlias() == name)
            return link;

    return nullptr;
}

uint64_t TContainer::GetChildrenSum(const std::string &property, std::shared_ptr<const TContainer> except, uint64_t exceptVal) const {
    uint64_t val = 0;

    for (auto iter : Children)
        if (auto child = iter.lock()) {
            if (except && except == child) {
                val += exceptVal;
                continue;
            }

            uint64_t childval = child->Prop->Get<uint64_t>(property);
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
        uint64_t parent = c->Prop->Get<uint64_t>(property);
        if (parent && value > parent)
            return false;
    }

    if (GetParent()) {
        uint64_t parent = GetParent()->Prop->Get<uint64_t>(property);
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

    TError error = memorySubsystem->UseHierarchy(memcg, config().container().use_hierarchy());
    if (error) {
        L_ERR() << "Can't set use_hierarchy for " << memcg->Relpath() << ": " << error << std::endl;
        // we don't want to get this error endlessly when user switches config
        // so be tolerant
        //return error;
    }

    error = memorySubsystem->SetGuarantee(memcg, Prop->Get<uint64_t>(P_MEM_GUARANTEE));
    if (error) {
        L_ERR() << "Can't set " << P_MEM_GUARANTEE << ": " << error << std::endl;
        return error;
    }

    error = memorySubsystem->SetLimit(memcg, Prop->Get<uint64_t>(P_MEM_LIMIT));
    if (error) {
        if (error.GetErrno() == EBUSY)
            return TError(EError::InvalidValue, std::string(P_MEM_LIMIT) + " is too low");

        L_ERR() << "Can't set " << P_MEM_LIMIT << ": " << error << std::endl;
        return error;
    }

    error = memorySubsystem->RechargeOnPgfault(memcg, Prop->Get<bool>(P_RECHARGE_ON_PGFAULT));
    if (error) {
        L_ERR() << "Can't set " << P_RECHARGE_ON_PGFAULT << ": " << error << std::endl;
        return error;
    }

    auto cpucg = GetLeafCgroup(cpuSubsystem);
    error = cpuSubsystem->SetPolicy(cpucg, Prop->Get<std::string>(P_CPU_POLICY));
    if (error) {
        L_ERR() << "Can't set " << P_CPU_POLICY << ": " << error << std::endl;
        return error;
    }

    if (Prop->Get<std::string>(P_CPU_POLICY) == "normal") {
        error = cpuSubsystem->SetLimit(cpucg, Prop->Get<uint64_t>(P_CPU_LIMIT));
        if (error) {
            L_ERR() << "Can't set " << P_CPU_LIMIT << ": " << error << std::endl;
            return error;
        }

        error = cpuSubsystem->SetGuarantee(cpucg, Prop->Get<uint64_t>(P_CPU_GUARANTEE));
        if (error) {
            L_ERR() << "Can't set " << P_CPU_GUARANTEE << ": " << error << std::endl;
            return error;
        }
    }

    auto blkcg = GetLeafCgroup(blkioSubsystem);
    error = blkioSubsystem->SetPolicy(blkcg, Prop->Get<std::string>(P_IO_POLICY) == "batch");
    if (error) {
        L_ERR() << "Can't set " << P_IO_POLICY << ": " << error << std::endl;
        return error;
    }

    error = memorySubsystem->SetIoLimit(memcg, Prop->Get<uint64_t>(P_IO_LIMIT));
    if (error) {
        L_ERR() << "Can't set " << P_IO_LIMIT << ": " << error << std::endl;
        return error;
    }

    return TError::Success();
}

std::shared_ptr<TContainer> TContainer::FindRunningParent() const {
    auto p = Parent;
    while (p) {
        if (p->Task && p->Task->IsRunning())
            return p;
        p = p->Parent;
    }

    return nullptr;
}

bool TContainer::UseParentNamespace() const {
    if (Prop->GetRaw<bool>(P_ISOLATE))
        return false;

    return FindRunningParent() != nullptr;
}

TError TContainer::PrepareNetwork() {
    if (!config().network().enabled())
        return TError::Success();

    PORTO_ASSERT(Tclass == nullptr);

    if (UseParentNamespace()) {
        Tclass = Parent->Tclass;
        return TError::Success();
    }

    if (Parent) {
        PORTO_ASSERT(Parent->Tclass != nullptr);

        auto tclass = Parent->Tclass;
        uint32_t handle = TcHandle(TcMajor(tclass->GetHandle()), Id);
        Tclass = std::make_shared<TTclass>(Net, tclass, handle);
    } else {
        uint32_t handle = TcHandle(TcMajor(Net->GetQdisc()->GetHandle()), Id);
        Tclass = std::make_shared<TTclass>(Net, Net->GetQdisc(), handle);
    }

    TUintMap prio, rate, ceil;
    prio = Prop->Get<TUintMap>(P_NET_PRIO);
    rate = Prop->Get<TUintMap>(P_NET_GUARANTEE);
    ceil = Prop->Get<TUintMap>(P_NET_LIMIT);

    Tclass->Prepare(prio, rate, ceil);

    auto net_lock = Net->ScopedLock();
    TError error = Tclass->Create();
    if (error) {
        L_ERR() << "Can't create tclass: " << error << std::endl;
        return error;
    }

    return TError::Success();
}

void TContainer::ShutdownOom() {
    if (Source)
        Holder->EpollLoop->RemoveSource(Source);
    Efd = -1;
    Source = nullptr;
}

TError TContainer::PrepareOomMonitor() {
    auto memcg = GetLeafCgroup(memorySubsystem);

    Efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (Efd.GetFd() < 0) {
        TError error(EError::Unknown, errno, "Can't create eventfd");
        L_ERR() << "Can't update OOM settings: " << error << std::endl;
        return error;
    }

    Source = std::make_shared<TEpollSource>(Holder->EpollLoop, Efd.GetFd(),
                                            EPOLL_EVENT_OOM, shared_from_this());

    TError error = Holder->EpollLoop->AddSource(Source);
    if (error) {
        ShutdownOom();
        return error;
    }

    auto cfdPath = memcg->Path() / "memory.oom_control";
    TScopedFd cfd(open(cfdPath.c_str(), O_RDONLY | O_CLOEXEC));
    if (cfd.GetFd() < 0) {
        ShutdownOom();
        TError error(EError::Unknown, errno, "Can't open " + memcg->Path().ToString());
        L_ERR() << "Can't update OOM settings: " << error << std::endl;
        return error;
    }

    TFile f(memcg->Path() / "cgroup.event_control");
    string s = std::to_string(Efd.GetFd()) + " " + std::to_string(cfd.GetFd());
    error = f.WriteStringNoAppend(s);
    if (error) {
        ShutdownOom();
        return error;
    }

    return TError::Success();
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

    if (config().network().enabled()) {
        auto netcls = GetLeafCgroup(netclsSubsystem);
        uint32_t handle = Tclass->GetHandle();
        TError error = netcls->SetKnobValue("net_cls.classid", std::to_string(handle), false);
        if (error) {
            L_ERR() << "Can't set classid: " << error << std::endl;
            return error;
        }
    }

    if (!IsRoot()) {
        TError error = ApplyDynamicProperties();
        if (error)
            return error;
    }

    if (!IsRoot() && !IsPortoRoot()) {
        TError error = PrepareOomMonitor();
        if (error) {
            L_ERR() << "Can't prepare OOM monitoring: " << error << std::endl;
            return error;
        }

        auto devices = GetLeafCgroup(devicesSubsystem);
        error = devicesSubsystem->AllowDevices(devices,
                                               Prop->Get<TStrList>(P_ALLOWED_DEVICES));
        if (error) {
            L_ERR() << "Can't set " << P_ALLOWED_DEVICES << ": " << error << std::endl;
            return error;
        }
    }

    return TError::Success();
}

bool TContainer::IsNamespaceIsolated() {
    if (IsRoot() || IsPortoRoot())
        return false;

    if (Prop->Get<std::string>(P_ROOT) != "/" &&
        Prop->Get<bool>(P_ENABLE_PORTO))
        return true;

    if (Parent)
        return Parent->IsNamespaceIsolated();

    return false;
}

void TContainer::CleanupExpiredChildren() {
    for (auto iter = Children.begin(); iter != Children.end();) {
        auto child = iter->lock();
        if (child) {
            iter++;
            continue;
        }

        iter = Children.erase(iter);
    }
}

TError TContainer::PrepareTask(std::shared_ptr<TClient> client) {
    if (!Prop->Get<bool>(P_ISOLATE))
        for (auto name : Prop->List())
            if (Prop->Find(name)->GetFlags() & PARENT_RO_PROPERTY)
                if (!Prop->IsDefault(name))
                    return TError(EError::InvalidValue, "Can't use custom " + name + " with " + P_ISOLATE + " == false");

    auto vmode = Prop->Get<int>(P_VIRT_MODE);
    auto user = Prop->Get<std::string>(P_USER);
    auto taskEnv = std::unique_ptr<TTaskEnv>(new TTaskEnv());
    auto parent = FindRunningParent();

    taskEnv->LeafCgroups = LeafCgroups;

    taskEnv->Container = GetName();
    taskEnv->Command = Prop->Get<std::string>(P_COMMAND);
    taskEnv->Cwd = Prop->Get<std::string>(P_CWD);

    taskEnv->Root = Prop->Get<std::string>(P_ROOT);
    taskEnv->LoopDev = Prop->Get<int>(P_RAW_LOOP_DEV);
    taskEnv->CloneParentMntNs = false;

    if (taskEnv->LoopDev >= 0) {
        taskEnv->Root = GetTmpDir();
    } else if (parent) {
        if (Prop->IsDefault(P_ROOT) ||
                taskEnv->Root == parent->Prop->Get<std::string>(P_ROOT)) {
            taskEnv->Root = "/";
            taskEnv->CloneParentMntNs = true;
        } else {
            auto inner_root = parent->RootPath().InnerPath(taskEnv->Root, true);

            if (!inner_root.IsEmpty()) {
                taskEnv->Root = inner_root;
                taskEnv->CloneParentMntNs = true;
            }
        }
    }

    taskEnv->RootRdOnly = Prop->Get<bool>(P_ROOT_RDONLY);
    taskEnv->CreateCwd = Prop->IsDefault(P_ROOT) && Prop->IsDefault(P_CWD) && !UseParentNamespace();

    if (vmode == VIRT_MODE_OS) {
        user = "root";
        taskEnv->Cred = TCred(0, 0);
    } else {
        taskEnv->Cred = OwnerCred;
        TError error = taskEnv->Cred.LoadGroups(user);
        if (error)
            return error;
    }

    std::map<std::string, std::string> portoEnv = {
        { "PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" },
        { "container", "lxc" },
        { "PORTO_NAME", GetName() },
        { "PORTO_HOST", GetHostName() },
        { "HOME", Prop->Get<std::string>(P_CWD) },
        { "USER", user },
    };

    taskEnv->Environ = Prop->Get<TStrList>(P_ENV);
    for (auto pair : portoEnv) {
        if (taskEnv->EnvHasKey(pair.first))
            continue;

        taskEnv->Environ.push_back(pair.first + "=" + pair.second);
    }

    taskEnv->Isolate = Prop->Get<bool>(P_ISOLATE);
    taskEnv->TripleFork = false;

    taskEnv->DefaultStdin = Prop->IsDefault(P_STDIN_PATH);
    taskEnv->DefaultStdout = Prop->IsDefault(P_STDOUT_PATH);
    taskEnv->DefaultStderr = Prop->IsDefault(P_STDERR_PATH);
    taskEnv->StdinPath = Prop->Get<std::string>(P_STDIN_PATH);
    taskEnv->StdoutPath = Prop->Get<std::string>(P_STDOUT_PATH);
    taskEnv->StderrPath = Prop->Get<std::string>(P_STDERR_PATH);

    taskEnv->Hostname = Prop->Get<std::string>(P_HOSTNAME);
    taskEnv->SetEtcHostname = (vmode == VIRT_MODE_OS) &&
                                !taskEnv->Root.IsRoot() &&
                                !taskEnv->RootRdOnly;

    taskEnv->BindDns = Prop->Get<bool>(P_BIND_DNS);

    if (client) {
        std::shared_ptr<TContainer> clientContainer;
        TError error = client->GetContainer(clientContainer);
        if (error)
            return error;
        TPath clientRoot = clientContainer->RootPath();
        if (clientRoot.IsEmpty())
            return TError(EError::InvalidValue, "Cannot get client root path");
        if (!clientRoot.IsRoot()) {
            TError error = taskEnv->ClientMntNs.Open(client->GetPid(), "ns/mnt");
            if (error)
                return error;

            if (!taskEnv->DefaultStdin)
                taskEnv->StdinPath = clientRoot.InnerPath(taskEnv->StdinPath, true);
            if (!taskEnv->DefaultStdout)
                taskEnv->StdoutPath = clientRoot.InnerPath(taskEnv->StdoutPath, true);
            if (!taskEnv->DefaultStderr)
                taskEnv->StderrPath = clientRoot.InnerPath(taskEnv->StderrPath, true);
        }
    }

    TError error = Prop->PrepareTaskEnv(P_ULIMIT, *taskEnv);
    if (error)
        return error;

    error = Prop->PrepareTaskEnv(P_BIND, *taskEnv);
    if (error)
        return error;

    error = Prop->PrepareTaskEnv(P_CAPABILITIES, *taskEnv);
    if (error)
        return error;

    if (Prop->Get<bool>(P_ENABLE_PORTO) && IsNamespaceIsolated()) {
        TBindMap bm = { config().rpc_sock().file().path(),
                        config().rpc_sock().file().path(),
                        false };

        taskEnv->BindMap.push_back(bm);
    }

    if (config().network().enabled()) {
        error = Prop->PrepareTaskEnv(P_IP, *taskEnv);
        if (error)
            return error;

        error = Prop->PrepareTaskEnv(P_DEFAULT_GW, *taskEnv);
        if (error)
            return error;

        error = Prop->PrepareTaskEnv(P_NET, *taskEnv);
        if (error)
            return error;
    } else {
        taskEnv->NetCfg.Clear();
        taskEnv->NetCfg.NewNetNs = false;
        taskEnv->NetCfg.Host = true;
    }

    taskEnv->NetUp = Prop->Get<int>(P_VIRT_MODE) != VIRT_MODE_OS;

    if (parent) {
        pid_t parent_pid = parent->Task->GetPid();

        if (!taskEnv->Isolate) {
            error = taskEnv->ParentNs.Open(parent_pid);
            if (client && error)
                return error;
        } else {
            if (!InPidNamespace(parent_pid, getpid())) {
                error = taskEnv->ParentNs.Pid.Open(parent_pid, "ns/pid");
                if (client && error)
                    return error;
                taskEnv->TripleFork = true;
            }
            if (taskEnv->NetCfg.Inherited) {
                error = taskEnv->ParentNs.Net.Open(parent_pid, "ns/net");
                if (client && error)
                    return error;
            }
            if (taskEnv->CloneParentMntNs) {
                error = taskEnv->ParentNs.Mnt.Open(parent_pid, "ns/mnt");
                if (client && error)
                    return error;
            }
            if (taskEnv->Hostname == "") {
                error = taskEnv->ParentNs.Uts.Open(parent_pid, "ns/uts");
                if (client && error)
                    return error;
            }
        }
    }

    if (!taskEnv->CloneParentMntNs) {
        /* We'll return into host after trip into ClientMntNs */
        error = taskEnv->ParentNs.Mnt.Open(getpid(), "ns/mnt");
        if (client && error)
            return error;
    }

    if (taskEnv->NetCfg.NetNsName != "") {
        error = taskEnv->ParentNs.Net.Open("/var/run/netns/" +
                                           taskEnv->NetCfg.NetNsName);
        if (client && error)
            return error;
    }

    if (taskEnv->Command.empty() || taskEnv->TripleFork) {
        TPath exe("/proc/self/exe");
        TPath path;
        TError error = exe.ReadLink(path);
        if (error)
            return error;
        path = path.DirName() / "portoinit";
        taskEnv->PortoInitFd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (taskEnv->PortoInitFd.GetFd() < 0)
            return TError(EError::Unknown, errno, "Cannot open " + path.ToString());
    }

    // Create new mount namespaces if we have to make any changes
    taskEnv->NewMountNs = taskEnv->Isolate || taskEnv->BindMap.size() ||
                          taskEnv->RootRdOnly || !taskEnv->Root.IsRoot();

    Task = std::unique_ptr<TTask>(new TTask(taskEnv));

    return TError::Success();
}

void TContainer::AddChild(std::shared_ptr<TContainer> child) {
    Children.push_back(child);
}

TError TContainer::Create(const TCred &cred) {
    L_ACT() << "Create " << GetName() << " with id " << Id << " uid " << cred.Uid << " gid " << cred.Gid << std::endl;

    TError error = Prepare();
    if (error) {
        L_ERR() << "Can't prepare container: " << error << std::endl;
        return error;
    }

    OwnerCred = TCred(cred.Uid, cred.Gid);

    error = Prop->Set<std::string>(P_USER, cred.UserAsString());
    if (error)
        return error;

    error = Prop->Set<std::string>(P_GROUP, cred.GroupAsString());
    if (error)
        return error;

    SetState(EContainerState::Stopped);

    return TError::Success();
}

TError TContainer::Start(std::shared_ptr<TClient> client, bool meta) {
    auto state = GetState();

    if (state != EContainerState::Stopped)
        return TError(EError::InvalidState, "invalid container state " +
                      ContainerStateName(state));

    TError error = CheckPausedParent();
    if (error)
        return error;

    auto vmode = Prop->Get<int>(P_VIRT_MODE);
    if (vmode == VIRT_MODE_OS && !CredConf.PrivilegedUser(OwnerCred)) {
        for (auto name : Prop->List())
            if (Prop->Find(name)->GetFlags() & OS_MODE_PROPERTY)
                Prop->Reset(name);
    }

    if (!meta && !Prop->Get<std::string>(P_COMMAND).length())
        return TError(EError::InvalidValue, "container command is empty");

    // since we now have a complete picture of properties, check
    // them once again (so we don't miss something due to set order)
    for (auto name : Prop->List()) {
        if (Prop->IsDefault(name))
            continue;

        TError error = Prop->FromString(name, Prop->ToString(name), false);
        if (error)
            return error;
    }

    L_ACT() << "Start " << GetName() << " " << Id << std::endl;

    error = Data->Set<uint64_t>(D_RESPAWN_COUNT, 0);
    if (error)
        return error;

    error = Data->Set<int>(D_EXIT_STATUS, -1);
    if (error)
        return error;

    error = Data->Set<bool>(D_OOM_KILLED, false);
    if (error)
        return error;

    error = Prop->Set<uint64_t>(P_RAW_START_TIME, GetCurrentTimeMs());
    if (error)
        return error;

    error = PrepareResources();
    if (error)
        return error;

    if (!meta || (meta && Prop->Get<bool>(P_ISOLATE))) {

        error = PrepareTask(client);
        if (error)
            goto error;

        error = Task->Start();
        if (error) {
            TError e = Data->Set<int>(D_START_ERRNO, error.GetErrno());
            if (e)
                L_ERR() << "Can't set start_errno: " << e << std::endl;
            goto error;
        }

        error = Data->Set<int>(D_START_ERRNO, -1);
        if (error)
            goto error;

        L() << GetName() << " started " << std::to_string(Task->GetPid()) << std::endl;

        error = Prop->Set<std::vector<int>>(P_RAW_ROOT_PID, Task->GetPids());
        if (error)
            goto error;
    }

    IsMeta = meta;
    if (meta)
        SetState(EContainerState::Meta);
    else
        SetState(EContainerState::Running);
    Statistics->Started++;
    error = UpdateSoftLimit();
    if (error)
        L_ERR() << "Can't update meta soft limit: " << error << std::endl;

    return TError::Success();

error:
    FreeResources();
    return error;
}

TError TContainer::Freeze(TScopedLock &holder_lock) {
    auto cg = GetLeafCgroup(freezerSubsystem);
    TError error = freezerSubsystem->Freeze(cg);
    if (error) {
        L_ERR() << "Can't freeze container: " << error << std::endl;
        return error;
    }

    {
        TScopedUnlock unlock(holder_lock);
        error = freezerSubsystem->WaitForFreeze(cg);
        if (error) {
            L_ERR() << "Can't wait for freeze container: " << error << std::endl;
            return error;
        }
    }

    return error;
}

TError TContainer::Unfreeze(TScopedLock &holder_lock) {
    auto cg = GetLeafCgroup(freezerSubsystem);
    TError error = freezerSubsystem->Unfreeze(cg);
    if (error) {
        L_ERR() << "Can't unfreeze container: " << error << std::endl;
        return error;
    }

    {
        TScopedUnlock unlock(holder_lock);
        error = freezerSubsystem->WaitForUnfreeze(cg);
        if (error) {
            L_ERR() << "Can't wait for unfreeze container: " << error << std::endl;
            return error;
        }
    }

    return error;
}

bool TContainer::IsFrozen() {
    auto cg = GetLeafCgroup(freezerSubsystem);
    return freezerSubsystem->IsFrozen(cg);
}

bool TContainer::IsValid() {
    return GetState() != EContainerState::Unknown;
}

TError TContainer::SendSignal(int signal) {
    L_ACT() << "Send signal " << signal << " to " << GetName() << std::endl;

    vector<pid_t> reap;
    auto cg = GetLeafCgroup(freezerSubsystem);
    TError error = cg->GetTasks(reap);
    if (error)
        return error;

    return cg->Kill(signal);
}

TError TContainer::SendTreeSignal(TScopedLock &holder_lock, int signal) {
    TError error = CheckPausedParent();
    if (error)
        return error;

    ApplyForTreePostorder(holder_lock, [] (TScopedLock &holder_lock,
                                           TContainer &child) {
        (void)child.SendSignal(SIGKILL);
        return TError::Success();
    });
    (void)SendSignal(SIGKILL);

    return TError::Success();
}

TError TContainer::KillAll(TScopedLock &holder_lock) {
    auto cg = GetLeafCgroup(freezerSubsystem);

    L_ACT() << "Kill all " << GetName() << std::endl;

    // we can't unfreeze child if parent is frozen, so just
    // send SIGKILL to all tasks, mark container as dead and hope that
    // when parent container is thawed these tasks will die
    if (CheckPausedParent() != TError::Success()) {
        L_WRN() << "Container " << GetName() << " can't be thawed, just send SIGKILL" << std::endl;
        return SendSignal(SIGKILL);
    }

    // try to stop all tasks gracefully
    if (!SendSignal(SIGTERM)) {
        TScopedUnlock unlock(holder_lock);
        int ret = SleepWhile(config().container().kill_timeout_ms(),
                             [&]{ return cg->IsEmpty() == false; });
        if (ret)
            L() << "Child didn't exit via SIGTERM, sending SIGKILL" << std::endl;
    }

    TError error = Freeze(holder_lock);
    if (error)
        return error;

    // then kill any task that didn't want to stop via SIGTERM signal;
    // freeze all container tasks to make sure no one forks and races with us
    error = SendSignal(SIGKILL);
    if (error) {
            (void)Unfreeze(holder_lock);
            return error;
    }

    return Unfreeze(holder_lock);
}

TError TContainer::ApplyForTreePreorder(TScopedLock &holder_lock,
                                std::function<TError (TScopedLock &holder_lock,
                                                      TContainer &container)> fn) {
    for (auto iter : Children)
        if (auto child = iter.lock()) {
            TNestedScopedLock lock(*child, holder_lock);
            if (child->IsValid()) {
                TError error = fn(holder_lock, *child);
                if (error)
                    return error;

                error = child->ApplyForTreePreorder(holder_lock, fn);
                if (error)
                    return error;
            }
        }

    return TError::Success();
}

TError TContainer::ApplyForTreePostorder(TScopedLock &holder_lock,
                                std::function<TError (TScopedLock &holder_lock,
                                                      TContainer &container)> fn) {
    for (auto iter : Children)
        if (auto child = iter.lock()) {
            TNestedScopedLock lock(*child, holder_lock);
            if (child->IsValid()) {
                TError error = child->ApplyForTreePostorder(holder_lock, fn);
                if (error)
                    return error;

                error = fn(holder_lock, *child);
                if (error)
                    return error;
            }
        }

    return TError::Success();
}

TError TContainer::PrepareLoop() {
    TPath loop_image(Prop->Get<std::string>(P_ROOT));
    if (loop_image.GetType() != EFileType::Regular)
        return TError::Success();

    TError error;
    TPath temp_path = GetTmpDir();
    if (!temp_path.Exists()) {
        error = temp_path.Mkdir(0755);
        if (error)
            return error;
    }

    if (Prop->IsDefault(P_ROOT) ||
            loop_image == Parent->Prop->Get<std::string>(P_ROOT))
        return TError::Success();

    int loop_dev;
    error = SetupLoopDevice(loop_image, loop_dev);
    if (error)
        return error;

    error = Prop->Set<int>(P_RAW_LOOP_DEV, loop_dev);
    if (error)
        (void)PutLoopDev(loop_dev);

    return error;
}

TError TContainer::PrepareResources() {
    TError error = PrepareNetwork();
    if (error) {
        L_ERR() << "Can't prepare task network: " << error << std::endl;
        FreeResources();
        return error;
    }

    error = PrepareCgroups();
    if (error) {
        L_ERR() << "Can't prepare task cgroups: " << error << std::endl;
        FreeResources();
        return error;
    }

    error = PrepareLoop();
    if (error) {
        L_ERR() << "Can't prepare loop device: " << error << std::endl;
        FreeResources();
        return error;
    }

    return TError::Success();
}

void TContainer::RemoveLog(const TPath &path) {
    if (path.GetType() != EFileType::Character &&
        path.GetType() != EFileType::Block) {
        TFile f(path);
        if (f.Exists()) {
            TError error = f.Remove();
            if (error)
                L_ERR() << "Can't remove stdio file " << path << ": " << error << std::endl;
        }
    }
}

void TContainer::FreeResources() {
    LeafCgroups.clear();

    if (!Parent || Parent->Tclass != Tclass) {
        auto lock = Net->ScopedLock();

        TError error = Tclass->Remove();
        if (error)
            L_ERR() << "Can't remove tc classifier: " << error << std::endl;
    }
    Tclass = nullptr;
    Task = nullptr;
    ShutdownOom();

    if (IsRoot() || IsPortoRoot())
        return;

    if (Prop->IsDefault(P_STDOUT_PATH))
        RemoveLog(Prop->Get<std::string>(P_STDOUT_PATH));

    if (Prop->IsDefault(P_STDERR_PATH))
        RemoveLog(Prop->Get<std::string>(P_STDERR_PATH));

    int loopNr = Prop->Get<int>(P_RAW_LOOP_DEV);

    TError error = Prop->Set<int>(P_RAW_LOOP_DEV, -1);
    if (error)
        L_ERR() << "Can't set " << P_RAW_LOOP_DEV << ": " << error << std::endl;

    if (loopNr >= 0) {
        error = PutLoopDev(loopNr);
        if (error)
            L_ERR() << "Can't put loop device " << loopNr << ": " << error << std::endl;
    }

    TPath temp_path = GetTmpDir();
    if (temp_path.Exists()) {
        error = temp_path.Rmdir();
        if (error)
            L_ERR() << "Can't remove " << temp_path << ": " << error << std::endl;
    }
}

void TContainer::AcquireForced() {
    if (config().log().verbose())
        L() << "Acquire " << GetName() << " (forced)" << std::endl;
    Acquired++;
}

bool TContainer::Acquire() {
    if (!IsAcquired()) {
        if (config().log().verbose())
            L() << "Acquire " << GetName() << std::endl;
        Acquired++;
        return true;
    }
    return false;
}

void TContainer::Release() {
    if (config().log().verbose())
        L() << "Release " << GetName() << std::endl;
    PORTO_ASSERT(Acquired > 0);
    Acquired--;
}

bool TContainer::IsAcquired() const {
    return (Acquired || (Parent && Parent->IsAcquired()));
}

TError TContainer::Stop(TScopedLock &holder_lock) {
    auto state = GetState();

    if (state == EContainerState::Stopped ||
        state == EContainerState::Paused)
        return TError(EError::InvalidState, "invalid container state " + ContainerStateName(state));

    L_ACT() << "Stop " << GetName() << " " << Id << std::endl;

    ShutdownOom();

    if (Task && Task->IsRunning()) {
        TError error = KillAll(holder_lock);
        if (error) {
            L_ERR() << "Can't kill all tasks in container: " << error << std::endl;
            return error;
        }

        auto cg = GetLeafCgroup(freezerSubsystem);

        TScopedUnlock unlock(holder_lock);
        int ret = SleepWhile(config().container().stop_timeout_ms(),
                             [&] () -> int {
                                 if (cg && cg->IsEmpty())
                                     return 0;
                                 kill(Task->GetPid(), 0);
                                 return errno != ESRCH;
                             });
        if (ret) {
            L_ERR() << "Can't wait for container to stop" << std::endl;
            return TError(EError::Unknown, "Container didn't stop in " + std::to_string(config().container().stop_timeout_ms()) + "ms");
        }

        Task->Exit(-1);
    }

    SetState(EContainerState::Stopped);
    FreeResources();

    return TError::Success();
}

TError TContainer::CheckPausedParent() {
    for (auto p = Parent; p; p = p->Parent)
        if (p->GetState() == EContainerState::Paused)
            return TError(EError::InvalidState, "parent " + p->GetName() + " is paused");
    return TError::Success();
}

TError TContainer::CheckAcquiredChild(TScopedLock &holder_lock) {
    return ApplyForTreePreorder(holder_lock, [] (TScopedLock &holder_lock,
                                                 TContainer &child) {
        if (child.Acquired)
            return TError(EError::Busy, "child " + child.GetName() + " is busy");
        return TError::Success();
    });
}

TError TContainer::Pause(TScopedLock &holder_lock) {
    auto state = GetState();
    if (state != EContainerState::Running)
        return TError(EError::InvalidState, "invalid container state " +
                      ContainerStateName(state));

    // some child subtree may be in stop/destroy and we don't want
    // to freeze parent in that moment
    TError error = CheckAcquiredChild(holder_lock);
    if (error)
        return error;

    error = Freeze(holder_lock);
    if (error)
        return error;

    SetState(EContainerState::Paused);
    ApplyForTreePreorder(holder_lock, [&] (TScopedLock &holder_lock,
                                           TContainer &child) {
        if (child.GetState() == EContainerState::Running ||
            child.GetState() == EContainerState::Meta) {
            child.SetState(EContainerState::Paused);
            child.Journal("paused", shared_from_this());
        }
        return TError::Success();
    });
    return TError::Success();
}

TError TContainer::Resume(TScopedLock &holder_lock) {
    auto state = GetState();

    // container may be dead by still frozen
    if (!IsFrozen())
        return TError(EError::InvalidState, "invalid container state " +
                      ContainerStateName(state));

    TError error = CheckPausedParent();
    if (error)
        return error;

    error = Unfreeze(holder_lock);
    if (error) {
        L_ERR() << "Can't resume " << GetName() << ": " << error << std::endl;
        return error;
    }

    if (GetState() == EContainerState::Paused)
        SetState(EContainerState::Running);

    ApplyForTreePreorder(holder_lock, [&] (TScopedLock &holder_lock,
                                           TContainer &child) {
        if (child.GetState() == EContainerState::Paused) {
            child.SetState(EContainerState::Running);
            child.Journal("resumed", shared_from_this());
        }
        return TError::Success();
    });
    return TError::Success();
}

TError TContainer::Kill(int sig) {
    L_ACT() << "Kill " << GetName() << " " << Id << std::endl;

    auto state = GetState();
    if (state != EContainerState::Running)
        return TError(EError::InvalidState, "invalid container state " +
                      ContainerStateName(state));

    return Task->Kill(sig);
}

void TContainer::ParsePropertyName(std::string &name, std::string &idx) {
    std::vector<std::string> tokens;
    TError error = SplitString(name, '[', tokens);
    if (error || tokens.size() != 2)
        return;

    name = tokens[0];
    idx = StringTrim(tokens[1], " \t\n]");
}

TError TContainer::GetData(const string &origName, string &value,
                           std::shared_ptr<TClient> client) {
    std::string name = origName;
    std::string idx;
    ParsePropertyName(name, idx);

    if (!Data->IsValid(name))
        return TError(EError::InvalidData, "invalid container data");

    auto cv = ToContainerValue(Data->Find(name));
    if (!cv->IsImplemented())
        return TError(EError::NotSupported, name + " is not implemented");

    auto validState = cv->GetState();
    if (validState.find(GetState()) == validState.end())
        return TError(EError::InvalidState, "invalid container state");

    if (name == D_ROOT_PID && Task && client) {
        value = std::to_string(Task->GetPidFor(client->GetPid()));
        return TError::Success();
    }

    if (idx.length()) {
        TUintMap m = Data->Get<TUintMap>(name);
        if (m.find(idx) == m.end())
            return TError(EError::InvalidValue, "invalid index " + idx);

        value = std::to_string(m.at(idx));
    } else {
        value = Data->ToString(name);
    }

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
                property = P_CPU_POLICY;
                value = "normal";
            } else {
                property = P_CPU_POLICY;
                value = "rt";
            }
        } else if (property == "memory.limit_in_bytes") {
            property = P_MEM_LIMIT;
        } else if (property == "memory.low_limit_in_bytes") {
            property = P_MEM_GUARANTEE;
        } else if (property == "memory.recharge_on_pgfault") {
            property = P_RECHARGE_ON_PGFAULT;
            value = value == "0" ? "false" : "true";
        }

        return TError::Success();
}

static std::map<std::string, std::string> alias = {
    { "cpu.smart", P_CPU_POLICY },
    { "memory.limit_in_bytes", P_MEM_LIMIT },
    { "memory.low_limit_in_bytes", P_MEM_GUARANTEE },
    { "memory.recharge_on_pgfault", P_RECHARGE_ON_PGFAULT },
};

TError TContainer::GetProperty(const string &origProperty, string &value,
                               std::shared_ptr<TClient> client) const {
    if (IsRoot() || IsPortoRoot())
        return TError(EError::InvalidProperty, "no properties for container " + GetName());

    string property = origProperty;
    std::string idx;
    ParsePropertyName(property, idx);

    if (alias.find(origProperty) != alias.end())
        property = alias.at(origProperty);

    TError error = Prop->Check(property);
    if (error)
        return error;

    if (!Prop->IsImplemented(property))
        return TError(EError::NotSupported, property + " is not implemented");

    if (idx.length()) {
        TUintMap m;
        TError error = Prop->GetChecked<TUintMap>(property, m);
        if (error)
            return TError(EError::InvalidValue, "Invalid subscript for property");

        if (m.find(idx) == m.end())
            return TError(EError::InvalidValue, "invalid index " + idx);

        value = std::to_string(m.at(idx));
    } else {
        value = Prop->ToString(property);
    }
    PropertyToAlias(origProperty, value);

    if (Prop->HasFlags(property, PATH_PROPERTY) && client) {
        std::shared_ptr<TContainer> clientContainer;
        TError error = client->GetContainer(clientContainer);
        if (error)
            return error;
        TPath clientRoot = clientContainer->RootPath();
        if (clientRoot.IsEmpty())
            return TError(EError::InvalidValue, "Cannot get client root path");
        value = clientRoot.InnerPath(value, true).ToString();
    }

    return TError::Success();
}

bool TContainer::ShouldApplyProperty(const std::string &property) {
    if (!Prop->HasState(property, EContainerState::Running))
       return false;

    auto state = GetState();
    if (state == EContainerState::Dead || state == EContainerState::Stopped)
        return false;

    if (property == P_PRIVATE)
        return false;

    return true;
}

TError TContainer::SetProperty(const string &origProperty,
                               const string &origValue,
                               std::shared_ptr<TClient> client) {
    if (IsRoot() || IsPortoRoot())
        return TError(EError::InvalidValue, "Can't set property for container " + GetName());

    string property = origProperty;
    std::string idx;
    ParsePropertyName(property, idx);
    string value = StringTrim(origValue);

    TError error = AliasToProperty(property, value);
    if (error)
        return error;

    error = Prop->Check(property);
    if (error)
        return error;

    if (!Prop->IsImplemented(property))
        return TError(EError::NotSupported, property + " is not implemented");

    bool superuser = client && client->GetCred().IsPrivileged();

    if (Prop->HasFlags(property, SUPERUSER_PROPERTY) && !superuser)
        if (Prop->ToString(property) != value)
            return TError(EError::Permission, "Only root can change this property");

    if (Prop->HasFlags(property, RESTROOT_PROPERTY) && !superuser && !CredConf.RestrictedUser(OwnerCred))
        return TError(EError::Permission, "Only restricted root can change this property");

    if (Prop->HasFlags(property, PATH_PROPERTY) && client) {
        std::shared_ptr<TContainer> clientContainer;
        TError error = client->GetContainer(clientContainer);
        if (error)
            return error;
        TPath clientRoot = clientContainer->RootPath();
        if (clientRoot.IsEmpty())
            return TError(EError::InvalidValue, "Cannot get client root path");
        value = (clientRoot / value).ToString();
    }

    if (!Prop->HasState(property, GetState()))
        return TError(EError::InvalidState, "Can't set dynamic property " + property + " in state " + ContainerStateName(GetState()));

    if (Parent && !Parent->IsRoot() && !Parent->IsPortoRoot() &&
        !Prop->GetRaw<bool>(P_ISOLATE) && Prop->HasFlags(property, PARENT_RO_PROPERTY))
        return TError(EError::NotSupported, "Can't set " + property + " for child container");

    if (idx.length()) {
        TUintMap m;
        TError error = Prop->GetChecked<TUintMap>(property, m);
        if (error)
            return TError(EError::InvalidValue, "Invalid subscript for property");

        if (m.find(idx) == m.end()) {
            return TError(EError::InvalidValue, "Invalid index " + idx);
        } else {
            uint64_t uval;
            TError error = StringToUint64(value, uval);
            if (error)
                return TError(EError::InvalidValue, "Invalid integer value for index " + idx);

            m[idx] = uval;
            error = Prop->Set<TUintMap>(property, m);
            if (error)
                return error;
        }
    } else {
        error = Prop->FromString(property, value);
        if (error)
            return error;
    }

    if (ShouldApplyProperty(property)) {
        error = ApplyDynamicProperties();
        if (error)
            return error;
    }

    // Write KVS snapshot, otherwise it may grow indefinitely and on next
    // restart we will merge it forever

    error = Prop->Flush();
    if (error)
        return error;

    error = Prop->Sync();
    if (error)
        return error;

    error = Data->Sync();

    return error;
}

TError TContainer::Prepare() {
    std::shared_ptr<TKeyValueNode> kvnode;
    if (!IsRoot() && !IsPortoRoot())
        kvnode = Storage->GetNode(Id);

    Prop = std::make_shared<TPropertyMap>(kvnode, shared_from_this());
    Data = std::make_shared<TValueMap>(kvnode);
    if (!Prop || !Data)
        throw std::bad_alloc();

    RegisterData(Data, shared_from_this());
    RegisterProperties(Prop, shared_from_this());

    if (Name == ROOT_CONTAINER) {
        auto dataList = Data->List();
        auto propList = Prop->List();

        for (auto name : dataList)
            if (std::find(propList.begin(), propList.end(), name) != propList.end())
                return TError(EError::Unknown, "Data and property names conflict: " + name);
    }

    TError error = Prop->Create();
    if (error)
        return error;

    error = Data->Create();
    if (error)
        return error;

    if (!Data->HasValue(D_START_ERRNO)) {
        error = Data->Set<int>(D_START_ERRNO, -1);
        if (error)
            return error;
    }

    error = Prop->Set<std::string>(P_RAW_NAME, GetName());
    if (error)
        return error;

    error = Prop->Set<int>(P_RAW_ID, (int)Id);
    if (error)
        return error;

    CgroupEmptySince = 0;

    return TError::Success();
}

TError TContainer::Restore(TScopedLock &holder_lock, const kv::TNode &node) {
    L_ACT() << "Restore " << GetName() << " with id " << Id << std::endl;

    TError error = Prepare();
    if (error)
        return error;

    error = Prop->Restore(node);
    if (error)
        return error;

    error = Data->Restore(node);
    if (error)
        return error;

    error = Prop->Flush();
    if (error)
        return error;

    error = Data->Flush();
    if (error)
        return error;

    error = Prop->Sync();
    if (error)
        return error;

    error = Data->Sync();
    if (error)
        return error;

    // There are several points where we save value to the persistent store
    // which we may use as indication for events like:
    // - Container create failed
    // - Container create succeed
    // - Container start failed
    // - Container start succeed
    //
    // -> Create
    // { SET user, group
    // } SET state -> stopped
    //
    // -> Start
    // { SET respawn_count, oom_killed, start_errno
    // } SET state -> running

    bool created = Data->HasValue(D_STATE);
    if (!created)
        return TError(EError::Unknown, "Container has not been created");

    bool started = Prop->HasValue(P_RAW_ROOT_PID);
    if (started) {
        std::vector<int> pids = Prop->Get<std::vector<int>>(P_RAW_ROOT_PID);

        if (pids.size() == 1)
            pids = {pids[0], 0, pids[0]};

        if (pids[0] == GetPid())
            pids = {0, 0, 0};

        L_ACT() << GetName() << ": restore started container " << pids[0] << std::endl;

        auto parent = Parent;
        while (parent && !parent->IsRoot() && !parent->IsPortoRoot()) {
            if (parent->GetState() == EContainerState::Running ||
                parent->GetState() == EContainerState::Meta ||
                parent->GetState() == EContainerState::Dead)
                break;
            bool meta = parent->Prop->Get<std::string>(P_COMMAND).empty();

            L() << "Start parent " << parent->GetName() << " meta " << meta << std::endl;

            TError error = parent->Start(nullptr, meta);
            if (error)
                return error;

            parent = parent->Parent;
        }

        TError error = PrepareResources();
        if (error)
            return error;

        error = PrepareTask(nullptr);
        if (error) {
            FreeResources();
            return error;
        }

        Task->Restore(pids);

        if (Task->HasCorrectParent()) {
            if (Task->IsZombie()) {
                    L() << "Task is zombie and belongs to porto" << std::endl;
            } else {
                if (Task->HasCorrectFreezer()) {
                    L() << "Task is running and belongs to porto" << std::endl;

                    TError error = Task->FixCgroups();
                    if (error)
                        L_WRN() << "Can't fix cgroups: " << error << std::endl;
                } else {
                    L_WRN() << "Task is running, belongs to porto but doesn't have valid freezer" << std::endl;
                    LostAndRestored = true;
                }
            }
        } else {
            if (Task->HasCorrectFreezer()) {
                L() << "Task is dead or doesn't belong to porto" << std::endl;
                LostAndRestored = true;
            } else {
                L() << "Task is not running or has been reparented" << std::endl;
                LostAndRestored = true;
            }
        }

        auto state = Data->Get<std::string>(D_STATE);
        if (state == ContainerStateName(EContainerState::Dead)) {
            // we started recording death time since porto v1.15,
            // use some sensible default
            if (!Prop->HasValue(P_RAW_DEATH_TIME))
                Prop->Set<uint64_t>(P_RAW_DEATH_TIME, GetCurrentTimeMs());

            SetState(EContainerState::Dead);
        } else {
            // we started recording start time since porto v1.15,
            // use some sensible default
            if (!Prop->HasValue(P_RAW_START_TIME))
                Prop->Set<uint64_t>(P_RAW_START_TIME, GetCurrentTimeMs());

            if (Prop->Get<std::string>(P_COMMAND).empty())
                SetState(EContainerState::Meta);
            else
                SetState(EContainerState::Running);

            auto cg = GetLeafCgroup(freezerSubsystem);
            if (freezerSubsystem->IsFrozen(cg))
                SetState(EContainerState::Paused);
        }

        if (!Task->IsZombie())
            SyncStateWithCgroup(holder_lock);

        if (MayRespawn())
            ScheduleRespawn();
    } else {
        L_ACT() << GetName() << ": restore created container " << std::endl;

        // we didn't report to user that we started container,
        // make sure nobody is running

        auto cg = GetLeafCgroup(freezerSubsystem);
        TError error = cg->Create();
        if (error)
            (void)KillAll(holder_lock);

        SetState(EContainerState::Stopped);
        Task = nullptr;
    }

    if (GetState() == EContainerState::Stopped) {
        if (Prop->IsDefault(P_STDOUT_PATH))
            RemoveLog(Prop->Get<std::string>(P_STDOUT_PATH));
        if (Prop->IsDefault(P_STDERR_PATH))
            RemoveLog(Prop->Get<std::string>(P_STDERR_PATH));
    }

    if (Task)
        Task->ClearEnv();

    if (Parent)
        Parent->AddChild(shared_from_this());

    Journal("restored");

    return TError::Success();
}

std::shared_ptr<TCgroup> TContainer::GetLeafCgroup(shared_ptr<TSubsystem> subsys) {
    if (LeafCgroups.find(subsys) != LeafCgroups.end())
        return LeafCgroups[subsys];

    if (IsRoot())
        return subsys->GetRootCgroup();
    else if (IsPortoRoot())
        return subsys->GetRootCgroup()->GetChild(PORTO_ROOT_CGROUP);

    return Parent->GetLeafCgroup(subsys)->GetChild(Name);
}

void TContainer::ExitTree(TScopedLock &holder_lock, int status, bool oomKilled) {
    L_EVT() << "Exit tree " << GetName() << " with status "
            << status << (oomKilled ? " invoked by OOM" : "")
            << std::endl;

    if (!oomKilled && !Processes().empty() && Prop->Get<bool>(P_ISOLATE) == true) {
        L_WRN() << "Skipped bogus exit event (" << status << "), some process is still alive in " << GetName() << std::endl;
        return;
    }

    TError error = CheckPausedParent();
    if (error)
        L() << "Exit tree while parent is paused" << std::endl;

    if (IsFrozen())
        (void)Resume(holder_lock);

    ApplyForTreePreorder(holder_lock, [&] (TScopedLock &holder_lock,
                                           TContainer &child) {
        if (child.IsFrozen())
            (void)child.Resume(holder_lock);

        child.Exit(holder_lock, status, oomKilled);
        Journal(oomKilled ? "killed by OOM" : "killed", shared_from_this());
        return TError::Success();
    });

    Exit(holder_lock, status, oomKilled);
    if (oomKilled)
        Journal("killed by OOM");
    else
        Journal("init process quitted, exit code " + std::to_string(status));
}

void TContainer::Exit(TScopedLock &holder_lock, int status, bool oomKilled) {
    if (!Task)
        return;

    L_EVT() << "Exit " << GetName() << " (root_pid " << Task->GetPid() << ")"
            << " with status " << status << (oomKilled ? " invoked by OOM" : "")
            << std::endl;

    ShutdownOom();

    TError error = Data->Set<int>(D_EXIT_STATUS, status);
    if (error)
        L_ERR() << "Can't set " << D_EXIT_STATUS << ": " << error << std::endl;

    error = Prop->Set<uint64_t>(P_RAW_DEATH_TIME, GetCurrentTimeMs());
    if (error)
        L_ERR() << "Can't set " << P_RAW_DEATH_TIME << ": " << error << std::endl;

    if (oomKilled) {
        L_EVT() << Task->GetPid() << " killed by OOM" << std::endl;

        TError error = Data->Set<bool>(D_OOM_KILLED, true);
        if (error)
            L_ERR() << "Can't set " << D_OOM_KILLED << ": " << error << std::endl;

        error = KillAll(holder_lock);
        if (error)
            L_WRN() << "Can't kill all tasks in container: " << error << std::endl;
    }

    if (!Prop->Get<bool>(P_ISOLATE)) {
        TError error = KillAll(holder_lock);
        if (error)
            L_WRN() << "Can't kill all tasks in non-isolated container: " << error << std::endl;
    }

    Task->Exit(status);
    SetState(EContainerState::Dead);

    error = Prop->Set<std::vector<int>>(P_RAW_ROOT_PID, {0, 0, 0});
    if (error)
        L_ERR() << "Can't set " << P_RAW_ROOT_PID << ": " << error << std::endl;

    if (MayRespawn())
        ScheduleRespawn();
}

bool TContainer::MayExit(int pid) {
    if (!Task)
        return false;

    if (Task->GetWPid() != pid)
        return false;

    if (GetState() == EContainerState::Dead)
        return false;

    return true;
}

bool TContainer::MayRespawn() {
    if (GetState() != EContainerState::Dead)
        return false;

    if (!Prop->Get<bool>(P_RESPAWN))
        return false;

    return Prop->Get<int>(P_MAX_RESPAWNS) < 0 || Data->Get<uint64_t>(D_RESPAWN_COUNT) < (uint64_t)Prop->Get<int>(P_MAX_RESPAWNS);
}

bool TContainer::MayReceiveOom(int fd) {
    if (Efd.GetFd() != fd)
        return false;

    if (!Task)
        return false;

    if (GetState() == EContainerState::Dead)
        return false;

    return true;
}

void TContainer::ScheduleRespawn() {
    TEvent e(EEventType::Respawn, shared_from_this());
    Holder->Queue->Add(config().container().respawn_delay_ms(), e);
}

TError TContainer::Respawn(TScopedLock &holder_lock) {
    TScopedAcquire acquire(shared_from_this());
    if (!acquire.IsAcquired())
        return TError(EError::Busy, "Can't respawn busy container");

    TError error = StopTree(holder_lock);
    if (error)
        return error;

    uint64_t tmp = Data->Get<uint64_t>(D_RESPAWN_COUNT);
    error = Start(nullptr, false);
    Data->Set<uint64_t>(D_RESPAWN_COUNT, tmp + 1);
    if (error)
        return error;

    Journal("respawned");
    return TError::Success();
}

bool TContainer::CanRemoveDead() const {
    return State == EContainerState::Dead &&
        Prop->Get<uint64_t>(P_RAW_DEATH_TIME) / 1000 +
        Prop->Get<uint64_t>(P_AGING_TIME) <= GetCurrentTimeMs() / 1000;
}

std::vector<std::string> TContainer::GetChildren() {
    std::vector<std::string> vec;

    for (auto weakChild : Children)
        if (auto child = weakChild.lock())
            vec.push_back(child->GetName());

    return vec;
}

TError TContainer::RotateLog(const TPath &path) {
    off_t max_log_size = config().container().max_log_size();

    if (path.GetType() == EFileType::Regular) {
        TFile file(path);

        TError error = file.RotateLog(max_log_size);
        if (error)
            return error;
    }

    return TError::Success();
}

void TContainer::DeliverEvent(TScopedLock &holder_lock, const TEvent &event) {
    TError error;
    switch (event.Type) {
        case EEventType::Exit:
            ExitTree(holder_lock, event.Exit.Status, FdHasEvent(Efd.GetFd()));
            break;
        case EEventType::RotateLogs:
            if (GetState() == EContainerState::Running && Task) {
                TError error = RotateLog(Prop->Get<std::string>(P_STDOUT_PATH));
                if (error)
                    L_ERR() << "Can't rotate stdout: " << error << std::endl;

                error = RotateLog(Prop->Get<std::string>(P_STDERR_PATH));
                if (error)
                    L_ERR() << "Can't rotate stderr: " << error << std::endl;
            }
            JournalStream.close();
            break;
        case EEventType::Respawn:
            error = Respawn(holder_lock);
            if (error)
                L_WRN() << "Can't respawn container: " << error << std::endl;
            else
                L() << "Respawned " << GetName() << std::endl;
            break;
        case EEventType::OOM:
            ExitTree(holder_lock, SIGKILL, true);
            break;
        default:
            break;
    }
}

TError TContainer::CheckPermission(const TCred &ucred) {
    // for root we report more meaningful errors from handlers, so don't
    // check permissions here
    if (IsRoot() || IsPortoRoot())
        return TError::Success();

    if (ucred.IsPermitted(OwnerCred))
        return TError::Success();

    return TError(EError::Permission, "Permission error");
}

std::string TContainer::GetPortoNamespace() const {
    if (Parent)
        return Parent->GetPortoNamespace() + Prop->Get<std::string>(P_PORTO_NAMESPACE);
    else
        return "";
}

TError TContainer::RelativeName(const TContainer &c, std::string &name) const {
    std::string ns = GetPortoNamespace();
    if (c.IsRoot()) {
        name = ROOT_CONTAINER;
        return TError::Success();
    } else if (ns == "") {
        name = c.GetName();
        return TError::Success();
    } else {
        std::string n = c.GetName();
        if (n.length() <= ns.length() || n.compare(0, ns.length(), ns) != 0) {
            return TError(EError::ContainerDoesNotExist,
                          "Can't access container " + n + " from namespace " + ns);
        }

        name = n.substr(ns.length());
        return TError::Success();
    }
}

TError TContainer::AbsoluteName(const std::string &orig, std::string &name,
                                bool resolve_meta) const {
    if (!resolve_meta && (orig == DOT_CONTAINER || orig == PORTO_ROOT_CONTAINER ||
                          orig == ROOT_CONTAINER))
        return TError(EError::Permission,
                      "Meta containers (like . and /) are provided in read-only mode");

    std::string ns = GetPortoNamespace();
    if (orig == ROOT_CONTAINER || orig == PORTO_ROOT_CONTAINER)
        name = orig;
    else if (orig == DOT_CONTAINER) {
        size_t off = ns.rfind('/');
        if (off != std::string::npos) {
            name = ns.substr(0, off);
        } else
            name = PORTO_ROOT_CONTAINER;
    } else
        name = ns + orig;

    return TError::Success();
}

void TContainer::AddWaiter(std::shared_ptr<TContainerWaiter> waiter) {
    if (GetState() == EContainerState::Running) {
        CleanupWaiters();
        Waiters.push_back(waiter);
    } else {
        waiter->Signal(this);
    }
}

void TContainer::NotifyWaiters() {
    if (GetState() != EContainerState::Running) {
        CleanupWaiters();
        for (auto &w : Waiters) {
            auto waiter = w.lock();
            if (waiter)
                waiter->Signal(this);
        }
    }
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

TError TContainer::UpdateNetwork() {
    if (Tclass) {
        TUintMap prio, rate, ceil;
        prio = Prop->Get<TUintMap>(P_NET_PRIO);
        rate = Prop->Get<TUintMap>(P_NET_GUARANTEE);
        ceil = Prop->Get<TUintMap>(P_NET_LIMIT);

        Tclass->Prepare(prio, rate, ceil);

        auto net_lock = Net->ScopedLock();
        return Tclass->Create();
    }
    return TError::Success();
}

bool TContainer::PrepareJournal() {
    if (!JournalStream.is_open()) {
        std::string filename = config().journal_dir().path() + GetName(true, "+");

        TFile f(filename);
        if (!f.Exists())
            f.Touch();
        f.GetPath().Chown(OwnerCred);
        f.GetPath().Chmod(0644);
        f.RotateLog(1024 * 1024);

        JournalStream.open(filename, std::ios_base::app);
        if (!JournalStream.is_open()) {
            L_WRN() << "Can't open " << filename << " for writing container journal"
                    << std::endl;
            return false;
        }
    }
    return true;
}

void TContainer::Journal(const std::string &message) {
    if (PrepareJournal())
        JournalStream << TLogger::GetTime() << " " << message << std::endl;
}

void TContainer::Journal(const std::string &message, std::shared_ptr<TClient> client) {
    if (PrepareJournal())
        JournalStream << TLogger::GetTime() << " " << message
                      << " by " << *client << std::endl;
}

void TContainer::Journal(const std::string &message, std::shared_ptr<TContainer> root) {
    if (PrepareJournal())
        JournalStream << TLogger::GetTime() << " " << message
                      << " due to container " << root->GetName() << std::endl;
}

TContainerWaiter::TContainerWaiter(std::shared_ptr<TClient> client,
                                   std::function<void (std::shared_ptr<TClient>,
                                                       TError, std::string)> callback) :
    Client(client), Callback(callback) {
}

void TContainerWaiter::Signal(const TContainer *who) {
    std::shared_ptr<TClient> client = Client.lock();
    if (client) {
        std::shared_ptr<TContainer> container;
        TError error = client->GetContainer(container);
        if (!error) {
            std::string name;
            TError err = TError::Success();
            if (who)
                err = container->RelativeName(*who, name);
            Callback(client, err, name);
        }

        client->Waiter = nullptr;
    }
}

TError TContainer::StopTree(TScopedLock &holder_lock) {
    if (IsFrozen()) {
        TError error = Resume(holder_lock);
        if (error)
            return error;
    }

    TError error = ApplyForTreePostorder(holder_lock, [&] (TScopedLock &holder_lock,
                                                           TContainer &child) {
        if (child.IsFrozen()) {
            TError error = child.Unfreeze(holder_lock);
            if (error)
                return error;
        }

        if (child.GetState() != EContainerState::Stopped) {
            TError err = child.Stop(holder_lock);
            if (!err && !IsRoot())
                child.Journal("stopped", shared_from_this());
            return err;
        }
        return TError::Success();
    });

    if (error)
        return error;

    error = Stop(holder_lock);
    if (error)
        return error;

    error = UpdateSoftLimit();
    if (error)
        L_ERR() << "Can't update meta soft limit: " << error << std::endl;

    return TError::Success();
}
