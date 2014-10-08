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

static int64_t BootTime = 0;

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

    static string OomKilled(TContainer& c) {
        if (c.OomKilled)
            return "true";
        else
            return "false";
    }

    static string Parent(TContainer& c) {
        return c.Parent->Name;
    }

    static string RespawnCount(TContainer& c) {
        return std::to_string(c.RespawnCount);
    }

    static string RootPid(TContainer& c) {
        if (c.Task)
            return std::to_string(c.Task->GetPid());
        else
            return "-1";
    };

    static string ExitStatus(TContainer& c) {
        if (c.Task && !c.Task->IsRunning())
            return std::to_string(c.Task->GetExitStatus());
        else
            return "-1";
    };

    static string StartErrno(TContainer& c) {
        return std::to_string(c.TaskStartErrno);
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

        return std::to_string(val);
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
            TLogger::LogError(error, "Can't get memory usage");
            return "-1";
        }

        return std::to_string(val);
    };

    static string NetBytes(TContainer& c) {
        uint64_t val;
        TError error = c.Tclass->GetStat(ETclassStat::Bytes, val);
        if (error) {
            TLogger::LogError(error, "Can't get transmitted bytes");
            return "-1";
        }

        return std::to_string(val);
    };

    static string NetPackets(TContainer& c) {
        uint64_t val;
        TError error = c.Tclass->GetStat(ETclassStat::Packets, val);
        if (error) {
            TLogger::LogError(error, "Can't get transmitted packets");
            return "-1";
        }

        return std::to_string(val);
    };

    static string NetDrops(TContainer& c) {
        uint64_t val;
        TError error = c.Tclass->GetStat(ETclassStat::Drops, val);
        if (error) {
            TLogger::LogError(error, "Can't get dropped packets");
            return "-1";
        }

        return std::to_string(val);
    };

    static string NetOverlimits(TContainer& c) {
        uint64_t val;
        TError error = c.Tclass->GetStat(ETclassStat::Overlimits, val);
        if (error) {
            TLogger::LogError(error, "Can't get number of packets over limit");
            return "-1";
        }

        return std::to_string(val);
    };

    static string MinorFaults(TContainer& c) {
        uint64_t val;
        auto cg = c.GetLeafCgroup(memorySubsystem);
        TError error = memorySubsystem->Statistics(cg, "pgfault", val);
        if (error)
            return "-1";

        return std::to_string(val);
    };

    static string MajorFaults(TContainer& c) {
        uint64_t val;
        auto cg = c.GetLeafCgroup(memorySubsystem);
        TError error = memorySubsystem->Statistics(cg, "pgmajfault", val);
        if (error)
            return "-1";

        return std::to_string(val);
    };

    static string IoRead(TContainer& c) {
        auto cg = c.GetLeafCgroup(blkioSubsystem);

        vector<BlkioStat> stat;
        TError error = blkioSubsystem->Statistics(cg, "blkio.io_service_bytes_recursive", stat);
        if (error)
            return "-1";

        std::stringstream str;
        for (auto &s : stat) {
            if (str.str().length())
                str << " ";
            str << s.Device << ":" << s.Read;
        }

        return str.str();
    };

    static string IoWrite(TContainer& c) {
        auto cg = c.GetLeafCgroup(blkioSubsystem);

        vector<BlkioStat> stat;
        TError error = blkioSubsystem->Statistics(cg, "blkio.io_service_bytes_recursive", stat);
        if (error)
            return "-1";

        std::stringstream str;
        for (auto &s : stat) {
            if (str.str().length())
                str << " ";
            str << s.Device << ":" << s.Write;
        }

        return str.str();
    };

    static string RunningTime(TContainer& c) {
        if (!c.Task || !c.Task->IsRunning())
            return "0";

        int pid = c.Task->GetPid();
        TFile f("/proc/" + std::to_string(pid) + "/stat");
        string line;
        if (f.AsString(line))
            return "0";

        vector<string> cols;
        if (SplitString(line, ' ', cols))
            return "0";

        if (cols.size() <= 21)
            return "0";

        int64_t started;
        if (StringToInt64(cols[21], started))
            return "0";

        started /= sysconf(_SC_CLK_TCK);
        started += BootTime;

        return std::to_string(time(nullptr) - started);
    };
};

