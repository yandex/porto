#include <algorithm>
#include <cmath>
#include <csignal>

#include "cgroup.hpp"
#include "device.hpp"
#include "config.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"

extern "C" {
#include <fcntl.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
}

const TFlagsNames ControllersName = {
    { CGROUP_FREEZER,   "freezer" },
    { CGROUP_MEMORY,    "memory" },
    { CGROUP_CPU,       "cpu" },
    { CGROUP_CPUACCT,   "cpuacct" },
    { CGROUP_NETCLS,    "net_cls" },
    { CGROUP_BLKIO,     "blkio" },
    { CGROUP_DEVICES,   "devices" },
    { CGROUP_HUGETLB,   "hugetlb" },
    { CGROUP_CPUSET,    "cpuset" },
    { CGROUP_PIDS,      "pids" },
};

TPath TCgroup::Path() const {
    if (!Subsystem)
        return TPath();
    return Subsystem->Root / Name;
}

TPath TCgroup::Knob(const std::string &knob) const {
    if (!Subsystem)
        return TPath();
    return Subsystem->Root / Name / knob;
}

bool TCgroup::IsRoot() const {
    return Name == "/";
}

bool TCgroup::Exists() const {
    if (!Subsystem)
        return false;
    return Path().IsDirectoryStrict();
}

TError TCgroup::Create() {
    TError error;

    if (Secondary())
        return TError(EError::Unknown, "Cannot create secondary cgroup " + Type());

    L_ACT("Create cgroup {}", *this);
    error = Path().Mkdir(0755);
    if (error)
        L_ERR("Cannot create cgroup {} : {}", *this, error);

    for (auto subsys: Subsystems) {
        if (subsys->IsEnabled(*this)) {
            error = subsys->InitializeCgroup(*this);
            if (error)
                return error;
        }
    }

    return error;
}

TError TCgroup::SetSuffix(const std::string suffix) {
    auto dir = Path().DirName();
    auto basename = Path().BaseName();
    auto pos = basename.find('#');

    if (pos != std::string::npos)
        basename = basename.substr(0, pos);

    if (suffix.size())
        basename += "#" + suffix;

    auto error = Path().Rename(dir / basename);

    if (!error)
        Name = (TPath(Name).DirName() / basename).ToString();

    return error;
}


TError TCgroup::Remove() {
    struct stat st;
    uint64_t count = 0;
    TError error;

    if (Secondary())
        return TError(EError::Unknown, "Cannot create secondary cgroup " + Type());

    L_ACT("Remove cgroup {}", *this);
    error = Path().Rmdir();

    /* workaround for bad synchronization */
    if (error && error.GetErrno() == EBUSY &&
        !Path().StatStrict(st) && st.st_nlink == 2) {
        uint64_t deadline = GetCurrentTimeMs() +
                            config().daemon().cgroup_remove_timeout_s() * 1000;

        do {
            (void)SetSuffix(std::to_string(count++));
            (void)KillAll(SIGKILL);

            error = Path().Rmdir();

            if (!error || error.GetErrno() != EBUSY)
                break;
        } while (!WaitDeadline(deadline));
    }

    if (error && (error.GetErrno() != ENOENT || Exists())) {
        std::vector<pid_t> tasks;
        GetTasks(tasks);
        L_ERR("Cannot remove cgroup {} : {}, {} tasks inside",
              *this, error, tasks.size());
    }

    return error;
}

bool TCgroup::Has(const std::string &knob) const {
    if (!Subsystem)
        return false;
    return Knob(knob).IsRegularStrict();
}

TError TCgroup::Get(const std::string &knob, std::string &value) const {
    if (!Subsystem)
        return TError(EError::Unknown, "Cannot get from null cgroup");
    return Knob(knob).ReadAll(value);
}

TError TCgroup::Set(const std::string &knob, const std::string &value) const {
    if (!Subsystem)
        return TError(EError::Unknown, "Cannot set to null cgroup");
    L_ACT("Set {} {} = {}", *this, knob, value);
    return Knob(knob).WriteAll(value);
}

TError TCgroup::GetInt64(const std::string &knob, int64_t &value) const {
    std::string string;
    TError error = Get(knob, string);
    if (!error)
        error = StringToInt64(string, value);
    return error;
}

TError TCgroup::SetInt64(const std::string &knob, int64_t value) const {
    return Set(knob, std::to_string(value));
}

TError TCgroup::GetUint64(const std::string &knob, uint64_t &value) const {
    std::string string;
    TError error = Get(knob, string);
    if (!error)
        error = StringToUint64(string, value);
    return error;
}

TError TCgroup::SetUint64(const std::string &knob, uint64_t value) const {
    return Set(knob, std::to_string(value));
}

TError TCgroup::GetBool(const std::string &knob, bool &value) const {
    std::string string;
    TError error = Get(knob, string);
    if (!error)
        value = StringTrim(string) != "0";
    return error;
}

