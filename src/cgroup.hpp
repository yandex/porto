#pragma once

#include <string>

#include "common.hpp"
#include "config.hpp"
#include "util/path.hpp"

struct TDevice;
class TCgroup;

#define CGROUP_FREEZER  0x00001ull
#define CGROUP_MEMORY   0x00002ull
#define CGROUP_CPU      0x00004ull
#define CGROUP_CPUACCT  0x00008ull
#define CGROUP_NETCLS   0x00010ull
#define CGROUP_BLKIO    0x00020ull
#define CGROUP_DEVICES  0x00040ull
#define CGROUP_HUGETLB  0x00080ull
#define CGROUP_CPUSET   0x00100ull
#define CGROUP_PIDS     0x00200ull
#define CGROUP_PERF     0x00400ull
#define CGROUP_SYSTEMD  0x01000ull
#define CGROUP2         0x10000ull

extern const TFlagsNames ControllersName;

class TSubsystem {
public:
    const uint64_t Kind = 0x0ull;
    uint64_t Controllers = 0x0ull;
    const std::string Type;
    const std::string MountType;
    const TSubsystem *Hierarchy = nullptr;
    TPath Root;
    TFile Base;
    bool Supported = false;

    TSubsystem(uint64_t kind, const std::string &type)
        : Kind(kind)
        , Type(type)
        , MountType(Kind == CGROUP2 ? "cgroup2" : "cgroup")
    { }
    virtual bool IsDisabled() { return false; }
    virtual bool IsOptional() { return false; }
    virtual std::string TestOption() const { return Type; }
    virtual std::vector<std::string> MountOptions() const { return {Type}; }

    virtual TError InitializeSubsystem() {
        return OK;
    }

    virtual TError InitializeCgroup(TCgroup &cgroup) {
        (void)cgroup;
        return OK;
    }

    TCgroup RootCgroup() const;
    TCgroup Cgroup(const std::string &name) const;

    TError TaskCgroup(pid_t pid, TCgroup &cgroup) const;
    bool IsBound(const TCgroup &cgroup) const;

    static std::string Format(uint64_t controllers) {
        return StringFormatFlags(controllers, ControllersName, ";");
    }
};

class TCgroup {
public:
    static bool IsRestore;
    const TSubsystem *Subsystem = nullptr;
    std::string Name;

    TCgroup() { }
    TCgroup(const TSubsystem *subsystem, const std::string &name) :
        Subsystem(subsystem), Name(name) { }

    bool Secondary() const {
        return !Subsystem || Subsystem->Hierarchy != Subsystem;
    }

    std::string Type() const {
        return Subsystem ? Subsystem->Type : "(null)";
    }

    friend std::ostream& operator<<(std::ostream& os, const TCgroup& cgroup) {
        return os << cgroup.Type() << ":" << cgroup.Name;
    }

    friend bool operator==(const TCgroup& lhs, const TCgroup& rhs) {
        return lhs.Name == rhs.Name;
    }

    friend bool operator!=(const TCgroup& lhs, const TCgroup& rhs) {
        return lhs.Name != rhs.Name;
    }

    TCgroup Child(const std::string& name) const;
    TError ChildsAll(std::vector<TCgroup> &cgroups, bool all = false) const;
    bool IsChildOf(const TCgroup &parent) const;

    TPath Path() const;
    bool IsRoot() const;
    bool Exists() const;

    bool IsSubsystem(uint64_t kind) const {
        return Subsystem && Subsystem->Kind == kind;
    }
    bool IsNetcls() const {
        return IsSubsystem(CGROUP_NETCLS);
    }
    bool IsCgroup2() const {
        return IsSubsystem(CGROUP2);
    }

    TError Create();
    TError Rename(TCgroup &target);
    TError Remove();
    TError RemoveOne();

    TError KillAll(int signal) const;

    TError GetProcesses(std::vector<pid_t> &pids) const {
        return GetPids("cgroup.procs", pids);
    }

    TError GetTasks(std::vector<pid_t> &pids) const {
        if (IsCgroup2())
            return GetPids("cgroup.threads", pids);
        return GetPids("tasks", pids);
    }

    TError GetCount(bool threads, uint64_t &count) const;

    bool IsEmpty() const;

    static void StartRestore();
    static void FinishRestore();

    TError Attach(pid_t pid, bool thread = false) const;
    TError AttachAll(const TCgroup &cg) const;

    TPath Knob(const std::string &knob) const;
    bool Has(const std::string &knob) const;
    TError Get(const std::string &knob, std::string &value) const;
    TError Set(const std::string &knob, const std::string &value) const;

