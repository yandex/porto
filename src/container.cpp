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
#include "device.hpp"
#include "property.hpp"
#include "data.hpp"
#include "event.hpp"
#include "holder.hpp"
#include "network.hpp"
#include "context.hpp"
#include "container_value.hpp"
#include "epoll.hpp"
#include "kvalue.hpp"
#include "volume.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/cred.hpp"
#include "util/unix.hpp"
#include "client.hpp"
#include "stream.hpp"
#include "kv.pb.h"

extern "C" {
#include <sys/types.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/reboot.h>
#include <fcntl.h>
#include <sys/fsuid.h>
#include <sys/stat.h>
#include <fnmatch.h>
}

std::mutex ContainersMutex;
std::map<std::string, std::shared_ptr<TContainer>> Containers;

using std::string;
using std::vector;
using std::shared_ptr;
using std::unique_ptr;
using std::map;

__thread TContainer *CurrentContainer = nullptr;
__thread TClient *CurrentClient = nullptr;

TContainerUser ContainerUser(P_USER, USER_SET, "Start command with given user");
TContainerGroup ContainerGroup(P_GROUP, GROUP_SET, "Start command with given group");
TContainerMemoryGuarantee ContainerMemoryGuarantee(P_MEM_GUARANTEE, MEM_GUARANTEE_SET,
                                                    "Guaranteed amount of memory "
                                                    "[bytes] (dynamic)");
std::map<std::string, TContainerProperty*> ContainerPropMap;

TContainer::TContainer(std::shared_ptr<TContainerHolder> holder,
                       std::shared_ptr<TKeyValueStorage> storage,
                       const std::string &name, std::shared_ptr<TContainer> parent,
                       int id) :
    Holder(holder), Name(StripParentName(name)), Parent(parent),
    Storage(storage), Id(id), Level(parent == nullptr ? 0 : parent->GetLevel() + 1)
{
    Statistics->Containers++;
    PropMask = 0lu;
    MemGuarantee = 0;
    CurrentMemGuarantee = 0;
}

TContainer::~TContainer() {
    // so call them explicitly in Tcontainer::Destroy()
    PORTO_ASSERT(Net == nullptr);
    Statistics->Containers--;
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

/* Working directory in host namespace */
TPath TContainer::WorkPath() const {
    return TPath(config().container().tmp_dir()) / GetName();
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
        if (path.IsRegularFollow())
            path = GetTmpDir();
        path = path.NormalPath();
    }

    return Parent->RootPath() / path;
}

TPath TContainer::ActualStdPath(const std::string &property, bool host) const {
    TPath path = Prop->Get<std::string>(property);

    if (Prop->IsDefault(property)) {
        /* /dev/null is /dev/null */
        if (path == "/dev/null")
            return path;

        /* By default, std files are located on host in a special folder */
        return WorkPath() / path;
    } else {
	/* Custom std paths are given relative to container root and/or cwd. */
        TPath cwd(Prop->Get<std::string>(P_CWD));
        TPath ret;

        if (host)
            ret = RootPath();

        if (path.IsAbsolute())
            ret /= path;
        else
            ret /= cwd / path;

        return ret;
    }
}

TError TContainer::RotateStdFile(TStdStream &stream, const std::string &type) {
    off_t loss;
    TError error = stream.Rotate(config().container().max_log_size(), loss);
    if (error) {
        L_ERR() << "Can't rotate " + type + ": " << error << std::endl;
    } else if (loss) {
            uint64_t offset = Data->Get<uint64_t>(type);
            Data->Set<uint64_t>(type, offset + loss);
    }

    return error;
}

void TContainer::CreateStdStreams() {
    Stdin = TStdStream(STDIN_FILENO,
                       ActualStdPath(P_STDIN_PATH, false),
                       ActualStdPath(P_STDIN_PATH, true),
                       Prop->IsDefault(P_STDIN_PATH));
    Stdout = TStdStream(STDOUT_FILENO,
                        ActualStdPath(P_STDOUT_PATH, false),
                        ActualStdPath(P_STDOUT_PATH, true),
                        Prop->IsDefault(P_STDOUT_PATH));
    Stderr = TStdStream(STDERR_FILENO,
                        ActualStdPath(P_STDERR_PATH, false),
                        ActualStdPath(P_STDERR_PATH, true),
                        Prop->IsDefault(P_STDERR_PATH));
}

TError TContainer::PrepareStdStreams(std::shared_ptr<TClient> client) {
    TError err = Stdin.Prepare(OwnerCred, client);
    if (err)
        return err;
    err = Stdout.Prepare(OwnerCred, client);
    if (err)
        return err;
    return Stderr.Prepare(OwnerCred, client);
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
    if (Net) {
        auto lock = Net->ScopedLock();
        return Net->GetTrafficCounters(Id, stat, m);
    } else
        return TError(EError::NotSupported, "Network statistics is not available");
}

void TContainer::UpdateRunningChildren(size_t diff) {
    RunningChildren += diff;

    if (!RunningChildren && State == EContainerState::Meta)
        NotifyWaiters();

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

    if (newState != EContainerState::Running && newState != EContainerState::Meta)
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
        L_ERR() << "Can't remove key-value node " << kvnode->Name << ": " << error << std::endl;
}