TError TCgroup::SetBool(const std::string &knob, bool value) const {
    return Set(knob, value ? "1" : "0");
}

TError TCgroup::GetUintMap(const std::string &knob, TUintMap &value) const {
    if (!Subsystem)
        return TError(EError::Unknown, "Cannot get from null cgroup");

    FILE *file = fopen(Knob(knob).c_str(), "r");
    char *key;
    unsigned long long val;

    if (!file)
        return TError(EError::Unknown, errno, "Cannot open knob " + knob);

    while (fscanf(file, "%ms %llu\n", &key, &val) == 2) {
        value[std::string(key)] = val;
        free(key);
    }

    fclose(file);
    return TError::Success();
}

TError TCgroup::Attach(pid_t pid) const {
    if (Secondary())
        return TError(EError::Unknown, "Cannot attach to secondary cgroup " + Type());

    L_ACT("Attach process {} to {}", pid, *this);
    TError error = Knob("cgroup.procs").WriteAll(std::to_string(pid));
    if (error)
        L_ERR("Cannot attach process {} to {} : {}", pid, *this, error);

    return error;
}

TError TCgroup::AttachAll(const TCgroup &cg) const {
    if (Secondary())
        return TError(EError::Unknown, "Cannot attach to secondary cgroup " + Type());

    L_ACT("Attach all processes from {} to {}", cg, *this);

    std::vector<pid_t> pids, prev;
    bool retry;

    do {
        TError error = cg.GetProcesses(pids);
        if (error)
            return error;
        retry = false;
        for (auto pid: pids) {
            error = Knob("cgroup.procs").WriteAll(std::to_string(pid));
            if (error && error.GetErrno() != ESRCH)
                return error;
            retry = retry || std::find(prev.begin(), prev.end(), pid) == prev.end();
        }
        prev = pids;
    } while (retry);

    return TError::Success();
}

TCgroup TCgroup::Child(const std::string& name) const {
    PORTO_ASSERT(name[0] != '/');
    if (IsRoot())
        return TCgroup(Subsystem, "/" + name);
    return TCgroup(Subsystem, Name + "/" + name);
}

TError TCgroup::Childs(std::vector<TCgroup> &cgroups) const {
    std::vector<std::string> subdirs;

    TError error = Path().ListSubdirs(subdirs);
    if (error)
        return error;

    for (auto &name : subdirs) {
        if (IsRoot() && !StringStartsWith(name, PORTO_CGROUP_PREFIX + 1))
            continue;
        cgroups.push_back(Child(name));
    }
    return TError::Success();
}

TError TCgroup::ChildsAll(std::vector<TCgroup> &cgroups) const {
    TError error;

    error = Childs(cgroups);
    if (!error) {
        for (std::vector<TCgroup>::size_type i = 0; i < cgroups.size(); i++) {
            TCgroup cgroup = cgroups[i];
            TError error2 = cgroup.Childs(cgroups);
            if (error2) {
                L_ERR("Cannot dump childs of {} : {}", cgroup, error);
                if (!error)
                    error = error2;
            }
        }
    }

    return error;
}

TError TCgroup::GetPids(const std::string &knob, std::vector<pid_t> &pids) const {
    FILE *file;
    int pid;

    if (!Subsystem)
        return TError(EError::Unknown, "Cannot get from null cgroup");

    pids.clear();
    file = fopen(Knob(knob).c_str(), "r");
    if (!file)
        return TError(EError::Unknown, errno, "Cannot open knob " + knob);
    while (fscanf(file, "%d", &pid) == 1)
        pids.push_back(pid);
    fclose(file);

    return TError::Success();
}

TError TCgroup::GetCount(bool threads, uint64_t &count) const {
    std::vector<TCgroup> childs;
    TError error;

    if (!Subsystem)
        TError(EError::Unknown, "Cannot get from null cgroup");
    error = ChildsAll(childs);
    if (error)
        return error;
    childs.push_back(*this);
    count = 0;
    for (auto &cg: childs) {
        std::vector<pid_t> pids;
        error = cg.GetPids(threads ? "tasks" : "cgroup.procs", pids);
        if (error)
            break;
        count += pids.size();
    }
    return error;
}

bool TCgroup::IsEmpty() const {
    std::vector<pid_t> tasks;

    GetTasks(tasks);
    return tasks.empty();
}

TError TCgroup::KillAll(int signal) const {
    std::vector<pid_t> tasks, killed;
    TError error, error2;
    bool retry;
    bool frozen = false;
    int iteration = 0;

    L_ACT("KillAll {} {}", signal, *this);

    if (IsRoot())
        return TError(EError::Permission, "Bad idea");

    do {
        if (++iteration > 10 && !frozen && FreezerSubsystem.IsEnabled(*this) &&
                !FreezerSubsystem.IsFrozen(*this)) {
            error = FreezerSubsystem.Freeze(*this, false);
            if (error)
                L_ERR("Cannot freeze cgroup for killing {} : {}", *this, error);
            else
                frozen = true;
        }
        error = GetTasks(tasks);
        if (error)
            break;
        retry = false;
        for (auto pid: tasks) {
            if (std::find(killed.begin(), killed.end(), pid) == killed.end()) {
                if (kill(pid, signal) && errno != ESRCH && !error) {
                    error = TError(EError::Unknown, errno, "kill");
                    L_ERR("Cannot kill process {} : {}", pid, error);
                }
                retry = true;
            }
        }
        killed = tasks;
    } while (retry);

    if (frozen)
        (void)FreezerSubsystem.Thaw(*this, false);

    return error;
}