    TError GetPids(const std::string &knob, std::vector<pid_t> &pids) const;

    TError GetInt64(const std::string &knob, int64_t &value) const;
    TError SetInt64(const std::string &knob, int64_t value) const;

    TError GetUint64(const std::string &knob, uint64_t &value) const;
    TError SetUint64(const std::string &knob, uint64_t value) const;

    TError GetBool(const std::string &knob, bool &value) const;
    TError SetBool(const std::string &knob, bool value) const;

    TError GetUintMap(const std::string &knob, TUintMap &value) const;

    TError Recreate();
};

class TMemorySubsystem : public TSubsystem {
public:
    const std::string STAT = "memory.stat";
    const std::string OOM_CONTROL = "memory.oom_control";
    const std::string EVENT_CONTROL = "cgroup.event_control";
    const std::string USE_HIERARCHY = "memory.use_hierarchy";
    const std::string RECHARGE_ON_PAGE_FAULT = "memory.recharge_on_pgfault";
    const std::string USAGE = "memory.usage_in_bytes";
    const std::string LIMIT = "memory.limit_in_bytes";
    const std::string SOFT_LIMIT = "memory.soft_limit_in_bytes";
    const std::string LOW_LIMIT = "memory.low_limit_in_bytes";
    const std::string HIGH_LIMIT = "memory.high_limit_in_bytes";
    const std::string MEM_SWAP_LIMIT = "memory.memsw.limit_in_bytes";
    const std::string DIRTY_LIMIT = "memory.dirty_limit_in_bytes";
    const std::string DIRTY_RATIO = "memory.dirty_ratio";
    const std::string FS_BPS_LIMIT = "memory.fs_bps_limit";
    const std::string FS_IOPS_LIMIT = "memory.fs_iops_limit";
    const std::string ANON_USAGE = "memory.anon.usage";
    const std::string ANON_MAX_USAGE = "memory.anon.max_usage";
    const std::string ANON_LIMIT = "memory.anon.limit";
    const std::string ANON_ONLY = "memory.anon.only";

    TMemorySubsystem() : TSubsystem(CGROUP_MEMORY, "memory") {}

    TError Statistics(TCgroup &cg, TUintMap &stat) const {
        return cg.GetUintMap(STAT, stat);
    }

    TError Usage(TCgroup &cg, uint64_t &value) const {
        return cg.GetUint64(USAGE, value);
    }

    TError GetSoftLimit(TCgroup &cg, int64_t &limit) const {
        return cg.GetInt64(SOFT_LIMIT, limit);
    }

    TError SetSoftLimit(TCgroup &cg, int64_t limit) const {
        return cg.SetInt64(SOFT_LIMIT, limit);
    }

    bool SupportGuarantee() const {
        return RootCgroup().Has(LOW_LIMIT);
    }

    TError SetGuarantee(TCgroup &cg, uint64_t guarantee) const {
        if (!SupportGuarantee())
            return OK;
        return cg.SetUint64(LOW_LIMIT, guarantee);
    }

    bool SupportHighLimit() const {
        return RootCgroup().Has(HIGH_LIMIT);
    }

    TError SetHighLimit(TCgroup &cg, uint64_t limit) const {
        if (!SupportHighLimit())
            return OK;
        return cg.SetUint64(HIGH_LIMIT, limit);
    }

    bool SupportIoLimit() const {
        return RootCgroup().Has(FS_BPS_LIMIT);
    }

    bool SupportDirtyLimit() const {
        return RootCgroup().Has(DIRTY_LIMIT);
    }

    bool SupportSwap() const {
        return RootCgroup().Has(MEM_SWAP_LIMIT);
    }

    bool SupportRechargeOnPgfault() const {
        return RootCgroup().Has(RECHARGE_ON_PAGE_FAULT);
    }

    TError RechargeOnPgfault(TCgroup &cg, bool enable) const {
        if (!SupportRechargeOnPgfault())
            return OK;
        return cg.SetBool(RECHARGE_ON_PAGE_FAULT, enable);
    }

    TError GetCacheUsage(TCgroup &cg, uint64_t &usage) const;
    TError GetShmemUsage(TCgroup &cg, uint64_t &usage) const;
    TError GetMLockUsage(TCgroup &cg, uint64_t &usage) const;
    TError GetAnonUsage(TCgroup &cg, uint64_t &usage) const;

    TError GetAnonMaxUsage(TCgroup &cg, uint64_t &usage) const {
        return cg.GetUint64(ANON_MAX_USAGE, usage);
    }
    TError ResetAnonMaxUsage(TCgroup &cg) const {
        return cg.SetUint64(ANON_MAX_USAGE, 0);
    }