std::map<std::string, const TDataSpec> dataSpec = {
    { "state", { "container state", ROOT_DATA, TData::State, { EContainerState::Stopped, EContainerState::Dead, EContainerState::Running, EContainerState::Paused } } },
    { "oom_killed", { "indicates whether container has been killed by OOM", 0, TData::OomKilled, { EContainerState::Dead } } },
    { "parent", { "container parent", 0, TData::Parent, { EContainerState::Stopped, EContainerState::Dead, EContainerState::Running, EContainerState::Paused } } },
    { "respawn_count", { "how many time container was automatically respawned", 0, TData::RespawnCount, { EContainerState::Running, EContainerState::Dead } } },
    { "exit_status", { "container exit status", 0, TData::ExitStatus, { EContainerState::Dead } } },
    { "start_errno", { "container start error", 0, TData::StartErrno, { EContainerState::Stopped } } },
    { "root_pid", { "root process id", 0, TData::RootPid, { EContainerState::Running, EContainerState::Paused } } },
    { "stdout", { "return task stdout", 0, TData::Stdout, { EContainerState::Running, EContainerState::Paused, EContainerState::Dead } } },
    { "stderr", { "return task stderr", 0, TData::Stderr, { EContainerState::Running, EContainerState::Paused, EContainerState::Dead } } },
    { "cpu_usage", { "return consumed CPU time in nanoseconds", ROOT_DATA, TData::CpuUsage, { EContainerState::Running, EContainerState::Paused, EContainerState::Dead } } },
    { "net_bytes", { "number of tx bytes", ROOT_DATA, TData::NetBytes, { EContainerState::Running, EContainerState::Paused, EContainerState::Dead } } },
    { "net_packets", { "number of tx packets", ROOT_DATA, TData::NetPackets, { EContainerState::Running, EContainerState::Paused, EContainerState::Dead } } },
    { "net_drops", { "number of dropped tx packets", ROOT_DATA, TData::NetDrops, { EContainerState::Running, EContainerState::Paused, EContainerState::Dead } } },
    { "net_overlimits", { "number of tx packets that exceeded the limit", ROOT_DATA, TData::NetOverlimits, { EContainerState::Running, EContainerState::Paused, EContainerState::Dead } } },
    { "memory_usage", { "return consumed memory in bytes", ROOT_DATA, TData::MemUsage, { EContainerState::Running, EContainerState::Paused, EContainerState::Dead } } },
    { "minor_faults", { "return number of minor page faults", ROOT_DATA, TData::MinorFaults, { EContainerState::Running, EContainerState::Paused, EContainerState::Dead } } },
    { "major_faults", { "return number of major page faults", ROOT_DATA, TData::MajorFaults, { EContainerState::Running, EContainerState::Paused, EContainerState::Dead } } },

    { "io_read", { "return number of bytes read from disk", ROOT_DATA | HIDDEN_DATA, TData::IoRead, { EContainerState::Running, EContainerState::Paused, EContainerState::Dead } } },
    { "io_write", { "return number of bytes written to disk", ROOT_DATA | HIDDEN_DATA, TData::IoWrite, { EContainerState::Running, EContainerState::Paused, EContainerState::Dead } } },

    { "time", { "return running time of container", 0, TData::RunningTime, { EContainerState::Running, EContainerState::Paused, EContainerState::Dead } } },
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

    /*
    if (Filter) {
        TError error = Filter->Remove();
        TLogger::LogError(error, "Can't remove tc filter");
    }
    */

    if (IsRoot())
        FreeResources();

    if (DefaultTclass) {
        TError error = DefaultTclass->Remove();
        TLogger::LogError(error, "Can't remove default tc class");
    }

    if (Qdisc) {
        TError error = Qdisc->Remove();
        TLogger::LogError(error, "Can't remove tc qdisc");
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

std::string TContainer::GetPropertyStr(const std::string &property) const {
    return Spec.Get(shared_from_this(), property);
}

int TContainer::GetPropertyInt(const std::string &property) const {
    int val;

    if (StringToInt(GetPropertyStr(property), val))
        return 0;

    return val;
}

uint64_t TContainer::GetPropertyUint64(const std::string &property) const {
    uint64_t val;

    if (StringToUint64(GetPropertyStr(property), val))
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
    if (memroot->HasKnob("memory.low_limit_in_bytes") && GetPropertyInt("memory_guarantee") != 0) {
        TError error = memcg->SetKnobValue("memory.low_limit_in_bytes", GetPropertyStr("memory_guarantee"), false);
        TLogger::LogError(error, "Can't set memory_guarantee");
        if (error)
            return error;
    }

    if (GetPropertyInt("memory_limit") != 0) {
        error = memcg->SetKnobValue("memory.limit_in_bytes", GetPropertyStr("memory_limit"), false);
        TLogger::LogError(error, "Can't set memory_limit");
        if (error)
            return error;
    }

    if (memroot->HasKnob("memory.recharge_on_pgfault")) {
        string value = GetPropertyStr("recharge_on_pgfault") == "true" ? "1" : "0";
        error = memcg->SetKnobValue("memory.recharge_on_pgfault", value, false);
        TLogger::LogError(error, "Can't set recharge_on_pgfault");
        if (error)
            return error;
    }

    auto cpucg = GetLeafCgroup(cpuSubsystem);
    if (GetPropertyStr("cpu_policy") == "normal") {
        int cpuPrio;
        error = StringToInt(GetPropertyStr("cpu_priority"), cpuPrio);
        TLogger::LogError(error, "Can't parse cpu_priority");
        if (error)
            return error;

        error = cpucg->SetKnobValue("cpu.shares", std::to_string(cpuPrio + 2), false);
        TLogger::LogError(error, "Can't set cpu_priority");
        if (error)
            return error;
    }

    return TError::Success();
}

TError TContainer::PrepareNetwork() {
    PORTO_ASSERT(Tclass == nullptr);

    if (Parent) {
        uint32_t handle = TcHandle(TcMajor(Parent->Tclass->GetHandle()), Id);
        auto tclass = Parent->Tclass;
        Tclass = std::make_shared<TTclass>(tclass, handle);
    } else {
        uint32_t handle = TcHandle(TcMajor(Qdisc->GetHandle()), Id);
        Tclass = std::make_shared<TTclass>(Qdisc, handle);
    }

    TError error;
    uint32_t prio, rate, ceil;

    error = StringToUint32(GetPropertyStr("net_priority"), prio);
    if (error)
        return error;

    error = StringToUint32(GetPropertyStr("net_guarantee"), rate);
    if (error)
        return error;

    error = StringToUint32(GetPropertyStr("net_ceil"), ceil);
    if (error)
        return error;

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
        if (GetPropertyStr("cpu_policy") == "rt") {
            error = cpucg->SetKnobValue("cpu.smart", "1", false);
            TLogger::LogError(error, "Can't enable smart");
            if (error)
                return error;
        }
    }

    if (config().network().enabled()) {
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

    return TError::Success();
}

TError TContainer::PrepareTask() {
    TTaskEnv taskEnv;

    taskEnv.Command = GetPropertyStr("command");
    taskEnv.Cwd = GetPropertyStr("cwd");
    taskEnv.CreateCwd = Spec.IsDefault(shared_from_this(), "cwd");
    //taskEnv.Root = GetPropertyStr("root");
    taskEnv.User = GetPropertyStr("user");
    taskEnv.Group = GetPropertyStr("group");
    taskEnv.Environ = GetPropertyStr("env");
    taskEnv.Isolate = GetPropertyStr("isolate") == "true";
    taskEnv.StdinPath = GetPropertyStr("stdin_path");
    taskEnv.StdoutPath = GetPropertyStr("stdout_path");
    taskEnv.StderrPath = GetPropertyStr("stderr_path");

    TError error = taskEnv.Prepare();
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

    TError error = Spec.Create();
    if (error)
        return error;

    if (Uid >= 0) {
        TUser u(Uid);
        error = u.Load();
        if (error)
            return error;
        Spec.SetRaw("user", u.GetName());
    }

    if (Gid >= 0) {
        TGroup g(Gid);
        error = g.Load();
        if (error)
            return error;
        Spec.SetRaw("group", g.GetName());
    }

    Spec.SetRaw("uid", std::to_string(Uid));
    Spec.SetRaw("gid", std::to_string(Gid));

    if (Parent)
        Parent->Children.push_back(std::weak_ptr<TContainer>(shared_from_this()));

    if (IsRoot()) {
        // 1:0 qdisc
        // 1:2 default class    1:1 root class
        // (unclassified        1:3 container a, 1:4 container b
        //          traffic)    1:5 container a/c

        uint32_t defHandle = TcHandle(Id, Id + 1);
        uint32_t rootHandle = TcHandle(Id, 0);

        Qdisc = std::make_shared<TQdisc>(rootHandle, defHandle);
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
        TLogger::Log() << "Maybe running" << std::endl;
        MaybeReturnedOk = false;
        return TError::Success();
    }
    MaybeReturnedOk = false;
    RespawnCount = 0;

    if (!CheckState(EContainerState::Stopped))
        return TError(EError::InvalidState, "invalid container state " + ContainerStateName(State));

    if (Parent && !Parent->IsRoot() && Parent->State != EContainerState::Running)
        return TError(EError::InvalidState, "parent is not running");

    Spec.SetRaw("id", std::to_string(Id));
    OomKilled = false;

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

    if (IsRoot()) {
        State = EContainerState::Running;
        return TError::Success();
    }

    if (!GetPropertyStr("command").length()) {
        FreeResources();
        return TError(EError::InvalidValue, "container command is empty");
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
        TaskStartErrno = error.GetErrno();
        return error;
    }
    TaskStartErrno = -1;

    TLogger::Log() << GetName() << " started " << std::to_string(Task->GetPid()) << std::endl;

    Spec.SetRaw("root_pid", std::to_string(Task->GetPid()));
    State = EContainerState::Running;

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
            if (child->State != EContainerState::Stopped && child->State != EContainerState::Dead)
                child->Stop();
        } else {
            TLogger::Log() << "Warning: can't lock child while stopping" << std::endl;
        }
    }
}