TCgroup TSubsystem::RootCgroup() const {
    return TCgroup(this, "/");
}

TCgroup TSubsystem::Cgroup(const std::string &name) const {
    PORTO_ASSERT(name[0] == '/');
    return TCgroup(this, name);
}

TError TSubsystem::TaskCgroup(pid_t pid, TCgroup &cgroup) const {
    std::vector<std::string> lines;
    auto cg_file = TPath("/proc/" + std::to_string(pid) + "/cgroup");

    TError error = cg_file.ReadLines(lines);
    if (error)
        return error;

    for (auto &line : lines) {
        std::vector<std::string> fields;
        bool found = false;

        error = SplitString(line, ':', fields, 3);
        if (error)
            return error;

        std::vector<std::string> cgroups;
        error = SplitString(fields[1], ',', cgroups);
        if (error)
            return error;

        for (auto &cg : cgroups)
            if (cg == Type)
                found = true;

        if (found) {
            cgroup.Subsystem = this;
            cgroup.Name = fields[2];
            return TError::Success();
        }
    }

    return TError(EError::Unknown, "Cannot find " + Type +
                    " cgroup for process " + std::to_string(pid));
}

bool TSubsystem::IsEnabled(const TCgroup &cgroup) const {
    return cgroup.Subsystem && (cgroup.Subsystem->Controllers & Kind);
}

// Memory
TError TMemorySubsystem::SetLimit(TCgroup &cg, uint64_t limit) {
    uint64_t old_limit, cur_limit, new_limit;
    TError error;

    /*
     * Maxumum value depends on arch, kernel version and bugs
     * "-1" works everywhere since 2.6.31
     */
    if (!limit) {
        if (SupportSwap())
            (void)cg.Set(MEM_SWAP_LIMIT, "-1");
        return cg.Set(LIMIT, "-1");
    }

    error = cg.GetUint64(LIMIT, old_limit);
    if (error)
        return error;

    if (old_limit == limit)
        return TError::Success();

    /* Memory limit cannot be bigger than Memory+Swap limit. */
    if (SupportSwap()) {
        cg.GetUint64(MEM_SWAP_LIMIT, cur_limit);
        if (cur_limit < limit)
            (void)cg.SetUint64(MEM_SWAP_LIMIT, limit);
    }

    cur_limit = old_limit;
    new_limit = limit;
    do {
        error = cg.SetUint64(LIMIT, new_limit);
        if (error) {
            if (cur_limit < INT64_MAX)
                new_limit = (cur_limit + new_limit) / 2;
            else
                new_limit *= 2;
        } else {
            cur_limit = new_limit;
            new_limit = limit;
        }
    } while (cur_limit != limit && new_limit <= cur_limit - 4096 &&
             (!error || error.GetErrno() == EBUSY));

    if (!error && SupportSwap())
        error = cg.SetUint64(MEM_SWAP_LIMIT, limit);

    if (error)
        (void)cg.SetUint64(LIMIT, old_limit);

    return error;
}

TError TMemorySubsystem::GetCacheUsage(TCgroup &cg, uint64_t &usage) const {
    TUintMap stat;
    TError error = Statistics(cg, stat);
    if (!error)
        usage = stat["total_inactive_file"] +
                stat["total_active_file"];
    return error;
}

TError TMemorySubsystem::GetAnonUsage(TCgroup &cg, uint64_t &usage) const {
    if (cg.Has(ANON_USAGE))
        return cg.GetUint64(ANON_USAGE, usage);

    TUintMap stat;
    TError error = Statistics(cg, stat);
    if (!error)
        usage = stat["total_inactive_anon"] +
                stat["total_active_anon"] +
                stat["total_unevictable"] +
                stat["total_swap"];
    return error;
}

bool TMemorySubsystem::SupportAnonLimit() const {
    return Cgroup(PORTO_DAEMON_CGROUP).Has(ANON_LIMIT);
}

TError TMemorySubsystem::SetAnonLimit(TCgroup &cg, uint64_t limit) const {
    if (cg.Has(ANON_LIMIT))
        return cg.Set(ANON_LIMIT, limit ? std::to_string(limit) : "-1");
    return TError::Success();
}

TError TMemorySubsystem::SetIoLimit(TCgroup &cg, uint64_t limit) {
    if (!SupportIoLimit())
        return TError::Success();
    return cg.SetUint64(FS_BPS_LIMIT, limit);
}

