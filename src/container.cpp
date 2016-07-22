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
#include "event.hpp"
#include "holder.hpp"
#include "network.hpp"
#include "context.hpp"
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

std::map<std::string, TProperty*> ContainerPropMap;

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

    if (IsRoot() || IsPortoRoot())
        Cwd = "/";
    else
        Cwd = WorkPath().ToString();

    StdinPath = "/dev/null";
    StdoutPath = "stdout";
    StderrPath = "stderr";
    Root = "/";
    RootRo = false;
    Isolate = true;
    BindDns = false; /* Because root is default */
    VirtMode = VIRT_MODE_APP;
    NetProp = { "inherited" };
    Hostname = "";
    Caps = 0;
    LoopDev = -1;

    if (IsRoot())
        NsName = std::string(PORTO_ROOT_CONTAINER) + "/";
    else
        NsName = "";

    StdoutLimit = config().container().stdout_limit();
    MemLimit = 0;
    AnonMemLimit = 0;
    DirtyMemLimit = 0;
    RechargeOnPgfault = false;
    CpuPolicy = "normal";
    CpuLimit = GetNumCores();
    CpuGuarantee = 0;
    IoPolicy = "normal";
    IoLimit = 0;
    IopsLimit = 0;

    if (IsRoot())
        NetGuarantee["default"] = NET_MAX_GUARANTEE;
    else
        NetGuarantee["default"] = config().network().default_guarantee();

    NetLimit["default"] = 0;
    NetPriority["default"] = NET_DEFAULT_PRIO;
    ToRespawn = false;
    MaxRespawns = -1;
    RespawnCount = 0;
    Private = "";
    AgingTime = config().container().default_aging_time_s();
    PortoEnabled = true;
    for (auto c = parent; c; c = c->GetParent()) {
        PortoEnabled &= c->PortoEnabled;
    }
    IsWeak = false;
    ExitStatus = 0;
    TaskStartErrno = -1;
    StdoutOffset = 0;
    StderrOffset = 0;
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

    TPath path(Root);
    if (!path.IsRoot()) {
        if (path.IsRegularFollow())
            path = GetTmpDir();
        path = path.NormalPath();
    }

    return Parent->RootPath() / path;
}

