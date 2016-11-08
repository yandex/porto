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

TError TCgroup::Create() const {
    TError error;

    if (Secondary())
        return TError(EError::Unknown, "Cannot create secondary cgroup " + Type());

    L_ACT() << "Create cgroup " << *this << std::endl;
    error = Path().Mkdir(0755);
    if (error)
        L_ERR() << "Cannot create cgroup " << *this << " : " << error << std::endl;

    return error;
}

TError TCgroup::Remove() const {
    struct stat st;
    TError error;

    if (Secondary())
        return TError(EError::Unknown, "Cannot create secondary cgroup " + Type());

    L_ACT() << "Remove cgroup " << *this << std::endl;
    error = Path().Rmdir();

    /* workaround for bad synchronization */
    if (error && error.GetErrno() == EBUSY &&
            !Path().StatStrict(st) && st.st_nlink == 2) {
        uint64_t deadline = GetCurrentTimeMs() + config().daemon().cgroup_remove_timeout_s() * 1000;
        do {
            error = Path().Rmdir();
            if (!error || error.GetErrno() != EBUSY)
                break;
        } while (!WaitDeadline(deadline));
    }

    if (error && (error.GetErrno() != ENOENT || Exists()))
        L_ERR() << "Cannot remove cgroup " << *this << " : " << error << std::endl;

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
    L_ACT() << "Set " << *this << " " << knob << " = " << value << std::endl;
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

    L_ACT() << "Attach process " << pid << " to " << *this << std::endl;
    TError error = Knob("cgroup.procs").WriteAll(std::to_string(pid));
    if (error)
        L_ERR() << "Cannot attach process " << pid << " to " << *this << " : " << error << std::endl;

    return error;
}