TError TMemorySubsystem::SetIopsLimit(TCgroup &cg, uint64_t limit) {
    if (!SupportIoLimit())
        return TError::Success();
    return cg.SetUint64(FS_IOPS_LIMIT, limit);
}

TError TMemorySubsystem::SetDirtyLimit(TCgroup &cg, uint64_t limit) {
    if (!SupportDirtyLimit())
        return TError::Success();
    if (limit || !cg.Has(DIRTY_RATIO))
        return cg.SetUint64(DIRTY_LIMIT, limit);
    return cg.SetUint64(DIRTY_RATIO, 50);
}

TError TMemorySubsystem::SetupOOMEvent(TCgroup &cg, TFile &event) {
    TError error;
    TFile knob;

    error = knob.OpenRead(cg.Knob(OOM_CONTROL));
    if (error)
        return error;

    PORTO_ASSERT(knob.Fd > 2);

    event.Close();
    event.SetFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (event.Fd < 0)
        return TError(EError::Unknown, errno, "Cannot create eventfd");

    PORTO_ASSERT(event.Fd > 2);

    error = cg.Set(EVENT_CONTROL, std::to_string(event.Fd) + " " + std::to_string(knob.Fd));
    if (error)
        event.Close();
    return error;
}

uint64_t TMemorySubsystem::GetOomEvents(TCgroup &cg) {
    TUintMap stat;
    if (!Statistics(cg, stat))
        return stat["oom_events"];
    return 0;
}

// Freezer
TError TFreezerSubsystem::WaitState(const TCgroup &cg, const std::string &state) const {
    uint64_t deadline = GetCurrentTimeMs() + config().daemon().freezer_wait_timeout_s() * 1000;
    std::string cur;
    TError error;

    do {
        error = cg.Get("freezer.state", cur);
        if (error || StringTrim(cur) == state)
            return error;
    } while (!WaitDeadline(deadline));

    return TError(EError::Unknown, "Freezer " + cg.Name + " timeout waiting " + state);
}

TError TFreezerSubsystem::Freeze(const TCgroup &cg, bool wait) const {
    TError error = cg.Set("freezer.state", "FROZEN");
    if (error || !wait)
        return error;
    error = WaitState(cg, "FROZEN");
    if (error)
        (void)cg.Set("freezer.state", "THAWED");
    return error;
}

TError TFreezerSubsystem::Thaw(const TCgroup &cg, bool wait) const {
    TError error = cg.Set("freezer.state", "THAWED");
    if (error || !wait)
        return error;
    if (IsParentFreezing(cg))
        return TError(EError::Busy, "parent cgroup is frozen");
    return WaitState(cg, "THAWED");
}

bool TFreezerSubsystem::IsFrozen(const TCgroup &cg) const {
    std::string state;
    return !cg.Get("freezer.state", state) && StringTrim(state) != "THAWED";
}

bool TFreezerSubsystem::IsSelfFreezing(const TCgroup &cg) const {
    bool val;
    return !cg.GetBool("freezer.self_freezing", val) && val;
}

bool TFreezerSubsystem::IsParentFreezing(const TCgroup &cg) const {
    bool val;
    return !cg.GetBool("freezer.parent_freezing", val) && val;
}

// Cpu
void TCpuSubsystem::InitializeSubsystem() {
    TCgroup cg = RootCgroup();

    HasShares = cg.Has("cpu.shares");
    if (HasShares && cg.GetUint64("cpu.shares", BaseShares))
        BaseShares = 1024;

    MinShares = 2; /* kernel limit MIN_SHARES */
    MaxShares = 1024 * 256; /* kernel limit MAX_SHARES */

    HasQuota = cg.Has("cpu.cfs_quota_us") &&
               cg.Has("cpu.cfs_period_us");

    HasRtGroup = cg.Has("cpu.rt_runtime_us") &&
                 cg.Has("cpu.rt_period_us");

    HasReserve = HasShares && HasQuota &&
                 cg.Has("cpu.cfs_reserve_us") &&
                 cg.Has("cpu.cfs_reserve_shares");

    if (HasQuota && cg.GetUint64("cpu.cfs_period_us", BasePeriod))
        BasePeriod = 100000;    /* 100ms */

    HasSmart = cg.Has("cpu.smart");

    L_SYS("{} cores", GetNumCores());
    if (HasShares)
        L_SYS("base shares {}", BaseShares);
    if (HasQuota)
        L_SYS("quota period {}", BasePeriod);
    if (HasRtGroup)
        L_SYS("support rt group");
    if (HasReserve)
        L_SYS("support reserves");
    if (HasSmart)
        L_SYS("support smart");
}

TError TCpuSubsystem::InitializeCgroup(TCgroup &cg) {
    if (HasRtGroup)
        (void)cg.SetInt64("cpu.rt_runtime_us", -1);
    return TError::Success();
}