void TContainer::FreeResources() {
    LeafCgroups.clear();

    if (Tclass) {
        TError error = Tclass->Remove();
        Tclass = nullptr;
        TLogger::LogError(error, "Can't remove tc classifier");
    }

    Task = nullptr;
    TaskStartErrno = -1;
    Efd = -1;
}

TError TContainer::Stop() {
    if (IsRoot() || !(CheckState(EContainerState::Running) || CheckState(EContainerState::Dead)))
        return TError(EError::InvalidState, "invalid container state " + ContainerStateName(State));

    TLogger::Log() << "Stop " << GetName() << " " << Id << std::endl;

    int pid = Task->GetPid();

    TError error = KillAll();
    if (error)
        TLogger::LogError(error, "Can't kill all tasks in container");

    int ret = SleepWhile(1000, [&]{ kill(pid, 0); return errno != ESRCH; });
    if (ret)
        TLogger::Log() << "Error while waiting for container to stop" << std::endl;

    AckExitStatus(pid);
    Task->DeliverExitStatus(-1);

    State = EContainerState::Stopped;

    StopChildren();
    FreeResources();

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

TError TContainer::Kill(int sig) {
    if (IsRoot() || !CheckState(EContainerState::Running))
        return TError(EError::InvalidState, "invalid container state " + ContainerStateName(State));

    return Task->Kill(sig);
}

TError TContainer::GetData(const string &name, string &value) {
    if (dataSpec.find(name) == dataSpec.end())
        return TError(EError::InvalidValue, "invalid container data");

    if (IsRoot() && !(dataSpec[name].Flags & ROOT_DATA))
        return TError(EError::InvalidData, "invalid data for root container");

    if (dataSpec[name].Valid.find(State) == dataSpec[name].Valid.end())
        return TError(EError::InvalidState, "invalid container state " + ContainerStateName(State));

    value = dataSpec[name].Handler(*this);
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

    if (propertySpec.find(property) == propertySpec.end())
        return TError(EError::InvalidProperty, "invalid property");

    value = GetPropertyStr(property);
    PropertyToAlias(origProperty, value);

    return TError::Success();
}

bool TContainer::ShouldApplyProperty(const std::string &property) {
    if (!(Spec.GetFlags(property) & DYNAMIC_PROPERTY))
       return false;

    if (State == EContainerState::Dead || State == EContainerState::Stopped)
        return false;

    return true;
}

TError TContainer::SetProperty(const string &origProperty, const string &origValue, bool superuser) {
    if (IsRoot())
        return TError(EError::InvalidValue, "Can't set property for root");

    string property = origProperty;
    string value = StringTrim(origValue);

    TError error = AliasToProperty(property, value);
    if (error)
        return error;

    if (propertySpec.find(property) == propertySpec.end())
        return TError(EError::InvalidProperty, "invalid property");

    if ((Spec.GetFlags(property) & SUPERUSER_PROPERTY) && !superuser)
        return TError(EError::Permission, "Only root can change this property");

    if (State != EContainerState::Stopped && !(Spec.GetFlags(property) & DYNAMIC_PROPERTY))
        return TError(EError::InvalidValue, "Can't set dynamic property " + property + " for running container");

    error = Spec.Set(shared_from_this(), property, value);
    if (error)
        return error;

    if (ShouldApplyProperty(property))
        error = ApplyDynamicProperties();

    return error;
}

TError TContainer::Restore(const kv::TNode &node) {
    TLogger::Log() << "Restore " << GetName() << " " << Id << std::endl;

    TError error = Spec.Restore(node);
    if (error) {
        TLogger::LogError(error, "Can't restore task's spec");
        return error;
    }

    int pid = 0;
    bool started = true;
    string pidStr;
    error = Spec.GetRaw("root_pid", pidStr);
    if (error) {
        started = false;
    } else {
        error = StringToInt(pidStr, pid);
        if (error)
            started = false;
    }

    TLogger::Log() << GetName() << ": restore process " << std::to_string(pid) << " which " << (started ? "started" : "didn't start") << std::endl;

    string s;
    error = Spec.GetRaw("uid", s);
    TLogger::LogError(error, "Can't restore uid");
    if (!error) {
        error = StringToInt(s, Uid);
        TLogger::LogError(error, "Can't parse uid");
    }

    error = Spec.GetRaw("gid", s);
    TLogger::LogError(error, "Can't restore gid");
    if (!error) {
        error = StringToInt(s, Gid);
        TLogger::LogError(error, "Can't parse gid");
    }

    State = EContainerState::Stopped;

    if (started) {
        error = PrepareNetwork();
        if (error) {
            TLogger::LogError(error, "Can't prepare task network");
            return error;
        }

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

            auto cg = GetLeafCgroup(freezerSubsystem);
            if (cg->Exists())
                (void)KillAll();

            TLogger::LogError(error, "Can't restore task");
            return error;
        }

        State = Task->IsRunning() ? EContainerState::Running : EContainerState::Stopped;
        if (State == EContainerState::Running)
            MaybeReturnedOk = true;
    } else {
        auto cg = GetLeafCgroup(freezerSubsystem);
        if (IsAlive()) {
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

    return Parent->GetLeafCgroup(subsys)->GetChild(Name);
}

bool TContainer::DeliverExitStatus(int pid, int status) {
    if (State != EContainerState::Running || !Task)
        return false;

    if (Task->GetPid() != pid)
        return false;

    Task->DeliverExitStatus(status);
    TLogger::Log() << "Delivered " << status << " to " << GetName() << " with root_pid " << Task->GetPid() << std::endl;
    State = EContainerState::Dead;

    if (GetPropertyStr("isolate") == "false")
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
    if (State != EContainerState::Dead)
        return false;

    return GetPropertyStr("respawn") == "true" && TimeOfDeath + config().container().respawn_delay_ms() <= GetCurrentTimeMs();
}

TError TContainer::Respawn() {
    TError error = Stop();
    if (error)
        return error;

    size_t tmp = RespawnCount;
    error = Start();
    RespawnCount = tmp + 1;
    if (error)
        return error;

    return TError::Success();
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
    return State == EContainerState::Dead && TimeOfDeath + config().container().aging_time_ms() <= GetCurrentTimeMs();
}

bool TContainer::HasChildren() const {
    // link #1 - this
    // link #2 - TContainerHolder->Containers
    // any other link comes from TContainer->Parent and indicates that
    // current container has children
    return shared_from_this().use_count() > 2;
}

uint16_t TContainer::GetId() {
    return Id;
}

int TContainer::GetOomFd() {
    return Efd.GetFd();
}

void TContainer::DeliverOom() {
    if (IsRoot() || !(CheckState(EContainerState::Running) || CheckState(EContainerState::Dead))) {
        TError error(EError::InvalidState, "invalid container state " + ContainerStateName(State));
        TLogger::LogError(error, "Can't deliver OOM");
    }

    int pid = Task->GetPid();
    TLogger::Log() << "Delivered OOM to " << GetName() << " with root_pid " << pid << std::endl;

    TError error = KillAll();
    if (error)
        TLogger::LogError(error, "Can't kill all tasks in container");

    AckExitStatus(pid);
    Task->DeliverExitStatus(SIGKILL);
    State = EContainerState::Dead;;

    StopChildren();
    OomKilled = true;
    Efd = -1;
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
    BootTime = GetBootTime();

    TError error = Create(ROOT_CONTAINER, -1, -1);
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

    auto c = std::make_shared<TContainer>(name, parent, id);
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

        if (key == "id")
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

    auto c = std::make_shared<TContainer>(name, parent, id);
    error = c->Restore(node);
    if (error)
        return error;

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

void TContainerHolder::DeliverOom(int fd) {
    for (auto c : Containers) {
        if (fd != c.second->GetOomFd())
            continue;

        c.second->DeliverOom();
        return;
    }

    TLogger::Log() << "Couldn't deliver OOM notification to " << fd << std::endl;
}