TError TCgroup::AttachAll(const TCgroup &cg) const {
    if (Secondary())
        return TError(EError::Unknown, "Cannot attach to secondary cgroup " + Type());

    L_ACT() << "Attach all processes from " << cg << " to " << *this << std::endl;

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
                L_ERR() << "Cannot dump childs of " << cgroup << " : "
                        << error << std::endl;
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

bool TCgroup::IsEmpty() const {
    std::vector<pid_t> tasks;

    GetTasks(tasks);
    return tasks.empty();
}

TError TCgroup::KillAll(int signal) const {
    std::vector<pid_t> tasks;
    TError error;

    L_ACT() << "KillAll " << signal << " " << *this << std::endl;

    if (IsRoot())
        return TError(EError::Permission, "Bad idea");

    error = GetTasks(tasks);
    if (!error) {
        for (const auto &pid : tasks) {
            if (kill(pid, signal) && errno != ESRCH) {
                error = TError(EError::Unknown, errno, StringFormat("kill(%d, %d)", pid, signal));
                L_ERR() << "Cannot kill process " << pid << " : " << error << std::endl;
            }
        }
    }

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
    FILE *file = fopen(("/proc/" + std::to_string(pid) + "/cgroup").c_str(), "r");

    if (file) {
        int id;

        while (fscanf(file, "%d:", &id) == 1) {
            bool found = false;
            char *ss, *cg;

            while (fscanf(file, "%m[^:,],", &ss) == 1) {
                if (std::string(ss) == Type)
                    found = true;
                free(ss);
            }

            if (fscanf(file, ":%ms\n", &cg) == 1) {
                if (found) {
                    cgroup.Subsystem = this;
                    cgroup.Name = std::string(cg);
                    free(cg);
                    fclose(file);
                    return TError::Success();
                }
                free(cg);
            }
        }
        fclose(file);
    }

    return TError(EError::Unknown, errno, "Cannot find " + Type +
                    " cgroup for process " + std::to_string(pid));
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
    if (limit)
        return cg.SetUint64(DIRTY_LIMIT, limit);
    return cg.SetUint64(DIRTY_RATIO, 50);
}

TError TMemorySubsystem::SetupOOMEvent(TCgroup &cg, TFile &event) {
    TError error;
    TFile knob;

    error = knob.OpenRead(cg.Knob(OOM_CONTROL));
    if (error)
        return error;

    event.Close();
    event.SetFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (event.Fd < 0)
        return TError(EError::Unknown, errno, "Cannot create eventfd");

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
TError TFreezerSubsystem::WaitState(TCgroup &cg, const std::string &state) const {
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

TError TFreezerSubsystem::Freeze(TCgroup &cg) const {
    TError error = cg.Set("freezer.state", "FROZEN");
    if (error)
        return error;
    error = WaitState(cg, "FROZEN");
    if (error)
        (void)cg.Set("freezer.state", "THAWED");
    return error;
}

TError TFreezerSubsystem::Thaw(TCgroup &cg, bool wait) const {
    TError error = cg.Set("freezer.state", "THAWED");
    if (error || !wait)
        return error;
    if (IsParentFreezing(cg))
        return TError(EError::Busy, "parent cgroup is frozen");
    return WaitState(cg, "THAWED");
}

bool TFreezerSubsystem::IsFrozen(TCgroup &cg) const {
    std::string state;
    return !cg.Get("freezer.state", state) && StringTrim(state) != "THAWED";
}

bool TFreezerSubsystem::IsSelfFreezing(TCgroup &cg) const {
    bool val;
    return !cg.GetBool("freezer.self_freezing", val) && val;
}

bool TFreezerSubsystem::IsParentFreezing(TCgroup &cg) const {
    bool val;
    return !cg.GetBool("freezer.parent_freezing", val) && val;
}

// Cpu
void TCpuSubsystem::InitializeSubsystem() {
    TCgroup cg = RootCgroup();

    HasShares = cg.Has("cpu.shares");
    if (HasShares && cg.GetUint64("cpu.shares", BaseShares))
        BaseShares = 1024;

    HasQuota = cg.Has("cpu.cfs_quota_us") &&
               cg.Has("cpu.cfs_period_us");

    HasReserve = HasShares && HasQuota &&
                 cg.Has("cpu.cfs_reserve_us") &&
                 cg.Has("cpu.cfs_reserve_shares");

    if (HasQuota && cg.GetUint64("cpu.cfs_period_us", BasePeriod))
        BasePeriod = 100000;

    HasSmart = cg.Has("cpu.smart");

    L_SYS() << GetNumCores() << " cores" << std::endl;
    if (HasShares)
        L_SYS() << "base shares " << BaseShares << std::endl;
    if (HasQuota)
        L_SYS() << "quota period " << BasePeriod << std::endl;
    if (HasReserve)
        L_SYS() << "support reserves" << std::endl;
    if (HasSmart)
        L_SYS() << "support smart" << std::endl;
}

TError TCpuSubsystem::SetCpuPolicy(TCgroup &cg, const std::string &policy,
                                   double guarantee, double limit) {
    TError error;

    if (HasQuota) {
        int64_t quota = std::ceil(limit * BasePeriod);

        if (quota < 1000)
            quota = 1000;

        if (limit >= GetNumCores())
            quota = -1;

        error = cg.Set("cpu.cfs_quota_us", std::to_string(quota));
        if (error)
            return error;
    }

    if (HasReserve && config().container().enable_cpu_reserve()) {
        uint64_t reserve = std::floor(guarantee * BasePeriod);
        uint64_t shares = BaseShares, reserve_shares = BaseShares;

        if (policy == "rt") {
            shares *= 256;
            reserve = 0;
        } else if (policy == "high") {
            shares *= 16;
            reserve_shares *= 256;
        } else if (policy == "normal" || policy == "batch") {
            reserve_shares *= 16;
        } else if (policy == "idle") {
            shares /= 16;
        }

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
        uint64_t shares = std::floor((guarantee + 1) * BaseShares);

        if (policy == "rt")
            shares *= 256;
        else if (policy == "high")
            shares *= 16;
        else if (policy == "idle")
            shares /= 16;

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

    int sched_policy = SCHED_OTHER;
    struct sched_param sched_param;
    sched_param.sched_priority = 0;
    int sched_nice = 0;

    if (policy == "idle")
        sched_policy = SCHED_IDLE;

    if (policy == "batch")
        sched_policy = SCHED_BATCH;

    if (policy == "rt") {
        if (HasSmart && config().container().enable_smart()) {
            sched_policy = -1;
        } else if (config().container().rt_priority()) {
            sched_policy = SCHED_RR;
            sched_param.sched_priority = config().container().rt_priority();
        }
        /* just to show in top */
        sched_nice = config().container().rt_nice();
    }

    if (policy == "high")
        sched_nice = config().container().high_nice();

    if (sched_policy >= 0) {
        std::vector<pid_t> prev, pids;
        bool retry;

        L_ACT() << "Set " << cg << " sched policy " << sched_policy << std::endl;
        do {
            error = cg.GetTasks(pids);
            retry = false;
            for (auto pid: pids) {
                if (std::find(prev.begin(), prev.end(), pid) != prev.end() &&
                        sched_getscheduler(pid) == sched_policy)
                    continue;
                if (setpriority(PRIO_PROCESS, pid, sched_nice) && errno != ESRCH)
                    return TError(EError::Unknown, errno, "setpriority");
                if (sched_setscheduler(pid, sched_policy, &sched_param) &&
                        errno != ESRCH)
                    return TError(EError::Unknown, errno, "sched_setscheduler");
                retry = true;
            }
            prev = pids;
        } while (retry);
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

// Netcls

// Blkio

TError TBlkioSubsystem::GetStatLine(const std::vector<std::string> &lines,
                                    const size_t i,
                                    const std::string &name,
                                    uint64_t &val) const {
    std::vector<std::string> tokens;
    TError error = SplitString(lines[i], ' ', tokens);
    if (error)
        return error;

    if (tokens.size() < 3 || tokens[1] != name)
        return TError(EError::Unknown, "Unexpected field in blkio statistics");

    return StringToUint64(tokens[2], val);
}

TError TBlkioSubsystem::GetDevice(const std::string &majmin,
                                  std::string &device) const {
    TPath sym("/sys/dev/block/" + majmin), dev;
    TError error = sym.ReadLink(dev);
    if (!error)
        device = dev.BaseName();
    return error;
}

TError TBlkioSubsystem::Statistics(TCgroup &cg,
                                   const std::string &file,
                                   std::vector<BlkioStat> &stat) const {
    std::vector<std::string> lines;
    TError error = cg.Knob(file).ReadLines(lines);
    if (error)
        return error;

    BlkioStat s;
    for (size_t i = 0; i < lines.size(); i += 5) {
        std::vector<std::string> tokens;
        error = SplitString(lines[i], ' ', tokens);
        if (error)
            return error;

        if (tokens.size() == 3) {
            error = GetDevice(tokens[0], s.Device);
            if (error)
                return error;
        } else {
            continue; /* Total */
        }

        error = GetStatLine(lines, i + 0, "Read", s.Read);
        if (error)
            return error;
        error = GetStatLine(lines, i + 1, "Write", s.Write);
        if (error)
            return error;
        error = GetStatLine(lines, i + 2, "Sync", s.Sync);
        if (error)
            return error;
        error = GetStatLine(lines, i + 3, "Async", s.Async);
        if (error)
            return error;

        stat.push_back(s);
    }

    return TError::Success();
}

TError TBlkioSubsystem::SetIoPolicy(TCgroup &cg, const std::string &policy) const {
    if (!SupportIoPolicy())
        return TError::Success();

    uint64_t weight;
    if (policy == "normal")
        weight = config().container().normal_io_weight();
    else if (policy == "batch")
        weight = config().container().batch_io_weight();
    else
        return TError(EError::InvalidValue, "unknown policy: " + policy);

    return cg.SetUint64("blkio.weight", weight);
}

bool TBlkioSubsystem::SupportIoPolicy() const {
    return RootCgroup().Has("blkio.weight");
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

TMemorySubsystem    MemorySubsystem;
TFreezerSubsystem   FreezerSubsystem;
TCpuSubsystem       CpuSubsystem;
TCpuacctSubsystem   CpuacctSubsystem;
TCpusetSubsystem    CpusetSubsystem;
TNetclsSubsystem    NetclsSubsystem;
TBlkioSubsystem     BlkioSubsystem;
TDevicesSubsystem   DevicesSubsystem;
THugetlbSubsystem   HugetlbSubsystem;

std::vector<TSubsystem *> AllSubsystems = {
    { &FreezerSubsystem  },
    { &MemorySubsystem   },
    { &CpuSubsystem      },
    { &CpuacctSubsystem  },
    { &CpusetSubsystem   },
    { &NetclsSubsystem   },
    { &BlkioSubsystem    },
    { &DevicesSubsystem  },
    { &HugetlbSubsystem  },
};

std::vector<TSubsystem *> Subsystems;
std::vector<TSubsystem *> Hierarchies;


TError InitializeCgroups() {
    TPath root(config().daemon().sysfs_root());
    std::list<TMount> mounts;
    TMount mount;
    TError error;

    error = root.FindMount(mount);
    if (error) {
        L_ERR() << "Cannot find cgroups root mount: " << error << std::endl;
        return error;
    }

    if (mount.Target != root) {
        error = root.Mount("cgroup", "tmpfs", 0, {});
        if (error) {
            L_ERR() << "Cannot mount cgroups root: " << error << std::endl;
            return error;
        }
    }

    error = TPath::ListAllMounts(mounts);
    if (error) {
        L_ERR() << "Can't create mount snapshot: " << error << std::endl;
        return error;
    }

    for (auto subsys: AllSubsystems) {
        for (auto &mnt: mounts) {
            if (mnt.Type == "cgroup" && mnt.HasOption(subsys->Type)) {
                subsys->Root = mnt.Target;
                L() << "Found cgroup subsystem " << subsys->Type << " mounted at " << subsys->Root << std::endl;
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
            L_ERR() << "Cannot merge memory and blkio " << error << std::endl;
            (void)path.Rmdir();
        }
    }

    for (auto subsys: AllSubsystems) {
        if (subsys->Type == "hugetlb" && !config().container().enable_hugetlb())
            continue;

        if (subsys->Root.IsEmpty()) {
            subsys->Root = root / subsys->Type;

            L() << "Mount cgroup subsysm " << subsys->Type << " at " << subsys->Root << std::endl;
            if (!subsys->Root.Exists()) {
                error = subsys->Root.Mkdir(0755);
                if (error) {
                    L_ERR() << "Cannot create cgroup mountpoint: " << error << std::endl;
                    return error;
                }
            }

            error = subsys->Root.Mount("cgroup", "cgroup", 0, {subsys->Type});
            /* in kernels < 3.14 cgroup net_cls was in module cls_cgroup */
            if (error && subsys->Type == "net_cls") {
                if (system("modprobe cls_cgroup"))
                    L_ERR() << "Cannot load cls_cgroup" << std::endl;
                error = subsys->Root.Mount("cgroup", "cgroup", 0, {subsys->Type});
            }

            /* hugetlb is optional yet */
            if (error && subsys->Type == "hugetlb") {
                L() << "Seems not supported: " << error << std::endl;
                error = subsys->Root.Rmdir();
                continue;
            }

            if (error && subsys->Type == "cpuset") {
                L() << "Seems not supported: " << error << std::endl;
                error = subsys->Root.Rmdir();
                continue;
            }

            if (error) {
                L_ERR() << "Cannot mount cgroup: " << error << std::endl;
                (void)subsys->Root.Rmdir();
                return error;
            }
        }

        Subsystems.push_back(subsys);

        subsys->Hierarchy = subsys;
        subsys->Controllers |= subsys->Kind;
        for (auto hy: Hierarchies) {
            if (subsys->Root == hy->Root) {
                L() << "Cgroup subsystem " << subsys->Type
                    << " bound to hierarchy " << hy->Type << std::endl;
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
        { &MemorySubsystem   },
        { &CpuacctSubsystem  },
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

    cg = FreezerSubsystem.Cgroup(PORTO_CGROUP_PREFIX);
    if (!cg.Exists()) {
        error = cg.Create();
        if (error)
            return error;
    }

    return TError::Success();
}