TError TCpuSubsystem::SetCpuLimit(TCgroup &cg, const std::string &policy,
                                  double weight, double guarantee, double limit) {
    int max = GetNumCores();
    TError error;

    if (HasQuota) {
        int64_t quota = std::ceil(limit * BasePeriod);

        if (quota < 1000) /* 1ms */
            quota = 1000;

        if (limit <= 0 || limit >= max)
            quota = -1;

        error = cg.Set("cpu.cfs_quota_us", std::to_string(quota));
        if (error)
            return error;
    }

    if (guarantee < 0)
        guarantee = 0;

    if (guarantee > max)
        guarantee = max;

    if (HasReserve && config().container().enable_cpu_reserve()) {
        uint64_t reserve = std::floor(guarantee * BasePeriod);
        uint64_t shares = BaseShares, reserve_shares = BaseShares;

        shares *= weight;
        reserve_shares *= weight;

        if (policy == "rt") {
            shares *= 256;
            reserve = 0;
        } else if (policy == "high" || policy == "iso") {
            shares *= 16;
            reserve_shares *= 256;
        } else if (policy == "normal" || policy == "batch") {
            reserve_shares *= 16;
        } else if (policy == "idle") {
            shares /= 16;
        }

        shares = std::min(std::max(shares, MinShares), MaxShares);
        reserve_shares = std::min(std::max(reserve_shares, MinShares), MaxShares);

        error = cg.SetUint64("cpu.shares", shares);
        if (error)
            return error;

        error = cg.SetUint64("cpu.cfs_reserve_shares", reserve_shares);
        if (error)
            return error;

        error = cg.SetUint64("cpu.cfs_reserve_us", reserve);
        if (error)
            return error;

    } else if (HasShares) {
        uint64_t shares = std::floor(guarantee * BaseShares);

        /* default cpu_guarantee is 1c, shares < 1024 are broken */
        shares = std::max(shares, BaseShares);

        shares *= weight;

        if (policy == "rt")
            shares *= 256;
        else if (policy == "high" || policy == "iso")
            shares *= 16;
        else if (policy == "idle")
            shares /= 16;

        shares = std::min(std::max(shares, MinShares), MaxShares);

        error = cg.SetUint64("cpu.shares", shares);
        if (error)
            return error;
    }

    if (HasSmart) {
        error = cg.SetUint64("cpu.smart", (policy == "rt" &&
                    config().container().enable_smart()) ? 1 : 0);
        if (error)
            return error;
    }

    if (HasRtGroup) {
        int64_t root_runtime, root_period, period, runtime;

        if (RootCgroup().GetInt64("cpu.rt_period_us", root_period))
            root_period = 1000000;

        if (RootCgroup().GetInt64("cpu.rt_runtime_us", root_runtime))
            root_runtime = 950000;

        error = cg.SetInt64("cpu.rt_runtime_us", -1);
        if (error)
            return error;

        period = 100000;    /* 100ms */

        if (limit <= 0 || limit >= max ||
                limit / max * root_period > root_runtime) {
            runtime = -1;
        } else {
            runtime = limit * period / max;
            if (runtime < 1000)  /* 1ms */
                runtime = 1000;
        }

        error = cg.SetInt64("cpu.rt_period_us", period);
        if (error)
            return error;

        error = cg.SetInt64("cpu.rt_runtime_us", runtime);
        if (error)
            return error;
    }

    return TError::Success();
}

// Cpuacct
TError TCpuacctSubsystem::Usage(TCgroup &cg, uint64_t &value) const {
    std::string s;
    TError error = cg.Get("cpuacct.usage", s);
    if (error)
        return error;
    return StringToUint64(s, value);
}

TError TCpuacctSubsystem::SystemUsage(TCgroup &cg, uint64_t &value) const {
    TUintMap stat;
    TError error = cg.GetUintMap("cpuacct.stat", stat);
    if (error)
        return error;
    value = stat["system"] * (1000000000 / sysconf(_SC_CLK_TCK));
    return TError::Success();
}

// Cpuset
TError TCpusetSubsystem::SetCpus(TCgroup &cg, const std::string &cpus) const {
    std::string val;
    TError error;
    TPath copy;

    if (cpus == "")
        copy = cg.Path().DirName() / "cpuset.cpus";

    if (cpus == "all")
        copy = TPath("/sys/devices/system/cpu/present");

    if (StringStartsWith(cpus, "node ")) {
        int id;
        error = StringToInt(cpus.substr(5), id);
        if (error)
            return error;
        copy = TPath("/sys/devices/system/node/node" + std::to_string(id) + "/cpulist");
    }

    if (!copy.IsEmpty()) {
        error = copy.ReadAll(val);
        if (error)
            return error;
        val = StringTrim(val);
    } else
        val = cpus;

    return cg.Set("cpuset.cpus", val);
}