void TContainer::DestroyVolumes(TScopedLock &holder_lock) {
    if (!VolumeHolder)
        return;

    TScopedUnlock holder_unlock(holder_lock);
    TScopedLock vholder_lock = VolumeHolder->ScopedLock();

    for (auto volume: Volumes) {
        if (!volume->UnlinkContainer(GetName()))
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
        error = volume->Destroy(*VolumeHolder);
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
    if (Net) {
        auto lock = Net->ScopedLock();
        Net = nullptr;
    }
    RemoveKvs();
}

void TContainer::DestroyWeak() {
    if (Prop->Get<bool>(P_WEAK)) {
        TEvent event(EEventType::DestroyWeak, shared_from_this());
        Holder->Queue->Add(0, event);
    }
}

const std::string TContainer::GetName() const {
    if (IsRoot() || IsPortoRoot() || Parent->IsPortoRoot())
        return Name;
    return Parent->GetName() + "/" + Name;
}

const std::string TContainer::GetTextId(const std::string &separator) const {
     if (IsRoot() || IsPortoRoot() || Parent->IsPortoRoot())
         return Name;
     return Parent->GetTextId(separator) + separator + Name;
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

TError TContainer::OpenNetns(TNamespaceFd &netns) const {
    if (Task)
        return netns.Open(Task->GetPid(), "ns/net");
    if (Net == GetRoot()->Net)
        return netns.Open(GetTid(), "ns/net");
    return TError(EError::InvalidValue, "Cannot open netns: container not running");
}

uint64_t TContainer::GetHierarchyMemGuarantee(void) const {
    uint64_t val = 0lu;

    for (auto iter : Children)
        if (auto child = iter.lock())
            val += child->GetHierarchyMemGuarantee();

    return (CurrentMemGuarantee > val) ? CurrentMemGuarantee : val;
}

vector<pid_t> TContainer::Processes() {
    auto cg = GetCgroup(FreezerSubsystem);

    vector<pid_t> ret;
    cg.GetProcesses(ret);
    return ret;
}

TError TContainer::ApplyDynamicProperties() {
    auto memcg = GetCgroup(MemorySubsystem);
    TError error;

    error = MemorySubsystem.SetGuarantee(memcg, MemGuarantee);
    if (error) {
        L_ERR() << "Can't set " << P_MEM_GUARANTEE << ": " << error << std::endl;
        return error;
    }

    error = MemorySubsystem.SetLimit(memcg, Prop->Get<uint64_t>(P_MEM_LIMIT));
    if (error) {
        if (error.GetErrno() == EBUSY)
            return TError(EError::InvalidValue, std::string(P_MEM_LIMIT) + " is too low");

        L_ERR() << "Can't set " << P_MEM_LIMIT << ": " << error << std::endl;
        return error;
    }

    error = MemorySubsystem.SetAnonLimit(memcg, Prop->Get<uint64_t>(P_ANON_LIMIT));
    if (error) {
        L_ERR() << "Can't set " << P_ANON_LIMIT << ": " << error << std::endl;
        return error;
    }

    error = MemorySubsystem.RechargeOnPgfault(memcg, Prop->Get<bool>(P_RECHARGE_ON_PGFAULT));
    if (error) {
        L_ERR() << "Can't set " << P_RECHARGE_ON_PGFAULT << ": " << error << std::endl;
        return error;
    }

    auto cpucg = GetCgroup(CpuSubsystem);
    error = CpuSubsystem.SetCpuPolicy(cpucg,
            Prop->Get<std::string>(P_CPU_POLICY),
            Prop->Get<double>(P_CPU_GUARANTEE),
            Prop->Get<double>(P_CPU_LIMIT));
    if (error) {
        L_ERR() << "Cannot set cpu policy: " << error << std::endl;
        return error;
    }

    auto blkcg = GetCgroup(BlkioSubsystem);
    error = BlkioSubsystem.SetPolicy(blkcg, Prop->Get<std::string>(P_IO_POLICY) == "batch");
    if (error) {
        L_ERR() << "Can't set " << P_IO_POLICY << ": " << error << std::endl;
        return error;
    }

    error = MemorySubsystem.SetIoLimit(memcg, Prop->Get<uint64_t>(P_IO_LIMIT));
    if (error) {
        L_ERR() << "Can't set " << P_IO_LIMIT << ": " << error << std::endl;
        return error;
    }

    error = MemorySubsystem.SetIopsLimit(memcg, Prop->Get<uint64_t>(P_IO_OPS_LIMIT));
    if (error) {
        L_ERR() << "Can't set " << P_IO_OPS_LIMIT << ": " << error << std::endl;
        return error;
    }

    error = MemorySubsystem.SetDirtyLimit(memcg, Prop->Get<uint64_t>(P_DIRTY_LIMIT));
    if (error) {
        L_ERR() << "Can't set " << P_DIRTY_LIMIT << ": " << error << std::endl;
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

void TContainer::ShutdownOom() {
    if (Source)
        Holder->EpollLoop->RemoveSource(Source->Fd);
    OomEventFd = -1;
    Source = nullptr;
}

TError TContainer::PrepareOomMonitor() {
    TCgroup memoryCg = GetCgroup(MemorySubsystem);
    TError error;
    int fd;

    error = MemorySubsystem.SetupOOMEvent(memoryCg, fd);
    if (error)
        return error;

    OomEventFd = fd;
    Source = std::make_shared<TEpollSource>(Holder->EpollLoop, fd,
                                            EPOLL_EVENT_OOM, shared_from_this());

    error = Holder->EpollLoop->AddSource(Source);
    if (error)
        ShutdownOom();

    return error;
}

TError TContainer::ConfigureDevices(std::vector<TDevice> &devices) {
    auto config = Prop->Get<TStrList>(P_DEVICES);
    auto cg = GetCgroup(DevicesSubsystem);
    TDevice device;
    TError error;

    if (IsRoot() || IsPortoRoot())
        return TError::Success();

    if (Parent->IsPortoRoot() &&
            (!config.empty() || !OwnerCred.IsRootUser())) {
        error = DevicesSubsystem.ApplyDefault(cg);
        if (error)
            return error;
    }

    for (auto &cfg: config) {
        error = device.Parse(cfg);
        if (error)
            return TError(error, "device: " + cfg);

        error = device.Permitted(OwnerCred);
        if (error)
            return TError(error, "device: " + cfg);

        error = DevicesSubsystem.ApplyDevice(cg, device);
        if (error)
            return TError(error, "device: " + cfg);

        devices.push_back(device);
    }

    return TError::Success();
}

TError TContainer::PrepareCgroups() {
    TError error;

    for (auto hy: Hierarchies) {
        TCgroup cg = GetCgroup(*hy);

        if (cg.Exists()) //FIXME kludge for root and restore
            continue;

        error = cg.Create();
        if (error)
            return error;
    }

    if (IsPortoRoot()) {
        error = GetCgroup(MemorySubsystem).SetBool(MemorySubsystem.USE_HIERARCHY, true);
        if (error)
            return error;
    }

    if (!IsRoot()) {
        error = ApplyDynamicProperties();
        if (error)
            return error;
    }

    if (!IsRoot() && !IsPortoRoot()) {
        error = PrepareOomMonitor();
        if (error) {
            L_ERR() << "Can't prepare OOM monitoring: " << error << std::endl;
            return error;
        }
    }

    return TError::Success();
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

TError TContainer::ParseNetConfig(struct TNetCfg &NetCfg) {
    TError error;

    NetCfg.Parent = Parent;
    NetCfg.Id = Id;
    NetCfg.Hostname = Prop->Get<std::string>(P_HOSTNAME);
    NetCfg.NetUp = Prop->Get<int>(P_VIRT_MODE) != VIRT_MODE_OS;
    NetCfg.Holder = Holder;
    NetCfg.OwnerCred = OwnerCred;

    error = NetCfg.ParseNet(Prop->Get<std::vector<std::string>>(P_NET));
    if (error)
        return error;

    error = NetCfg.ParseIp(Prop->Get<std::vector<std::string>>(P_IP));
    if (error)
        return error;

    error = NetCfg.ParseGw(Prop->Get<std::vector<std::string>>(P_DEFAULT_GW));
    if (error)
        return error;

    if (Parent)
        NetCfg.ParentNet = Parent->Net;

    if (Net)
        NetCfg.Net = Net;

    return TError::Success();
}

TError TContainer::PrepareNetwork(struct TNetCfg &NetCfg) {
    TError error;

    error = NetCfg.PrepareNetwork();
    if (error)
        return error;

    if (NetCfg.SaveIp) {
        std::vector<std::string> lines;
        error = NetCfg.FormatIp(lines);
        if (error)
            return error;
        error = Prop->Set<std::vector<std::string>>(P_IP, lines);
        if (error)
            return error;
    }

    Net = NetCfg.Net;

    error = UpdateTrafficClasses();
    if (error) {
        L_ACT() << "Refresh network" << std::endl;
        Net->RefreshClasses(true);
        error = UpdateTrafficClasses();
        if (error) {
            L_ERR() << "Network refresh failed" << std::endl;
            return error;
        }
    }

    if (!IsRoot()) {
        auto netcls = GetCgroup(NetclsSubsystem);
        error = netcls.Set("net_cls.classid",
                std::to_string(TcHandle(ROOT_TC_MAJOR, Id)));
        if (error) {
            L_ERR() << "Can't set classid: " << error << std::endl;
            return error;
        }
    }

    return TError::Success();
}

TError TContainer::GetEnvironment(TEnv &env) {
    env.ClearEnv();

    env.SetEnv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
    env.SetEnv("HOME", Prop->Get<std::string>(P_CWD));
    env.SetEnv("USER", UserName(OwnerCred.Uid));

    env.SetEnv("container", "lxc");

    /* lock these two */
    env.SetEnv("PORTO_NAME", GetName(), true, true);
    env.SetEnv("PORTO_HOST", GetHostName(), true, true);

    /* inherit environment from all parent application containers */
    bool overwrite = true;
    for (auto ct = this; ct; ct = ct->Parent.get()) {
        TError error = env.Parse(ct->Prop->Get<TStrList>(P_ENV), overwrite);
        if (error && overwrite)
            return error;
        overwrite = false;

        if (ct->Prop->Get<int>(P_VIRT_MODE) == VIRT_MODE_OS)
            break;
    }

    return TError::Success();
}

TError TContainer::PrepareTask(std::shared_ptr<TClient> client,
                               struct TNetCfg *NetCfg) {
    auto vmode = Prop->Get<int>(P_VIRT_MODE);
    auto user = UserName(OwnerCred.Uid);
    auto taskEnv = std::unique_ptr<TTaskEnv>(new TTaskEnv());
    auto parent = FindRunningParent();
    TError error;

    taskEnv->Container = GetName();

    for (auto hy: Hierarchies)
        taskEnv->Cgroups.push_back(GetCgroup(*hy));

    taskEnv->Command = Prop->Get<std::string>(P_COMMAND);
    taskEnv->Cwd = Prop->Get<std::string>(P_CWD);
    taskEnv->ParentCwd = Parent->Prop->Get<std::string>(P_CWD);

    taskEnv->LoopDev = Prop->Get<int>(P_RAW_LOOP_DEV);
    if (taskEnv->LoopDev >= 0)
        taskEnv->Root = GetTmpDir();
    else
        taskEnv->Root = Prop->Get<std::string>(P_ROOT);

    taskEnv->RootRdOnly = Prop->Get<bool>(P_ROOT_RDONLY);

    if (vmode == VIRT_MODE_OS) {
        user = "root";
        taskEnv->Cred = TCred(0, 0);
    } else {
        taskEnv->Cred = OwnerCred;
        error = taskEnv->Cred.LoadGroups(user);
        if (error)
            return error;
    }

    error = GetEnvironment(taskEnv->Env);
    if (error)
        return error;

    taskEnv->Isolate = Prop->Get<bool>(P_ISOLATE);
    taskEnv->TripleFork = false;
    taskEnv->QuadroFork = (vmode == VIRT_MODE_APP) &&
                          taskEnv->Isolate &&
                          !taskEnv->Command.empty();

    taskEnv->Hostname = Prop->Get<std::string>(P_HOSTNAME);
    taskEnv->SetEtcHostname = (vmode == VIRT_MODE_OS) &&
                                !taskEnv->Root.IsRoot() &&
                                !taskEnv->RootRdOnly;

    taskEnv->BindDns = Prop->Get<bool>(P_BIND_DNS);

    error = Prop->PrepareTaskEnv(P_RESOLV_CONF, *taskEnv);
    if (error)
        return error;

    taskEnv->Stdin = Stdin;
    taskEnv->Stdout = Stdout;
    taskEnv->Stderr = Stderr;

    error = Prop->PrepareTaskEnv(P_ULIMIT, *taskEnv);
    if (error)
        return error;

    error = Prop->PrepareTaskEnv(P_BIND, *taskEnv);
    if (error)
        return error;

    error = Prop->PrepareTaskEnv(P_CAPABILITIES, *taskEnv);
    if (error)
        return error;

    if (!taskEnv->Root.IsRoot() && Prop->Get<bool>(P_ENABLE_PORTO)) {
        TBindMap bm = { PORTO_SOCKET_PATH, PORTO_SOCKET_PATH, false };

        taskEnv->BindMap.push_back(bm);
    }

    if (client) {
        error = ConfigureDevices(taskEnv->Devices);
        if (error) {
            L_ERR() << "Cannot configure devices: " << error << std::endl;
            return error;
        }
    }

    if (parent && client) {
        pid_t parent_pid = parent->Task->GetPid();

        error = taskEnv->ParentNs.Open(parent_pid);
        if (error)
            return error;

        /* one more fork for creating nested pid-namespace */
        if (taskEnv->Isolate && !InPidNamespace(parent_pid, getpid()))
            taskEnv->TripleFork = true;
    }

    if (NetCfg && NetCfg->NetNs.IsOpened())
        taskEnv->ParentNs.Net.EatFd(NetCfg->NetNs);

    if (NetCfg)
        taskEnv->Autoconf = NetCfg->Autoconf;

    if (taskEnv->Command.empty() || taskEnv->TripleFork || taskEnv->QuadroFork) {
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
    taskEnv->NewMountNs = taskEnv->Isolate ||
                          taskEnv->BindMap.size() || taskEnv->BindDns ||
                          !taskEnv->Root.IsRoot() || taskEnv->RootRdOnly;

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

    PropMask |= USER_SET | GROUP_SET;

    error = FindGroups(UserName(OwnerCred.Uid), OwnerCred.Gid, OwnerCred.Groups);
    if (error) {
        L_ERR() << "Can't set container owner: " << error << std::endl;
        return error;
    }

    SetState(EContainerState::Stopped);

    return Save(); /* Serialize on creation  */
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
    if (vmode == VIRT_MODE_OS && !OwnerCred.IsRootUser()) {
        if (!Prop->Get<bool>(P_ISOLATE) && OwnerCred.Uid != Parent->OwnerCred.Uid)
            return TError(EError::Permission, "virt_mode=os without isolation only for root or owner");
        if (RootPath().IsRoot())
            return TError(EError::Permission, "virt_mode=os without chroot only for root");
    }

    if (!meta && !Prop->Get<std::string>(P_COMMAND).length())
        return TError(EError::InvalidValue, "container command is empty");

    // FIXME must die
    // since we now have a complete picture of properties, check
    // them once again (so we don't miss something due to set order)
    for (auto name : Prop->List()) {
        auto prop = Prop->Find(name);
        if (prop->HasValue()) {
            std::string value;

            error = prop->GetString(value);
            if (!error)
                error = prop->SetString(value);
            if (error)
                return error;
        }
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

    error = PrepareResources(client);
    if (error)
        return error;

    struct TNetCfg NetCfg;

    error = ParseNetConfig(NetCfg);
    if (error)
        return error;

    error = PrepareNetwork(NetCfg);
    if (error)
        goto error;

    if (!meta || (meta && Prop->Get<bool>(P_ISOLATE))) {

        error = PrepareTask(client, &NetCfg);
        if (error)
            goto error;

        error = Task->Start();

        /* Always report OOM stuation if any */
        if (error && HasOomReceived()) {
            if (error)
                L() << "Start error: " << error << std::endl;
            error = TError(EError::InvalidValue, ENOMEM,
                           "OOM, memory limit too low");
        }

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
    auto cg = GetCgroup(FreezerSubsystem);
    TError error = FreezerSubsystem.Freeze(cg);
    if (error) {
        L_ERR() << "Can't freeze container: " << error << std::endl;
        return error;
    }

    {
        TScopedUnlock unlock(holder_lock);
        error = FreezerSubsystem.WaitForFreeze(cg);
        if (error) {
            L_ERR() << "Can't wait for freeze container: " << error << std::endl;
            return error;
        }
    }

    return error;
}

TError TContainer::Unfreeze(TScopedLock &holder_lock) {
    auto cg = GetCgroup(FreezerSubsystem);
    TError error = FreezerSubsystem.Unfreeze(cg);
    if (error) {
        L_ERR() << "Can't unfreeze container: " << error << std::endl;
        return error;
    }

    {
        TScopedUnlock unlock(holder_lock);
        error = FreezerSubsystem.WaitForUnfreeze(cg);
        if (error) {
            L_ERR() << "Can't wait for unfreeze container: " << error << std::endl;
            return error;
        }
    }

    return error;
}

bool TContainer::IsFrozen() {
    auto cg = GetCgroup(FreezerSubsystem);
    return FreezerSubsystem.IsFrozen(cg);
}

bool TContainer::IsValid() {
    return GetState() != EContainerState::Unknown;
}

TError TContainer::SendSignal(int signal) {
    if (IsRoot())
        return TError(EError::Permission, "Cannot kill root container");
    L_ACT() << "Send signal " << signal << " to " << GetName() << std::endl;
    auto cg = GetCgroup(FreezerSubsystem);
    return cg.KillAll(signal);
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

    return SendSignal(SIGKILL);
}

TError TContainer::KillAll(TScopedLock &holder_lock) {
    auto cg = GetCgroup(FreezerSubsystem);

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
        int ret;
        if (!SleepWhile([&]{ return cg.IsEmpty() == false; }, ret,
                        config().container().kill_timeout_ms()) || ret)
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
    if (!loop_image.IsRegularFollow())
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

TError TContainer::PrepareWorkDir() {
    if (IsRoot() || IsPortoRoot())
        return TError::Success();

    TPath work = WorkPath();
    if (work.Exists())
        return TError::Success(); /* FIXME kludge for restore */

    TError error = work.Mkdir(0755);
    if (!error)
        error = work.Chown(OwnerCred);
    return error;
}

TError TContainer::PrepareResources(std::shared_ptr<TClient> client) {
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

    CreateStdStreams();
    error = PrepareStdStreams(client);
    if (error) {
        L_ERR() << "Can't prepare std streams: " << error << std::endl;
        FreeResources();
        return error;
    }

    return TError::Success();
}

void TContainer::FreeResources() {
    TError error;

    if (!IsRoot()) {
        for (auto hy: Hierarchies) {
            auto cg = GetCgroup(*hy);

            error = cg.Remove();
            (void)error; //Logged inside
        }
    }

    if (Net) {
        struct TNetCfg NetCfg;

        error = ParseNetConfig(NetCfg);
        if (!error)
            error = NetCfg.DestroyNetwork();
        if (NetCfg.SaveIp) {
            std::vector<std::string> lines;
            if (!NetCfg.FormatIp(lines))
                Prop->Set<std::vector<std::string>>(P_IP, lines);
        }
        if (error)
            L_ERR() << "Cannot free network resources: " << error << std::endl;

        auto lock = Net->ScopedLock();

        error = Net->RemoveTrafficClasses(Id);
        if (error)
            L_ERR() << "Can't remove traffic class: " << error << std::endl;
    }

    Task = nullptr;
    if (Net && IsRoot()) {
        error = Net->Destroy();
        if (error)
            L_ERR() << "Cannot destroy network: " << error << std::endl;
    }
    Net = nullptr;
    ShutdownOom();

    if (IsRoot() || IsPortoRoot())
        return;

    Stdin.Cleanup();
    Stdout.Cleanup();
    Stderr.Cleanup();

    int loopNr = Prop->Get<int>(P_RAW_LOOP_DEV);

    error = Prop->Set<int>(P_RAW_LOOP_DEV, -1);
    if (error)
        L_ERR() << "Can't set " << P_RAW_LOOP_DEV << ": " << error << std::endl;

    if (loopNr >= 0) {
        error = PutLoopDev(loopNr);
        if (error)
            L_ERR() << "Can't put loop device " << loopNr << ": " << error << std::endl;
    }

    TPath temp_path = GetTmpDir();
    if (temp_path.Exists()) {
        error = temp_path.RemoveAll();
        if (error)
            L_ERR() << "Can't remove " << temp_path << ": " << error << std::endl;
    }

    TPath work_path = WorkPath();
    if (work_path.Exists()) {
        error = work_path.RemoveAll();
        if (error)
            L_ERR() << "Cannot remove working dir: " << error << std::endl;
    }
}

void TContainer::AcquireForced() {
    if (Verbose)
        L() << "Acquire " << GetName() << " (forced)" << std::endl;
    Acquired++;
}

bool TContainer::Acquire() {
    if (!IsAcquired()) {
        if (Verbose)
            L() << "Acquire " << GetName() << std::endl;
        Acquired++;
        return true;
    }
    return false;
}

void TContainer::Release() {
    if (Verbose)
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

        auto cg = GetCgroup(FreezerSubsystem);

        TScopedUnlock unlock(holder_lock);
        int ret;
        if (!SleepWhile([&] () -> int {
                    if (!cg.Exists() || cg.IsEmpty())
                        return 0;
                    kill(Task->GetPid(), 0);
                    return errno != ESRCH;
                }, ret, config().container().stop_timeout_ms()) || ret) {
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

TError TContainer::GetProperty(const string &origProperty, string &value,
                               std::shared_ptr<TClient> &client) const {
    std::string property = origProperty;
    auto dot = property.find('.');
    TError error;

    if (dot != string::npos) {
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

    std::string idx;
    ParsePropertyName(property, idx);

    auto prop = Prop->Find(property);
    if (!prop) {
        prop = Data->Find(property);
        if (!prop) {
            auto new_prop = ContainerPropMap.find(property);
            if (new_prop == ContainerPropMap.end())
                return TError(EError::InvalidProperty,
                              "Unknown container property: " + property);

            if (!(*new_prop).second->IsSupported)
                return TError(EError::NotSupported, "Not supported: " + property);

            CurrentContainer = const_cast<TContainer *>(this);
            CurrentClient = client.get();
            error = (*new_prop).second->Get(value);
            CurrentContainer = nullptr;
            CurrentClient = nullptr;
            return error;
        }
    }

    if (prop->HasFlag(UNSUPPORTED_FEATURE))
        return TError(EError::NotSupported, "Not supported: " + property);

    if (State == EContainerState::Stopped && prop->HasFlag(RUNTIME_VALUE))
        return TError(EError::InvalidState,
                      "Not available in stopped state: " + property);

    if (State != EContainerState::Dead && prop->HasFlag(POSTMORTEM_VALUE))
        return TError(EError::InvalidState,
                      "Available only in dead state: " + property);

    if (property == D_ROOT_PID && Task && client) {
        value = std::to_string(Task->GetPidFor(client->GetPid()));
        return TError::Success();
    }

    if (!prop->HasValue() && prop->HasFlag(PARENT_DEF_PROPERTY) &&
            !Prop->Get<bool>(P_ISOLATE)) {
        for (auto p = Parent; p; p = p->Parent) {
            prop = p->Prop->Find(property);
            if (prop->HasValue() || p->Prop->Get<bool>(P_ISOLATE))
                break;
        }
    }

    if (idx.length())
        return prop->GetIndexed(idx, value);

    error = prop->GetString(value);
    if (error)
        return error;

    return TError::Success();
}

TError TContainer::SetProperty(const string &origProperty,
                               const string &origValue,
                               std::shared_ptr<TClient> &client) {
    if (IsRoot() || IsPortoRoot())
        return TError(EError::Permission, "System containers are read only");

    string property = origProperty;
    std::string idx;
    ParsePropertyName(property, idx);
    string value = StringTrim(origValue);
    TError error;

    auto prop = Prop->Find(property);
    if (!prop) {
        auto new_prop = ContainerPropMap.find(property);
        if (new_prop == ContainerPropMap.end())
            return TError(EError::Unknown, "Invalid property " + property);

        if (!(*new_prop).second->IsSupported)
            return TError(EError::NotSupported, property + " is not supported");

        CurrentContainer = const_cast<TContainer *>(this);
        CurrentClient = client.get();
        error = (*new_prop).second->Set(value);
        CurrentContainer = nullptr;
        CurrentClient = nullptr;

        if (!error)
            return Save();
        return error;
    }

    if (prop->HasFlag(UNSUPPORTED_FEATURE))
        return TError(EError::NotSupported, property + " is not supported");

    if (State == EContainerState::Dead)
        return TError(EError::InvalidState, "Cannot change in dead state");

    if (State != EContainerState::Stopped && !prop->HasFlag(DYNAMIC_VALUE))
        return TError(EError::InvalidState, "Cannot change in runtime");

    bool superuser = client && client->GetCred().IsRootUser();

    if (prop->HasFlag(SUPERUSER_PROPERTY) && !superuser) {
        std::string current;
        error = prop->GetString(current);
        if (error)
            return error;
        if (value != current)
            return TError(EError::Permission, "Only root can change this property");
    }

    if (idx.length())
        error = prop->SetIndexed(idx, value);
    else
        error = prop->SetString(value);

    if (error)
        return error;

    if (State != EContainerState::Stopped && property != P_PRIVATE) {
        error = ApplyDynamicProperties();
        if (error)
            return error;

        if (property == P_NET_LIMIT || property == P_NET_GUARANTEE) {
            error = UpdateTrafficClasses();
            if (error) {
                L_ERR() << "Cannot update tc : " << error << std::endl;
                return error;
            }
        }
    }

    // Write KVS snapshot, otherwise it may grow indefinitely and on next
    // restart we will merge it forever

    return Save();
}

TError TContainer::Prepare() {
    std::shared_ptr<TKeyValueNode> kvnode;
    if (!IsRoot() && !IsPortoRoot())
        kvnode = Storage->GetNode(Id);

    Prop = std::make_shared<TPropertyMap>(kvnode, shared_from_this());
    PORTO_ASSERT(Prop != nullptr);
    Data = std::make_shared<TValueMap>(kvnode);
    PORTO_ASSERT(Data != nullptr);

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

        error = Net->RefreshDevices();
        if (error)
            return error;
        Net->NewManagedDevices = false;
    }

    error = UpdateTrafficClasses();
    if (error)
        return error;

    return TError::Success();
}

void TContainer::RestoreStdPath(const std::string &property) {
    TPath path = ActualStdPath(property, true);
    bool def = Prop->IsDefault(property);

    if (def && !path.Exists()) {
        TPath root(Prop->Get<std::string>(P_ROOT));
        std::string cwd(Prop->Get<std::string>(P_CWD));
        std::string name(Prop->Get<std::string>(property) + "." + GetTextId("_"));

        if (root.IsRegularFollow())
            path = GetTmpDir() / name;
        else
            path = root / cwd / name;

        /* Restore from < 2.7 */
        if (path.IsRegularStrict()) {
            L_ACT() << GetName() << ": restore " << property << " "
                    << path.ToString() << std::endl;
            Prop->Set<std::string>(property, path.ToString());
        }
    }

    if (def && GetState() == EContainerState::Stopped && path.IsRegularStrict())
        path.Unlink();
}

TError TContainer::Save(void) {

    /* Ensure that we're saving context with lock acquired... */

    auto kvnode = Storage->GetNode(Id);
    kv::TNode new_node;
    TError error;

    /* By creating we truncate existing node */
    kvnode->Create();

    for (auto knob_name : Prop->List()) {

        auto knob = Prop->Find(knob_name);

        if (!knob->HasFlag(PERSISTENT_VALUE) || !knob->HasValue())
            continue;

        std::string value;
        error = knob->GetString(value);
        if (error)
            return error;

        auto pair = new_node.add_pairs();
        pair->set_key(knob_name);
        pair->set_val(value);

    }

    for (auto data_knob_name : Data->List()) {

        auto data_knob = Data->Find(data_knob_name);

        if (!data_knob->HasFlag(PERSISTENT_VALUE) || !data_knob->HasValue())
            continue;

        std::string value;
        error = data_knob->GetString(value);
        if (error)
            return error;

        auto pair = new_node.add_pairs();
        pair->set_key(data_knob_name);
        pair->set_val(value);

    }

    TClient fakeroot(TCred(0,0));
    CurrentContainer = const_cast<TContainer *>(this);
    CurrentClient = &fakeroot;

    for (auto knob : ContainerPropMap) {
        std::string value;

        if (!(PropMask & knob.second->SetMask))
            continue; /* Skip knobs without a value */

        error = knob.second->Get(value);
        if (error)
            return error;

        auto pair = new_node.add_pairs();
        pair->set_key(knob.first);
        pair->set_val(value);
    }

    CurrentContainer = nullptr;
    CurrentClient = nullptr;

    return kvnode->Append(new_node);
}

TError TContainer::Restore(TScopedLock &holder_lock, const kv::TNode &node) {
    L_ACT() << "Restore " << GetName() << " with id " << Id << std::endl;

    TError error = Prepare();
    if (error)
        return error;

    TClient fakeroot(TCred(0,0));
    CurrentContainer = const_cast<TContainer *>(this);
    CurrentClient = &fakeroot;

    for (int i = 0; i < node.pairs_size(); i++) {

        std::string key = node.pairs(i).key();
        std::string value = node.pairs(i).val();
        error = TError::Success();

        auto pv = Prop->Find(key);

        if (pv && pv->HasFlag(PERSISTENT_VALUE)) {

            if (Verbose)
                L_ACT() << "Restoring as property " << key << " = " << value << std::endl;

            error = pv->SetString(value);
            if (!error)
                continue;
        }

        auto dv = Data->Find(key);

        if (dv && dv->HasFlag(PERSISTENT_VALUE)) {

            if (Verbose)
                L_ACT() << "Restoring as data " << key << " = " << value << std::endl;

            error = dv->SetString(value);
            if (!error)
                continue;
        }

        auto prop = ContainerPropMap.find(key);

        if (prop != ContainerPropMap.end()) {

            if (Verbose)
                L_ACT() << "Restoring as new property" << key << " = " << value << std::endl;

            error = (*prop).second->Set(value);
            if (!error)
                continue;

            PropMask |= (*prop).second->SetMask; /* Indicate that we've set the value */
        }

        if (error)
            L_ERR() << error << ": Can't restore " << key << ", skipped" << std::endl;

    }

    CurrentContainer = nullptr;
    CurrentClient = nullptr;

    error = Save(); /* FIXME: maybe we need do it at the end of func? */
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

    auto state = Data->Get<std::string>(D_STATE);

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

        TError error = PrepareResources(nullptr);
        if (error)
            return error;

        error = PrepareTask(nullptr, nullptr);
        if (error) {
            FreeResources();
            return error;
        }

        Task->Restore(pids);

        pid_t pid = Task->GetPid();
        pid_t ppid;

        TCgroup taskCg, freezerCg = GetCgroup(FreezerSubsystem);

        error = GetTaskParent(Task->GetWPid(), ppid);
        if (!error)
            error = FreezerSubsystem.TaskCgroup(pid, taskCg);

        if (error) {
            L() << "Cannot get ppid or cgroup of restored task " << pid
                << ": " << error << std::endl;
            LostAndRestored = true;
        } else if (ppid != getppid()) {
            L() << "Invalid ppid of restored task " << pid << ": "
                << ppid << " != " << getppid() << std::endl;
            LostAndRestored = true;
        } else if (Task->IsZombie()) {
            L() << "Task " << pid << " is zombie and belongs to porto" << std::endl;
        } else if (taskCg != freezerCg) {
            L_WRN() << "Task " << pid << " belongs to wrong freezer cgroup: "
                    << taskCg << " should be " << freezerCg << std::endl;
            LostAndRestored = true;
        } else {
            L() << "Task " << pid << " is running and belongs to porto" << std::endl;

            std::vector<pid_t> tasks;
            error = freezerCg.GetTasks(tasks);
            if (error) {
                L_WRN() << "Cannot dump cgroups " << freezerCg << " " << error << std::endl;
                LostAndRestored = true;
            } else {
                /* Sweep all tasks from freezer cgroup into correct cgroups */
                for (pid_t pid: tasks) {
                    for (auto hy: Hierarchies) {
                        TCgroup currentCg, correctCg = GetCgroup(*hy);
                        error = hy->TaskCgroup(pid, currentCg);
                        if (error || currentCg == correctCg)
                            continue;

                        /* Recheck freezer cgroup */
                        TCgroup currentFr;
                        error = FreezerSubsystem.TaskCgroup(pid, currentFr);
                        if (error || currentFr != freezerCg)
                            continue;

                        L_WRN() << "Process " << pid << " in " << currentCg
                                << " while should be in " << correctCg << std::endl;
                        (void)correctCg.Attach(pid);
                    }
                }
            }

            error = RestoreNetwork();
            if (error) {
                if (Task->HasCorrectParent() && !Task->IsZombie())
                    L_WRN() << "Cannot restore network: " << error << std::endl;
                LostAndRestored = true;
            }
        }

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

            auto cg = GetCgroup(FreezerSubsystem);
            if (FreezerSubsystem.IsFrozen(cg))
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

        auto freezerCg = GetCgroup(FreezerSubsystem);
        if (freezerCg.Exists() && !freezerCg.IsEmpty())
            (void)freezerCg.KillAll(9);

        if (state == ContainerStateName(EContainerState::Meta) &&
                Prop->Get<std::string>(P_COMMAND).empty())
            SetState(EContainerState::Meta);
        else
            SetState(EContainerState::Stopped);
        Task = nullptr;
    }

    RestoreStdPath(P_STDOUT_PATH);
    RestoreStdPath(P_STDERR_PATH);
    CreateStdStreams();

    if (Task)
        Task->ClearEnv();

    if (Parent)
        Parent->AddChild(shared_from_this());

    return TError::Success();
}

TCgroup TContainer::GetCgroup(const TSubsystem &subsystem) const {
    if (IsRoot())
        return subsystem.RootCgroup();
    if (IsPortoRoot())
        return subsystem.Cgroup(PORTO_ROOT_CGROUP);
    return subsystem.Cgroup(std::string(PORTO_ROOT_CGROUP) + "/" + GetName());
}

void TContainer::ExitTree(TScopedLock &holder_lock, int status, bool oomKilled) {

    /* Detect fatal signals: portoinit cannot kill itself */
    if (WIFEXITED(status) && WEXITSTATUS(status) > 128 &&
            WEXITSTATUS(status) < 128 + SIGRTMIN &&
            Prop->Get<int>(P_VIRT_MODE) == VIRT_MODE_APP &&
            Prop->Get<bool>(P_ISOLATE) == true)
        status = WEXITSTATUS(status) - 128;

    L_EVT() << "Exit tree " << GetName() << " with status "
            << status << (oomKilled ? " invoked by OOM" : "")
            << std::endl;

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
        return TError::Success();
    });

    Exit(holder_lock, status, oomKilled);
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

    RotateStdFile(Stdout, D_STDOUT_OFFSET);
    RotateStdFile(Stderr, D_STDERR_OFFSET);

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
    if (OomEventFd.GetFd() != fd)
        return false;

    if (!Task)
        return false;

    if (GetState() == EContainerState::Dead)
        return false;

    return true;
}

// Works only once
bool TContainer::HasOomReceived() {
    uint64_t val;

    return read(OomEventFd.GetFd(), &val, sizeof(val)) == sizeof(val) && val != 0;
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
    return error;
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

void TContainer::DeliverEvent(TScopedLock &holder_lock, const TEvent &event) {
    TError error;
    switch (event.Type) {
        case EEventType::Exit:
            ExitTree(holder_lock, event.Exit.Status, FdHasEvent(OomEventFd.GetFd()));
            break;
        case EEventType::RotateLogs:
            if (GetState() == EContainerState::Running && Task) {
                RotateStdFile(Stdout, D_STDOUT_OFFSET);
                RotateStdFile(Stderr, D_STDERR_OFFSET);
            }
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

    if (ucred.CanControl(OwnerCred))
        return TError::Success();

    return TError(EError::Permission, "Permission error");
}

std::string TContainer::GetPortoNamespace() const {
    if (Parent)
        return Parent->GetPortoNamespace() + Prop->Get<std::string>(P_PORTO_NAMESPACE);
    else
        return "";
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
    if (!IsRoot() && !IsPortoRoot())
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

TError TContainer::UpdateTrafficClasses() {
    if (Net) {
        TUintMap prio, rate, ceil;
        prio = Prop->Get<TUintMap>(P_NET_PRIO);
        rate = Prop->Get<TUintMap>(P_NET_GUARANTEE);
        ceil = Prop->Get<TUintMap>(P_NET_LIMIT);

        int parentId;

        if (IsRoot())
            parentId = 0;
        else if (Net == Parent->Net)
            parentId = Parent->Id;
        else
            parentId = PORTO_ROOT_CONTAINER_ID;

        auto net_lock = Net->ScopedLock();
        return Net->UpdateTrafficClasses(parentId, Id, prio, rate, ceil);
    }
    return TError::Success();
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
            err = client->ComposeRelativeName(*who, name);
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
        if (!fnmatch(wildcard.c_str(), name.c_str(), FNM_PATHNAME))
            return true;
    return false;
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

        if (child.GetState() != EContainerState::Stopped)
            return child.Stop(holder_lock);
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