TPath TContainer::ActualStdPath(const std::string &path_str,
                                bool is_default, bool host) const {
    TPath path(path_str);

    if (is_default) {
        /* /dev/null is /dev/null */
        if (path == "/dev/null")
            return path;

        /* By default, std files are located on host in a special folder */
        return WorkPath() / path;
    } else {
	/* Custom std paths are given relative to container root and/or cwd. */
        TPath cwd(Cwd);
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

TError TContainer::RotateStdFile(TStdStream &stream, uint64_t &offset_value) {
    off_t loss;
    TError error = stream.Rotate(config().container().max_log_size(), loss);

    if (!error && loss) {
            offset_value += loss;
    }

    return error;
}

void TContainer::CreateStdStreams() {
    Stdin = TStdStream(STDIN_FILENO,
                       ActualStdPath(StdinPath, !(PropMask & STDIN_SET), false),
                       ActualStdPath(StdinPath, !(PropMask & STDIN_SET) > 0, true),
                       !(PropMask & STDIN_SET));
    Stdout = TStdStream(STDOUT_FILENO,
                        ActualStdPath(StdoutPath, !(PropMask & STDOUT_SET), false),
                        ActualStdPath(StdoutPath, !(PropMask & STDOUT_SET), true),
                        !(PropMask & STDOUT_SET));
    Stderr = TStdStream(STDERR_FILENO,
                        ActualStdPath(StderrPath, !(PropMask & STDERR_SET), false),
                        ActualStdPath(StderrPath, !(PropMask & STDERR_SET), true),
                        !(PropMask & STDERR_SET));
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
        if (!volume->IsReady) {
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
    if (IsWeak) {
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

uint64_t TContainer::GetHierarchyMemLimit(std::shared_ptr<const TContainer> root) const {
    uint64_t val = MemLimit;
    std::shared_ptr<const TContainer> p = shared_from_this();

    if (State == EContainerState::Meta) {
        auto children_limit = 0lu;

        for (auto iter : p->Children) {
            if (auto child = iter.lock()) {
                auto child_limit = child->GetHierarchyMemLimit(p);

                if (child_limit) {
                    children_limit += child_limit;
                } else {
                    children_limit = 0;
                    break;
                }
            }
        }

        if (children_limit)
            val = val > 0 ?
                  (val > children_limit ? children_limit : val)
                  : children_limit;
    }

    while (p != root) {

        if (p->MemLimit)
            val = val > 0 ?
                  (val > p->MemLimit ? p->MemLimit : val)
                  : p->MemLimit;

        p = p->GetParent();
    }

    return val;
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

    error = MemorySubsystem.SetLimit(memcg, MemLimit);
    if (error) {
        if (error.GetErrno() == EBUSY)
            return TError(EError::InvalidValue, std::to_string(MemLimit) + " is too low");

        L_ERR() << "Can't set " << P_MEM_LIMIT << ": " << error << std::endl;
        return error;
    }

    error = MemorySubsystem.SetAnonLimit(memcg, AnonMemLimit);
    if (error) {
        L_ERR() << "Can't set " << P_ANON_LIMIT << ": " << error << std::endl;
        return error;
    }

    error = MemorySubsystem.RechargeOnPgfault(memcg, RechargeOnPgfault);
    if (error) {
        L_ERR() << "Can't set " << P_RECHARGE_ON_PGFAULT << ": " << error << std::endl;
        return error;
    }

    auto cpucg = GetCgroup(CpuSubsystem);
    error = CpuSubsystem.SetCpuPolicy(cpucg,
            CpuPolicy,
            CpuGuarantee,
            CpuLimit);
    if (error) {
        L_ERR() << "Cannot set cpu policy: " << error << std::endl;
        return error;
    }

    auto blkcg = GetCgroup(BlkioSubsystem);
    error = BlkioSubsystem.SetPolicy(blkcg, IoPolicy == "batch");
    if (error) {
        L_ERR() << "Can't set " << P_IO_POLICY << ": " << error << std::endl;
        return error;
    }

    error = MemorySubsystem.SetIoLimit(memcg, IoLimit);
    if (error) {
        L_ERR() << "Can't set " << P_IO_LIMIT << ": " << error << std::endl;
        return error;
    }

    error = MemorySubsystem.SetIopsLimit(memcg, IopsLimit);
    if (error) {
        L_ERR() << "Can't set " << P_IO_OPS_LIMIT << ": " << error << std::endl;
        return error;
    }

    error = MemorySubsystem.SetDirtyLimit(memcg, DirtyMemLimit);
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
    auto config = Devices;
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
    NetCfg.Hostname = Hostname;
    NetCfg.NetUp = VirtMode != VIRT_MODE_OS;
    NetCfg.Holder = Holder;
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
        IpList = lines;
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
    env.SetEnv("HOME", Cwd);
    env.SetEnv("USER", UserName(OwnerCred.Uid));

    env.SetEnv("container", "lxc");

    /* lock these two */
    env.SetEnv("PORTO_NAME", GetName(), true, true);
    env.SetEnv("PORTO_HOST", GetHostName(), true, true);

    /* inherit environment from all parent application containers */
    bool overwrite = true;
    for (auto ct = this; ct; ct = ct->Parent.get()) {
        TError error = env.Parse(EnvCfg, overwrite);
        if (error && overwrite)
            return error;
        overwrite = false;

        if (ct->VirtMode == VIRT_MODE_OS)
            break;
    }

    return TError::Success();
}

TError TContainer::PrepareTask(std::shared_ptr<TClient> client,
                               struct TNetCfg *NetCfg) {
    auto user = UserName(OwnerCred.Uid);
    auto taskEnv = std::unique_ptr<TTaskEnv>(new TTaskEnv());
    auto parent = FindRunningParent();
    TError error;

    taskEnv->Container = GetName();

    for (auto hy: Hierarchies)
        taskEnv->Cgroups.push_back(GetCgroup(*hy));

    taskEnv->Command = Command;
    taskEnv->Cwd = Cwd;
    taskEnv->ParentCwd = Parent->Cwd;

    taskEnv->LoopDev = LoopDev;
    if (taskEnv->LoopDev >= 0)
        taskEnv->Root = GetTmpDir();
    else
        taskEnv->Root = Root;

    taskEnv->RootRdOnly = RootRo;

    taskEnv->OwnerCred = OwnerCred;

    if (VirtMode == VIRT_MODE_OS) {
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

    taskEnv->Isolate = Isolate;
    taskEnv->TripleFork = false;
    taskEnv->QuadroFork = (VirtMode == VIRT_MODE_APP) &&
                          taskEnv->Isolate &&
                          !taskEnv->Command.empty();

    taskEnv->Hostname = Hostname;
    taskEnv->SetEtcHostname = (VirtMode == VIRT_MODE_OS) &&
                                !taskEnv->Root.IsRoot() &&
                                !taskEnv->RootRdOnly;

    taskEnv->BindDns = BindDns;

    if (PropMask & RESOLV_CONF_SET) {
        if (taskEnv->Root.IsRoot())
            return TError(EError::InvalidValue,
                    "resolv_conf requires separate root");

        taskEnv->BindDns = false;
        CurrentContainer->BindDns = false;
        for (auto &line: ResolvConf)
            taskEnv->ResolvConf += line + "\n";
    }

    taskEnv->Stdin = Stdin;
    taskEnv->Stdout = Stdout;
    taskEnv->Stderr = Stderr;

    taskEnv->Rlimit = Rlimit;

    taskEnv->BindMap = BindMap;

    taskEnv->Caps = Caps;

    if (!taskEnv->Root.IsRoot() && PortoEnabled) {
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

    CgroupEmptySince = 0;

    OwnerCred = TCred(cred.Uid, cred.Gid);

    PropMask |= USER_SET | GROUP_SET;

    TError error = FindGroups(UserName(OwnerCred.Uid), OwnerCred.Gid, OwnerCred.Groups);
    if (error) {
        L_ERR() << "Can't set container owner: " << error << std::endl;
        return error;
    }

    if (OwnerCred.IsRootUser())
        Caps = 0xffffffffffffffff >> (63 - LastCapability);

    SetState(EContainerState::Stopped);
    PropMask |= STATE_SET;

    RespawnCount = 0;
    PropMask |= RESPAWN_COUNT_SET;

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

    if (VirtMode == VIRT_MODE_OS && !OwnerCred.IsRootUser()) {
        if (!Isolate && OwnerCred.Uid != Parent->OwnerCred.Uid)
            return TError(EError::Permission, "virt_mode=os without isolation only for root or owner");
        if (RootPath().IsRoot())
            return TError(EError::Permission, "virt_mode=os without chroot only for root");
    }

    if (!meta && !Command.length())
        return TError(EError::InvalidValue, "container command is empty");

    L_ACT() << "Start " << GetName() << " " << Id << std::endl;

    ExitStatus = -1;
    PropMask |= EXIT_STATUS_SET;

    OomKilled = false;
    PropMask |= OOM_KILLED_SET;

    StartTime = GetCurrentTimeMs();
    PropMask |= START_TIME_SET;

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

    if (!meta || (meta && Isolate)) {

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
            TaskStartErrno = error.GetErrno();
            goto error;
        }

        TaskStartErrno = -1;

        L() << GetName() << " started " << std::to_string(Task->GetPid()) << std::endl;

        RootPid = Task->GetPids();
        PropMask |= ROOT_PID_SET;
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

    return Save();

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
    TPath loop_image(Root);
    if (!loop_image.IsRegularFollow())
        return TError::Success();

    TError error;
    TPath temp_path = GetTmpDir();
    if (!temp_path.Exists()) {
        error = temp_path.Mkdir(0755);
        if (error)
            return error;
    }

    if (!(PropMask & ROOT_SET) ||
            loop_image == Parent->Root)
        return TError::Success();

    int loop_dev;
    error = SetupLoopDevice(loop_image, loop_dev);
    if (error)
        return error;

    LoopDev = loop_dev;
    PropMask |= LOOP_DEV_SET;

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
                IpList = lines;
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

    int loopNr = LoopDev;
    LoopDev = -1;
    PropMask |= LOOP_DEV_SET;

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

    return Save();
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

        return child.Save();
    });

    return Save();
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

        return child.Save();
    });

    return Save();
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

    auto prop = ContainerPropMap.find(property);
    if (prop == ContainerPropMap.end())
        return TError(EError::InvalidProperty,
                              "Unknown container property: " + property);

    if (!(*prop).second->IsSupported)
        return TError(EError::NotSupported, "Not supported: " + property);

    CurrentContainer = const_cast<TContainer *>(this);
    CurrentClient = client.get();
    if (idx.length())
        error = (*prop).second->GetIndexed(idx, value);
    else
        error = (*prop).second->Get(value);
    CurrentContainer = nullptr;
    CurrentClient = nullptr;

    return error;
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

    auto new_prop = ContainerPropMap.find(property);
    if (new_prop == ContainerPropMap.end())
        return TError(EError::Unknown, "Invalid property " + property);

    if (!(*new_prop).second->IsSupported)
        return TError(EError::NotSupported, property + " is not supported");

    CurrentContainer = const_cast<TContainer *>(this);
    CurrentClient = client.get();
    if (idx.length())
        error = (*new_prop).second->SetIndexed(idx, value);
    else
        error = (*new_prop).second->Set(value);
    CurrentContainer = nullptr;
    CurrentClient = nullptr;

    if (error)
        return error;

    // Write KVS snapshot, otherwise it may grow indefinitely and on next
    // restart we will merge it forever

    return Save();
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

void TContainer::RestoreStdPath(const std::string &property,
                                const std::string &stdpath,
                                bool is_default) {
    TPath path = ActualStdPath(stdpath, is_default, true);

    if (is_default && !path.Exists()) {
        TPath root(Root);
        std::string cwd(Cwd);
        std::string name(stdpath + "." + GetTextId("_"));

        if (root.IsRegularFollow())
            path = GetTmpDir() / name;
        else
            path = root / cwd / name;

        /* Restore from < 2.7 */
        if (path.IsRegularStrict()) {
            L_ACT() << GetName() << ": restore " << property << " "
                    << path.ToString() << std::endl;
            ContainerPropMap[property]->Set(path.ToString());
        }
    }

    if (is_default && GetState() == EContainerState::Stopped && path.IsRegularStrict())
        path.Unlink();
}

TError TContainer::Save(void) {

    /* Ensure that we're saving context with lock acquired... */

    auto kvnode = Storage->GetNode(Id);
    kv::TNode new_node;
    TError error;

    /* By creating we truncate existing node */
    kvnode->Create();

    /*
     * _id and _name are exceptional knobs.
     * We restore Id and Name earlier than other knobs
     * (because of proper container restore order and id sets).
     * So let's save them manually.
     */

    auto pair = new_node.add_pairs();
    pair->set_key(std::string(P_RAW_ID));
    pair->set_val(std::to_string(Id));

    pair = new_node.add_pairs();
    pair->set_key(std::string(P_RAW_NAME));
    pair->set_val(GetName());

    TClient fakeroot(TCred(0,0));
    CurrentContainer = const_cast<TContainer *>(this);
    CurrentClient = &fakeroot;

    for (auto knob : ContainerPropMap) {
        std::string value;

        if (!(knob.second->IsSerializable) ||
            !(PropMask & knob.second->SetMask))
            continue; /* Skip knobs without a value */

        error = knob.second->GetToSave(value);
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

    CgroupEmptySince = 0;

    TClient fakeroot(TCred(0,0));
    CurrentContainer = const_cast<TContainer *>(this);
    CurrentClient = &fakeroot;

    std::string container_state;
    TError error;

    for (int i = 0; i < node.pairs_size(); i++) {

        std::string key = node.pairs(i).key();
        std::string value = node.pairs(i).val();
        error = TError::Success();

        if (key == D_STATE) {
            /*
             * We need to set state at the last moment
             * because properties depends on the current value
             */
            container_state = value;
            continue;
        }

        auto prop = ContainerPropMap.find(key);
        if (prop != ContainerPropMap.end()) {

            if (Verbose)
                L_ACT() << "Restoring as new property" << key << " = " << value << std::endl;

            error = (*prop).second->SetFromRestore(value);
            if (!error) {
                PropMask |= (*prop).second->SetMask; /* Indicate that we've set the value */
                continue;
            }
        }

        if (error)
            L_ERR() << error << ": Can't restore " << key << ", skipped" << std::endl;

    }

    /* Valid container state cannot be empty */
    if (container_state.length() > 0) {
        (*ContainerPropMap.find(D_STATE)).second->SetFromRestore(container_state);
        PropMask |= STATE_SET;
    }

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

    bool started = (PropMask & ROOT_PID_SET) > 0;
    bool created = (PropMask & STATE_SET) > 0;

    if (!created) {
        error = TError(EError::Unknown, "Container has not been created");
        goto error;
    }

    if (started) {
        std::vector<int> pids = RootPid;

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
            bool meta = parent->Command.empty();

            L() << "Start parent " << parent->GetName() << " meta " << meta << std::endl;

            error = parent->Start(nullptr, meta);
            if (error)
                goto error;

            parent = parent->Parent;
        }

        error = PrepareResources(nullptr);
        if (error)
            goto error;

        error = PrepareTask(nullptr, nullptr);
        if (error) {
            FreeResources();
            goto error;
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

        if (State == EContainerState::Dead) {
            // we started recording death time since porto v1.15,
            // use some sensible default
            if (!(PropMask & DEATH_TIME_SET)) {
                DeathTime = GetCurrentTimeMs();
                PropMask |= DEATH_TIME_SET;
            }

            SetState(EContainerState::Dead);
        } else {
            // we started recording start time since porto v1.15,
            // use some sensible default
            if (!(PropMask & START_TIME_SET)) {
                StartTime = GetCurrentTimeMs();
                PropMask |= START_TIME_SET;
            }

            if (Command.empty())
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

        if (State == EContainerState::Meta &&
                Command.empty())
            SetState(EContainerState::Meta);
        else
            SetState(EContainerState::Stopped);
        Task = nullptr;
    }

    RestoreStdPath(P_STDOUT_PATH, StdoutPath, !(PropMask & STDOUT_SET));
    RestoreStdPath(P_STDERR_PATH, StderrPath, !(PropMask & STDERR_SET));
    CreateStdStreams();

    if (Task)
        Task->ClearEnv();

    if (Parent)
        Parent->AddChild(shared_from_this());

    CurrentContainer = nullptr;
    CurrentClient = nullptr;

    return Save();

error:
    CurrentContainer = nullptr;
    CurrentClient = nullptr;

    return error;
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
            VirtMode == VIRT_MODE_APP &&
            Isolate == true)
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
    TError error;

    if (!Task)
        return;

    L_EVT() << "Exit " << GetName() << " (root_pid " << Task->GetPid() << ")"
            << " with status " << status << (oomKilled ? " invoked by OOM" : "")
            << std::endl;

    ShutdownOom();

    ExitStatus = status;
    PropMask |= EXIT_STATUS_SET;

    DeathTime = GetCurrentTimeMs();
    PropMask |= DEATH_TIME_SET;

    if (oomKilled) {
        L_EVT() << Task->GetPid() << " killed by OOM" << std::endl;

        OomKilled = true;
        PropMask |= OOM_KILLED_SET;

        error = KillAll(holder_lock);
        if (error)
            L_WRN() << "Can't kill all tasks in container: " << error << std::endl;
    }

    if (!Isolate) {
        TError error = KillAll(holder_lock);
        if (error)
            L_WRN() << "Can't kill all tasks in non-isolated container: " << error << std::endl;
    }

    Task->Exit(status);
    SetState(EContainerState::Dead);

    RootPid = {0, 0, 0};
    PropMask |= ROOT_PID_SET;

    error = RotateStdFile(Stdout, StdoutOffset);
    if (error)
        L_ERR() << "Can't rotate stdout_offset: " << error << std::endl;

    error = RotateStdFile(Stderr, StderrOffset);
    if (error)
        L_ERR() << "Can't rotate stderr_offset: " << error << std::endl;

    error = Save();
    if (error)
        L_WRN() << "Can't save container state after exit: " << error << std::endl;

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

    if (!ToRespawn)
        return false;

    return MaxRespawns < 0 || RespawnCount < (uint64_t)MaxRespawns;
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


    error = Start(nullptr, false);
    RespawnCount++;
    PropMask |= RESPAWN_COUNT_SET;

    if (error)
        return error;

    return Save();
}

bool TContainer::CanRemoveDead() const {
    return State == EContainerState::Dead &&
        DeathTime / 1000 +
        AgingTime <= GetCurrentTimeMs() / 1000;
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
            {
                uint64_t failcnt = 0lu;
                auto cg = GetCgroup(MemorySubsystem);
                error = MemorySubsystem.GetFailCnt(cg, failcnt);
                if (error)
                    L_WRN() << "Can't get container memory.failcnt" << std::endl;

                ExitTree(holder_lock, event.Exit.Status,
                         FdHasEvent(OomEventFd.GetFd()) || failcnt);
            }
            break;
        case EEventType::RotateLogs:
            if (GetState() == EContainerState::Running && Task) {
                error = RotateStdFile(Stdout, StdoutOffset);
                if (error)
                    L_ERR() << "Can't rotate stdout_offset: " << error
                            << std::endl;
                error = RotateStdFile(Stderr, StderrOffset);
                if (error)
                    L_ERR() << "Can't rotate stderr_offset: " << error
                            << std::endl;
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
        return Parent->GetPortoNamespace() + NsName;
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
        int parentId;

        if (IsRoot())
            parentId = 0;
        else if (Net == Parent->Net)
            parentId = Parent->Id;
        else
            parentId = PORTO_ROOT_CONTAINER_ID;

        auto net_lock = Net->ScopedLock();
        return Net->UpdateTrafficClasses(parentId, Id, NetPriority, NetGuarantee, NetLimit);
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