TError TCpusetSubsystem::SetMems(TCgroup &cg, const std::string &mems) const {
    std::string val;
    TError error;
    TPath copy;

    if (mems == "")
        copy = cg.Path().DirName() / "cpuset.mems";

    if (mems == "all")
        copy = TPath("/sys/devices/system/node/online");

    if (!copy.IsEmpty()) {
        error = copy.ReadAll(val);
        if (error)
            return error;
        val = StringTrim(val);
    } else
        val = mems;

    return cg.Set("cpuset.mems", val);
}

TError TCpusetSubsystem::InitializeCgroup(TCgroup &cg) {
    TError error;

    error = SetCpus(cg, "");
    if (error)
        return error;

    error = SetMems(cg, "");
    if (error)
        return error;

    return TError::Success();
}

// Netcls

// Blkio

TError TBlkioSubsystem::DiskName(const std::string &disk, std::string &name) const {
    TPath sym("/sys/dev/block/" + disk), dev;
    TError error = sym.ReadLink(dev);
    if (!error)
        name = dev.BaseName();
    return error;
}

/* converts absolule path or disk or partition name into "major:minor" */
TError TBlkioSubsystem::ResolveDisk(const std::string &key, std::string &disk) const {
    TError error;
    int tmp = 0;

    if (!sscanf(key.c_str(), "%*d:%*d%n", &tmp) && (unsigned)tmp == key.size()) {
        disk = key;
    } else {
        dev_t dev;

        if (key[0] == '/')
            dev = TPath(key).GetDev();
        else
            dev = TPath("/dev/" + key).GetBlockDev();

        if (!dev)
            return TError(EError::InvalidValue, "Disk not found: " + key);

        disk = StringFormat("%d:%d", major(dev), minor(dev));
    }

    if (!TPath("/sys/dev/block/" + disk).Exists())
        return TError(EError::InvalidValue, "Disk not found:  " + disk);

    /* convert partition to disk */
    if (TPath("/sys/dev/block/" + disk + "/partition").Exists()) {
        TPath diskDev("/sys/dev/block/" + disk + "/../dev");
        if (diskDev.IsRegularStrict() && !diskDev.ReadAll(disk)) {
            disk = StringTrim(disk);
            if (sscanf(disk.c_str(), "%*d:%*d%n", &tmp) || (unsigned)tmp != disk.size())
                return TError(EError::InvalidValue, "Unexpected disk format: " + disk);
        }
    }

    return TError::Success();
}

TError TBlkioSubsystem::GetIoStat(TCgroup &cg, TUintMap &map, int dir, bool iops) const {
    std::vector<std::string> lines;
    std::string knob, prev, name;
    TError error;

    /* get statistics from throttler if possible, it has couners for raids */
    if (HasThrottler)
        knob = iops ? "blkio.throttle.io_serviced" : "blkio.throttle.io_service_bytes";
    else
        knob = iops ? "blkio.io_serviced_recursive" : "blkio.io_service_bytes_recursive";

    error = cg.Knob(knob).ReadLines(lines);
    if (error)
        return error;

    /* in insane behavior throttler isn't hierarhical */
    if (HasThrottler && !HasSaneBehavior) {
        std::vector<TCgroup> list;

        error = cg.ChildsAll(list);
        if (error)
            return error;

        for (auto &child_cg: list) {
            error = child_cg.Knob(knob).ReadLines(lines);
            if (error)
                return error;
        }
    }

    for (auto &line: lines) {
        std::vector<std::string> word;
        if (SplitString(line, ' ', word) || word.size() != 3)
            continue;

        if (word[1] == "Read") {
            if (dir == 1)
                continue;
        } else if (word[1] == "Write") {
            if (dir == 0)
                continue;
        } else
            continue;

        if (word[0] != prev) {
            if (DiskName(word[0], name))
                continue;
            prev = word[0];
        }

        uint64_t val;
        if (!StringToUint64(word[2], val) && val)
            map[name] += val;
    }

    return TError::Success();
}

TError TBlkioSubsystem::SetIoLimit(TCgroup &cg, const TUintMap &map, bool iops) {
    std::string knob[2] = {
        iops ? "blkio.throttle.read_iops_device" : "blkio.throttle.read_bps_device",
        iops ? "blkio.throttle.write_iops_device" : "blkio.throttle.write_bps_device",
    };
    TError error, result;
    TUintMap plan[2];
    std::string disk;
    int dir;

    /* load current limits */
    for (dir = 0; dir < 2; dir++) {
        std::vector<std::string> lines;
        error = cg.Knob(knob[dir]).ReadLines(lines);
        if (error)
            return error;
        for (auto &line: lines) {
            auto sep = line.find(' ');
            if (sep != std::string::npos)
                plan[dir][line.substr(0, sep)] = 0;
        }
    }

    for (auto &it: map) {
        auto key = it.first;
        auto sep = key.rfind(' ');

        dir = 2;
        if (sep != std::string::npos) {
            if (sep != key.size() - 2 || ( key[sep+1] != 'r' && key[sep+1] != 'w'))
                return TError(EError::InvalidValue, "Invalid io limit key: " + key);
            dir = key[sep+1] == 'r' ? 0 : 1;
            key = key.substr(0, sep);
        }

        if (key == "fs")
            continue;

        error = ResolveDisk(key, disk);
        if (error)
            return error;

        if (dir == 0 || dir == 2)
            plan[0][disk] = it.second;
        if (dir == 1 || dir == 2)
            plan[1][disk] = it.second;
    }

    for (dir = 0; dir < 2; dir++) {
        for (auto &it: plan[dir]) {
            error = cg.Set(knob[dir], it.first + " " + std::to_string(it.second));
            if (error && !result)
                result = error;
        }
    }

    return result;
}