    bool SupportAnonLimit() const;
    TError SetAnonLimit(TCgroup &cg, uint64_t limit) const;

    bool SupportAnonOnly() const;
    TError SetAnonOnly(TCgroup &cg, bool val) const;

    TError SetLimit(TCgroup &cg, uint64_t limit);
    TError SetIoLimit(TCgroup &cg, uint64_t limit);
    TError SetIopsLimit(TCgroup &cg, uint64_t limit);
    TError SetDirtyLimit(TCgroup &cg, uint64_t limit);
    TError SetupOOMEvent(TCgroup &cg, TFile &event);
    uint64_t GetOomEvents(TCgroup &cg);
    TError GetOomKills(TCgroup &cg, uint64_t &count);
    TError GetReclaimed(TCgroup &cg, uint64_t &count) const;
};

class TFreezerSubsystem : public TSubsystem {
public:
    TFreezerSubsystem() : TSubsystem(CGROUP_FREEZER, "freezer") {}

    TError WaitState(const TCgroup &cg, const std::string &state) const;
    TError Freeze(const TCgroup &cg, bool wait = true) const;
    TError Thaw(const TCgroup &cg, bool wait = true) const;
    bool IsFrozen(const TCgroup &cg) const;
    bool IsSelfFreezing(const TCgroup &cg) const;
    bool IsParentFreezing(const TCgroup &cg) const;
};

class TCpuSubsystem : public TSubsystem {
public:
    bool HasShares = false;
    bool HasQuota = false;
    bool HasReserve = false;
    bool HasRtGroup = false;

    uint64_t BaseShares = 0ull;
    uint64_t MinShares = 0ull;
    uint64_t MaxShares = 0ull;

    TCpuSubsystem() : TSubsystem(CGROUP_CPU, "cpu") { }
    TError InitializeSubsystem() override;
    TError InitializeCgroup(TCgroup &cg) override;
    TError SetPeriod(TCgroup &cg, uint64_t period);
    TError SetLimit(TCgroup &cg, uint64_t period, uint64_t limit);
    TError SetRtLimit(TCgroup &cg, uint64_t period, uint64_t limit);
    TError SetGuarantee(TCgroup &cg, uint64_t period, uint64_t guarantee);
    TError SetShares(TCgroup &cg, const std::string &policy, double weight, uint64_t guarantee);
};

class TCpuacctSubsystem : public TSubsystem {
public:
    TCpuacctSubsystem() : TSubsystem(CGROUP_CPUACCT, "cpuacct") {}
    TError Usage(TCgroup &cg, uint64_t &value) const;
    TError SystemUsage(TCgroup &cg, uint64_t &value) const;
};

class TCpusetSubsystem : public TSubsystem {
public:
    TCpusetSubsystem() : TSubsystem(CGROUP_CPUSET, "cpuset") {}
    bool IsOptional() override { return true; }
    TError InitializeCgroup(TCgroup &cg) override;

    TError GetCpus(TCgroup &cg, std::string &cpus) const {
        return cg.Get("cpuset.cpus", cpus);
    }
    TError SetCpus(TCgroup &cg, const std::string &cpus) const;
    TError SetMems(TCgroup &cg, const std::string &mems) const;
};

class TNetclsSubsystem : public TSubsystem {
public:
    bool HasPriority;
    TNetclsSubsystem() : TSubsystem(CGROUP_NETCLS, "net_cls") {}
    bool IsOptional() override { return !config().network().enable_host_net_classes(); }
    TError InitializeSubsystem() override;
    TError SetClass(TCgroup &cg, uint32_t classid) const;
};

class TBlkioSubsystem : public TSubsystem {
public:
    const std::string CFQ_WEIGHT = "blkio.weight";
    const std::string BFQ_WEIGHT = "blkio.bfq.weight";

    bool HasThrottler = false;
    bool HasSaneBehavior = false;
    TBlkioSubsystem() : TSubsystem(CGROUP_BLKIO, "blkio") {}
    bool IsDisabled() override { return !config().container().enable_blkio(); }
    bool IsOptional() override { return true; }
    TError InitializeSubsystem() override {
        HasThrottler = RootCgroup().Has("blkio.throttle.read_bps_device");
        if (RootCgroup().GetBool("cgroup.sane_behavior", HasSaneBehavior))
            HasSaneBehavior = false;
        return OK;
    }
    enum IoStat {
        Read = 1,
        Write = 2,
        Iops = 4,
        ReadIops = Read | Iops,
        WriteIops = Write | Iops,
        Time = 8,
    };
    TError GetIoStat(TCgroup &cg, enum IoStat stat, TUintMap &map) const;
    TError SetIoWeight(TCgroup &cg, const std::string &policy, double weight) const;
    TError SetIoLimit(TCgroup &cg, const TPath &root, const TUintMap &map, bool iops = false);