TError TBlkioSubsystem::SetIoWeight(TCgroup &cg, const std::string &policy,
                                    double weight) const {
    if (!HasWeight)
        return TError::Success();

    if (policy == "rt" || policy == "high")
        weight *= 1000;
    else if (policy == "" || policy == "none" || policy == "normal")
        weight *= 500;
    else if (policy == "batch" || policy == "idle")
        weight *= 10;
    else
        return TError(EError::InvalidValue, "unknown policy: " + policy);

    return cg.SetUint64("blkio.weight", std::min(std::max(weight, 10.), 1000.));
}

// Devices

TError TDevicesSubsystem::ApplyDefault(TCgroup &cg) {
    TError error = cg.Set("devices.deny", "a");
    if (error)
        return error;

    //FIXME 'm' required only for start
    std::vector<std::string> rules = {
        "c 1:3 rwm",     // /dev/null
        "c 1:5 rwm",     // /dev/zero
        "c 1:7 rwm",     // /dev/full
        "c 1:8 rwm",     // /dev/random
        "c 1:9 rwm",     // /dev/urandom
        "c 5:0 rwm",     // /dev/tty
        "c 5:2 rw",     // /dev/ptmx
        "c 136:* rw",   // /dev/pts/*
        "c 254:0 rm",   // /dev/rtc0         FIXME
    };

    for (auto &rule: rules) {
        error = cg.Set("devices.allow", rule);
        if (error)
            break;
    }

    return error;
}

TError TDevicesSubsystem::ApplyDevice(TCgroup &cg, const TDevice &device) {
    std::string rule;
    TError error;

    rule = device.CgroupRule(true);
    if (rule != "")
        error = cg.Set("devices.allow", rule);
    rule = device.CgroupRule(false);
    if (!error && rule != "")
        error = cg.Set("devices.deny", rule);
    return error;
}

// Pids

TError TPidsSubsystem::GetUsage(TCgroup &cg, uint64_t &usage) const {
    return cg.GetUint64("pids.current", usage);
}

TError TPidsSubsystem::SetLimit(TCgroup &cg, uint64_t limit) const {
    if (!limit)
        return cg.Set("pids.max", "max");
    return cg.SetUint64("pids.max", limit);
}


TMemorySubsystem    MemorySubsystem;
TFreezerSubsystem   FreezerSubsystem;
TCpuSubsystem       CpuSubsystem;
TCpuacctSubsystem   CpuacctSubsystem;
TCpusetSubsystem    CpusetSubsystem;
TNetclsSubsystem    NetclsSubsystem;
TBlkioSubsystem     BlkioSubsystem;
TDevicesSubsystem   DevicesSubsystem;
THugetlbSubsystem   HugetlbSubsystem;
TPidsSubsystem      PidsSubsystem;

std::vector<TSubsystem *> AllSubsystems = {
    &FreezerSubsystem,
    &MemorySubsystem,
    &CpuSubsystem,
    &CpuacctSubsystem,
    &CpusetSubsystem,
    &NetclsSubsystem,
    &BlkioSubsystem,
    &DevicesSubsystem,
    &HugetlbSubsystem,
    &PidsSubsystem,
};

std::vector<TSubsystem *> Subsystems;
std::vector<TSubsystem *> Hierarchies;


TError InitializeCgroups() {
    TPath root("/sys/fs/cgroup");
    std::list<TMount> mounts;
    TMount mount;
    TError error;

    error = root.FindMount(mount);
    if (error) {
        L_ERR("Cannot find cgroups root mount: {}", error);
        return error;
    }

    if (mount.Target != root) {
        error = root.Mount("cgroup", "tmpfs", 0, {});
        if (error) {
            L_ERR("Cannot mount cgroups root: {}", error);
            return error;
        }
    } else if (StringStartsWith(mount.Options, "ro,")) {
        error = root.Remount(MS_REMOUNT | MS_NODEV | MS_NOSUID | MS_NOEXEC);
        if (error) {
            L_ERR("Cannot remount cgroups root: {}", error);
            return error;
        }
    }

    error = TPath::ListAllMounts(mounts);
    if (error) {
        L_ERR("Can't create mount snapshot: {}", error);
        return error;
    }

    for (auto subsys: AllSubsystems) {
        for (auto &mnt: mounts) {
            if (mnt.Type == "cgroup" && mnt.HasOption(subsys->Type)) {
                subsys->Root = mnt.Target;
                L("Found cgroup subsystem {} mounted at {}", subsys->Type, subsys->Root);
                break;
            }
        }
    }

    if (config().daemon().merge_memory_blkio_controllers() &&
            MemorySubsystem.Root.IsEmpty() && BlkioSubsystem.Root.IsEmpty()) {
        TPath path = root / "memory,blkio";

        if (!path.Exists())
            (void)path.Mkdir(0755);
        error = path.Mount("cgroup", "cgroup", 0, {"memory", "blkio"});
        if (!error) {
            (root / "memory").Symlink("memory,blkio");
            (root / "blkio").Symlink("memory,blkio");
            MemorySubsystem.Root = path;
            BlkioSubsystem.Root = path;
        } else {
            L_ERR("Cannot merge memory and blkio {}", error);
            (void)path.Rmdir();
        }
    }

    for (auto subsys: AllSubsystems) {
        if (subsys->Type == "hugetlb" && !config().container().enable_hugetlb())
            continue;

        if (subsys->Root.IsEmpty()) {
            subsys->Root = root / subsys->Type;

            L("Mount cgroup subsysm {} at {}", subsys->Type, subsys->Root);
            if (!subsys->Root.Exists()) {
                error = subsys->Root.Mkdir(0755);
                if (error) {
                    L_ERR("Cannot create cgroup mountpoint: {}", error);
                    return error;
                }
            }

            error = subsys->Root.Mount("cgroup", "cgroup", 0, {subsys->Type});
            /* in kernels < 3.14 cgroup net_cls was in module cls_cgroup */
            if (error && subsys->Type == "net_cls") {
                if (system("modprobe cls_cgroup"))
                    L_ERR("Cannot load cls_cgroup");
                error = subsys->Root.Mount("cgroup", "cgroup", 0, {subsys->Type});
            }

            /* hugetlb is optional yet */
            if (error && subsys->Type == "hugetlb") {
                L("Seems not supported: {}", error);
                error = subsys->Root.Rmdir();
                continue;
            }

            if (error && subsys->Type == "cpuset") {
                L("Seems not supported: {}", error);
                error = subsys->Root.Rmdir();
                continue;
            }

            if (error && subsys->Type == "pids") {
                L("Seems not supported: {}", error);
                error = subsys->Root.Rmdir();
                continue;
            }

            if (error) {
                L_ERR("Cannot mount cgroup: {}", error);
                (void)subsys->Root.Rmdir();
                return error;
            }
        }

        Subsystems.push_back(subsys);

        subsys->Hierarchy = subsys;
        subsys->Controllers |= subsys->Kind;
        for (auto hy: Hierarchies) {
            if (subsys->Root == hy->Root) {
                L("Cgroup subsystem {} bound to hierarchy {}", subsys->Type, hy->Type);
                subsys->Hierarchy = hy;
                hy->Controllers |= subsys->Kind;
                break;
            }
        }
        if (subsys->Hierarchy == subsys)
            Hierarchies.push_back(subsys);

        subsys->InitializeSubsystem();
    }

    for (auto subsys: AllSubsystems)
        if (subsys->Hierarchy)
            subsys->Controllers |= subsys->Hierarchy->Controllers;

    return error;
}

TError InitializeDaemonCgroups() {
    std::vector<TSubsystem *> DaemonSubsystems = {
        &MemorySubsystem,
        &CpuacctSubsystem,
    };

    for (auto subsys : DaemonSubsystems) {
        auto hy = subsys->Hierarchy;
        TError error;

        if (!hy)
            continue;

        TCgroup cg = hy->Cgroup(PORTO_DAEMON_CGROUP);
        if (!cg.Exists()) {
            error = cg.Create();
            if (error)
                return error;
        }

        // portod-slave
        error = cg.Attach(GetPid());
        if (error)
            return error;

        // portod master
        error = cg.Attach(GetPPid());
        if (error)
            return error;
    }

    TCgroup cg = MemorySubsystem.Cgroup(PORTO_DAEMON_CGROUP);
    TError error = MemorySubsystem.SetLimit(cg, config().daemon().memory_limit());
    if (error)
        return error;

    cg = MemorySubsystem.Cgroup(PORTO_HELPERS_CGROUP);
    if (!cg.Exists()) {
        error = cg.Create();
        if (error)
            return error;
    }

    error = MemorySubsystem.SetLimit(cg, config().daemon().helpers_memory_limit());
    if (error)
        return error;

    error = MemorySubsystem.SetDirtyLimit(cg, config().daemon().helpers_dirty_limit());
    if (error)
        L_ERR("Cannot set portod-helpers dirty limit: {}", error);

    cg = FreezerSubsystem.Cgroup(PORTO_CGROUP_PREFIX);
    if (!cg.Exists()) {
        error = cg.Create();
        if (error)
            return error;
    }

    return TError::Success();
}