    TError DiskName(const std::string &disk, std::string &name) const;
    TError ResolveDisk(const TPath &root, const std::string &key, std::string &disk) const;
};

class TDevicesSubsystem : public TSubsystem {
public:
    TDevicesSubsystem() : TSubsystem(CGROUP_DEVICES, "devices") {}
};

class THugetlbSubsystem : public TSubsystem {
public:
    const std::string HUGE_USAGE = "hugetlb.2MB.usage_in_bytes";
    const std::string HUGE_LIMIT = "hugetlb.2MB.limit_in_bytes";
    const std::string GIGA_USAGE = "hugetlb.1GB.usage_in_bytes";
    const std::string GIGA_LIMIT = "hugetlb.1GB.limit_in_bytes";
    THugetlbSubsystem() : TSubsystem(CGROUP_HUGETLB, "hugetlb") {}
    bool IsDisabled() override { return !config().container().enable_hugetlb(); }
    bool IsOptional() override { return true; }

    /* for now supports only 2MB pages */
    TError InitializeSubsystem() override {
        if (!RootCgroup().Has(HUGE_LIMIT))
            return TError(EError::NotSupported, "No {}", HUGE_LIMIT);
        return OK;
    }

    TError GetHugeUsage(TCgroup &cg, uint64_t &usage) const {
        return cg.GetUint64(HUGE_USAGE, usage);
    }

    TError SetHugeLimit(TCgroup &cg, int64_t limit) const {
        return cg.SetInt64(HUGE_LIMIT, limit);
    }

    bool SupportGigaPages() const {
        return RootCgroup().Has(GIGA_LIMIT);
    }

    TError SetGigaLimit(TCgroup &cg, int64_t limit) const {
        return cg.SetInt64(GIGA_LIMIT, limit);
    }
};

class TPidsSubsystem : public TSubsystem {
public:
    TPidsSubsystem() : TSubsystem(CGROUP_PIDS, "pids") {}
    bool IsOptional() override { return true; }
    TError GetUsage(TCgroup &cg, uint64_t &usage) const;
    TError SetLimit(TCgroup &cg, uint64_t limit) const;
};

class TPerfSubsystem : public TSubsystem {
public:
    TPerfSubsystem() : TSubsystem(CGROUP_PERF, "perf_event") {}
};

class TSystemdSubsystem : public TSubsystem {
public:
    TCgroup PortoService;
    TSystemdSubsystem() : TSubsystem(CGROUP_SYSTEMD, "systemd") {}
    TError InitializeSubsystem() override;
    bool IsDisabled() override { return !config().container().enable_systemd(); }
    bool IsOptional() override { return true; }
    std::string TestOption() const override { return "name=" + Type; }
    std::vector<std::string> MountOptions() const override { return { "none", "name=" + Type }; }
};

class TCgroup2Subsystem : public TSubsystem {
public:
    TCgroup2Subsystem() : TSubsystem(CGROUP2, "unified") { }
    bool IsDisabled() override { return !config().container().enable_cgroup2(); }
    bool IsOptional() override { return true; }
    std::string TestOption() const override { return ""; }
    std::vector<std::string> MountOptions() const override { return {}; }
};

extern TMemorySubsystem     MemorySubsystem;
extern TFreezerSubsystem    FreezerSubsystem;
extern TCpuSubsystem        CpuSubsystem;
extern TCpuacctSubsystem    CpuacctSubsystem;
extern TCpusetSubsystem     CpusetSubsystem;
extern TNetclsSubsystem     NetclsSubsystem;
extern TBlkioSubsystem      BlkioSubsystem;
extern TDevicesSubsystem    DevicesSubsystem;
extern THugetlbSubsystem    HugetlbSubsystem;
extern TPidsSubsystem       PidsSubsystem;
extern TPerfSubsystem       PerfSubsystem;
extern TSystemdSubsystem    SystemdSubsystem;
extern TCgroup2Subsystem    Cgroup2Subsystem;

extern std::vector<TSubsystem *> AllSubsystems;
extern std::vector<TSubsystem *> Subsystems;
extern std::vector<TSubsystem *> Hierarchies;

TError InitializeCgroups();
TError InitializeDaemonCgroups();
