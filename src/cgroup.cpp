#include <algorithm>
#include <cmath>
#include <csignal>
#include <unordered_set>
#include <mutex>

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
#include <sys/sysmacros.h>
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
    { CGROUP_PERF,      "perf_event" },
    { CGROUP_SYSTEMD,   "systemd" },
    { CGROUP2,          "cgroup2" },
};

bool TCgroup::IsRestore = false;

static std::map<std::string, std::vector<pid_t>> prevAttachedPidsMap;

extern pid_t MasterPid;
extern pid_t PortodPid;
extern std::unordered_set<pid_t> PortoTids;
extern std::mutex TidsMutex;

extern bool EnableCgroupNs;

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
        return TError("Cannot create secondary cgroup " + Type());

    L_CG("Create cgroup {}", *this);
    error = Path().Mkdir(0755);
    if (error)
        L_ERR("Cannot create cgroup {} : {}", *this, error);

    for (auto subsys: Subsystems) {
        if (subsys->IsBound(*this)) {
            error = subsys->InitializeCgroup(*this);
            if (error)
                return error;
        }
    }

    return error;
}

TError TCgroup::Rename(TCgroup &target) {
    TError error;

    if (Secondary())
        return TError("Cannot rename secondary cgroup " + Type());

    L_CG("Rename cgroup {} to {}", *this, target);

    if (target.Exists())
        return TError("Cannot rename to existing cgroup " + target.Name);

    if (target.Subsystem != Subsystem)
        return TError("Cannot rename to other subsystem " + target.Type());

    error = Path().Rename(target.Path());
    if (!error)
        Name = target.Name;

    return error;
}

TError TCgroup::Remove() {
    if ((Subsystem->Kind & CGROUP_SYSTEMD) || EnableCgroupNs) {
        std::vector<TCgroup> children;

        TError error = ChildsAll(children);
        if (error)
            return error;

        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            error = (*it).RemoveOne();
            if (error)
                return error;
        }
    }

    return RemoveOne();
}

TError TCgroup::RemoveOne() {
    struct stat st;
    TError error;

    if (Secondary())
        return TError("Cannot create secondary cgroup " + Type());

    L_CG("Remove cgroup {}", *this);
    error = Path().Rmdir();

    std::vector<pid_t> startTasks;
    GetTasks(startTasks);
    /* workaround for bad synchronization */
    if (error && error.Errno == EBUSY && !Path().StatStrict(st) && st.st_nlink == 2) {
        uint64_t deadline = GetCurrentTimeMs() + config().daemon().cgroup_remove_timeout_s() * 1000;
        uint64_t interval = 1;
        do {
            (void)KillAll(SIGKILL);
            error = Path().Rmdir();
            if (!error || error.Errno != EBUSY)
                break;
            if (interval < 1000)
                interval *= 10;
        } while (!WaitDeadline(deadline, interval));
    }

    if (error && (error.Errno != ENOENT || Exists())) {
        std::vector<pid_t> tasks;
        GetTasks(tasks);
        L_CG_ERR("Cannot remove cgroup {} : {}, {} tasks inside",
              *this, error, tasks.size());

        L("Tasks before destroy:");
        for (auto task : startTasks)
            L("task: {}", task);

        L("Tasks after destroy:");
        for (size_t i = 0;
             i < tasks.size() && i < config().daemon().debug_hung_tasks_count();
             ++i) {
            auto task = tasks[i];
            PrintProc("status", task);
            PrintProc("wchan", task);
            PrintStack(task);
        }
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
        return TError("Cannot get from null cgroup");
    return Knob(knob).ReadAll(value);
}

TError TCgroup::Set(const std::string &knob, const std::string &value) const {
    if (!Subsystem)
        return TError("Cannot set to null cgroup");
    L_CG("Set {} {} = {}", *this, knob, value);
    TError error = Knob(knob).WriteAll(value);
    if (error)
        error = TError(error, "Cannot set cgroup {} = {}", knob, value);
    return error;
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
        return TError("Cannot get from null cgroup");

    FILE *file = fopen(Knob(knob).c_str(), "r");
    char *key;
    unsigned long long val;

    if (!file)
        return TError::System("Cannot open knob " + knob);

    while (fscanf(file, "%ms %llu\n", &key, &val) == 2) {
        value[std::string(key)] = val;
        free(key);
    }

    fclose(file);
    return OK;
}

TError TCgroup::Attach(pid_t pid, bool thread) const {
    if (IsNetcls() && !config().network().enable_host_net_classes())
        return OK;

    if (Secondary())
        return TError("Cannot attach to secondary cgroup " + Type());

    if (IsCgroup2() && thread)
        return OK;

    L_CG("Attach {} {} to {}", thread ? "thread" : "process", pid, *this);
    TError error = Knob(thread ? "tasks" : "cgroup.procs").WriteAll(std::to_string(pid));
    if (error)
        L_ERR("Cannot attach {} {} to {} : {}", thread ? "thread" : "process", pid, *this, error);

    return error;
}

TError TCgroup::AttachAll(const TCgroup &cg) const {
    if (IsNetcls() && !config().network().enable_host_net_classes())
        return OK;

    if (Secondary())
        return TError("Cannot attach to secondary cgroup " + Type());

    L_CG("Attach all processes from {} to {}", cg, *this);

    std::vector<pid_t> pids, prev;
    std::vector<pid_t>& prevAttachPids = prevAttachedPidsMap[this->Type()];

    bool retry;
    TError error = cg.GetProcesses(pids);
    if (error)
        return error;

    bool thread = IsRestore && std::find_first_of(pids.begin(), pids.end(), prevAttachPids.begin(), prevAttachPids.end()) != pids.end();

    if (thread && IsCgroup2())
        return OK;

    do {
        error = thread ? cg.GetTasks(pids) : cg.GetProcesses(pids);
        if (error)
            return error;

        retry = false;
        for (auto pid: pids) {
            error = Knob(thread ? "tasks" : "cgroup.procs").WriteAll(std::to_string(pid));
            if (error && error.Errno != ESRCH)
                return error;
            retry = retry || std::find(prev.begin(), prev.end(), pid) == prev.end();
        }
        prev = pids;
    } while (retry);

    if (IsRestore) {
        error = GetProcesses(prevAttachPids);
        if (error)
            return error;
    }

    return OK;
}

TCgroup TCgroup::Child(const std::string& name) const {
    PORTO_ASSERT(name[0] != '/');
    if (IsRoot())
        return TCgroup(Subsystem, "/" + name);
    return TCgroup(Subsystem, Name + "/" + name);
}

TError TCgroup::ChildsAll(std::vector<TCgroup> &cgroups) const {
    TPathWalk walk;
    TError error;

    cgroups.clear();

    error = walk.OpenList(Path());
    if (error)
        return TError(error, "Cannot get childs for {}", *this);

    while (1) {
        error = walk.Next();
        if (error)
            return TError(error, "Cannot get childs for {}", *this);

        if (!walk.Path)
            break;

        if (!S_ISDIR(walk.Stat->st_mode) || walk.Postorder || walk.Level() == 0)
            continue;

        std::string name = Subsystem->Root.InnerPath(walk.Path).ToString();

        /* Ignore non-proto cgroups */
        if (!StringStartsWith(name, PORTO_CGROUP_PREFIX) && !EnableCgroupNs)
            continue;

        cgroups.push_back(TCgroup(Subsystem,  name));
    }

    return OK;
}

TError TCgroup::GetPids(const std::string &knob, std::vector<pid_t> &pids) const {
    FILE *file;
    int pid;

    if (!Subsystem)
        return TError("Cannot get from null cgroup");

    pids.clear();
    file = fopen(Knob(knob).c_str(), "r");
    if (!file)
        return TError::System("Cannot open knob " + knob);
    while (fscanf(file, "%d", &pid) == 1)
        pids.push_back(pid);
    fclose(file);

    return OK;
}

TError TCgroup::GetCount(bool threads, uint64_t &count) const {
    std::vector<TCgroup> childs;
    TError error;

    if (!Subsystem)
        TError("Cannot get from null cgroup");
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

void TCgroup::StartRestore() {
    IsRestore = true;
}

void TCgroup::FinishRestore() {
    prevAttachedPidsMap.clear();
    IsRestore = false;
}
TError TCgroup::KillAll(int signal) const {
    std::vector<pid_t> tasks, killed;
    TError error, error2;
    bool retry;
    bool frozen = false;
    int iteration = 0;

    L_CG("KillAll {} {}", signal, *this);

    if (IsRoot())
        return TError(EError::Permission, "Bad idea");

    do {
        if (++iteration > 10 && !frozen && FreezerSubsystem.IsBound(*this) &&
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
            std::unique_lock<std::mutex> lock(TidsMutex);
            bool portoThread = PortoTids.find(pid) != PortoTids.end();
            lock.unlock();

            if (portoThread || pid == MasterPid || pid == PortodPid || pid <= 0) {
                L_TAINT(fmt::format("Cannot kill portod thread {}", pid));
                continue;
            }
            if (std::find(killed.begin(), killed.end(), pid) == killed.end()) {
                if (kill(pid, signal) && errno != ESRCH && !error) {
                    error = TError::System("kill");
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

/* Note that knobs will stay in default values, thus use is limited */
TError TCgroup::Recreate() {
    TError error;
    TCgroup tmpcg = Subsystem->Cgroup(Name + "_tmp");

    if (!Exists())
        return Create();

    if (tmpcg.Exists()) {
        (void)AttachAll(tmpcg);
        (void)tmpcg.KillAll(SIGKILL);
        (void)tmpcg.Remove();
    }

    L_CG("Recreate cgroup {}", *this);
    error = tmpcg.Create();
    if (error)
        return error;

    error = tmpcg.AttachAll(*this);
    if (error)
        goto cleanup;

    error = Remove();
    if (error)
        goto cleanup;

    // renaming is not allowed for cgroup2 in kernel
    if (!IsCgroup2()) {
        error = tmpcg.Rename(*this);
        if (!error)
            return OK;

        L_CG_ERR("Cannot recreate cgroup {} by rename, fallback to reattaching pids", *this);
    }

    error = Create();
    if (error)
        return error;

cleanup:
    (void)AttachAll(tmpcg);
    (void)tmpcg.KillAll(SIGKILL);
    (void)tmpcg.Remove();

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
    auto type = TestOption();
    std::unordered_set<std::string> cgroupTypes;

    TError error = cg_file.ReadLines(lines);
    if (error)
        return error;

    bool found = false;

    for (auto &line : lines) {
        auto fields = SplitString(line, ':', 3);
        if (fields.size() < 3)
            continue;

        auto cgroups = SplitString(fields[1], ',');
        if (Kind == CGROUP2 && cgroups.empty())
            cgroups.push_back("");

        for (auto &cg : cgroups) {
            // KERNEL-651
            // check that we do not have fake cgroup created by cgroup with \n in name
            if (cgroupTypes.find(cg) != cgroupTypes.end())
                return TError(EError::Permission, "Fake cgroup found");
            cgroupTypes.insert(cg);

            if (!found && cg == type) {
                found = true;
                cgroup.Subsystem = this;
                cgroup.Name = fields[2];
            }
        }
    }

    return found ? OK : TError("Cannot find {} cgroup for process {}", Type, pid);
}

bool TSubsystem::IsBound(const TCgroup &cgroup) const {
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
        return OK;

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
             (!error || error.Errno == EBUSY));

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

TError TMemorySubsystem::GetShmemUsage(TCgroup &cg, uint64_t &usage) const {
    TUintMap stat;
    TError error = Statistics(cg, stat);
    if (error)
        return error;

    if (stat.count("total_shmem")) {
        usage = stat["total_shmem"];
    } else {
        if (cg.Has(ANON_USAGE))
            cg.GetUint64(ANON_USAGE, usage);
        else
            usage = stat["total_inactive_anon"] +
                    stat["total_active_anon"] +
                    stat["total_unevictable"];

        if (usage >= stat["total_rss"])
            usage -= stat["total_rss"];
        else
            usage = 0;
    }

    return error;
}

TError TMemorySubsystem::GetMLockUsage(TCgroup &cg, uint64_t &usage) const {
    TUintMap stat;
    TError error = Statistics(cg, stat);
    if (!error)
        usage = stat["total_unevictable"];
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
    return OK;
}

bool TMemorySubsystem::SupportAnonOnly() const {
    return Cgroup(PORTO_DAEMON_CGROUP).Has(ANON_ONLY);
}

TError TMemorySubsystem::SetAnonOnly(TCgroup &cg, bool val) const {
    if (cg.Has(ANON_ONLY))
        return cg.SetBool(ANON_ONLY, val);
    return OK;
}

TError TMemorySubsystem::SetIoLimit(TCgroup &cg, uint64_t limit) {
    if (!SupportIoLimit())
        return OK;
    return cg.SetUint64(FS_BPS_LIMIT, limit);
}

TError TMemorySubsystem::SetIopsLimit(TCgroup &cg, uint64_t limit) {
    if (!SupportIoLimit())
        return OK;
    return cg.SetUint64(FS_IOPS_LIMIT, limit);
}

TError TMemorySubsystem::SetDirtyLimit(TCgroup &cg, uint64_t limit) {
    if (!SupportDirtyLimit())
        return OK;
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
        return TError::System("Cannot create eventfd");

    PORTO_ASSERT(event.Fd > 2);

    error = cg.Set(EVENT_CONTROL, std::to_string(event.Fd) + " " + std::to_string(knob.Fd));
    if (error)
        event.Close();
    return error;
}

TError TMemorySubsystem::GetOomKills(TCgroup &cg, uint64_t &count) {
    TUintMap map;
    TError error = cg.GetUintMap(OOM_CONTROL, map);
    if (error)
        return error;
    if (!map.count("oom_kill"))
        return TError(EError::NotSupported, "no oom kill counter");
    count = map.at("oom_kill");
    return OK;
}

uint64_t TMemorySubsystem::GetOomEvents(TCgroup &cg) {
    TUintMap stat;
    if (!Statistics(cg, stat))
        return stat["oom_events"];
    return 0;
}

TError TMemorySubsystem::GetReclaimed(TCgroup &cg, uint64_t &count) const {
    TUintMap stat;
    Statistics(cg, stat);
    count = stat["total_pgpgout"] * 4096; /* Best estimation for now */
    return OK;
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

    return TError("Freezer {} timeout waiting {}", cg.Name, state);
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
TError TCpuSubsystem::InitializeSubsystem() {
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

    L_SYS("{} cores", GetNumCores());
    if (HasShares)
        L_CG("support shares {}", BaseShares);
    if (HasRtGroup)
        L_CG("support rt group");
    if (HasReserve)
        L_CG("support reserves");

    return OK;
}

TError TCpuSubsystem::InitializeCgroup(TCgroup &cg) {
    if (HasRtGroup && config().container().rt_priority())
        (void)cg.SetInt64("cpu.rt_runtime_us", -1);
    return OK;
}


TError TCpuSubsystem::SetPeriod(TCgroup &cg, uint64_t period) {
    TError error;

    period = period / 1000; /* ns -> us */

    if (HasQuota) {
        if (period < 1000) /* 1ms */
            period = 1000;

        if (period > 1000000) /* 1s */
            period = 1000000;

        return cg.Set("cpu.cfs_period_us", std::to_string(period));
    }

    return OK;
}

TError TCpuSubsystem::SetLimit(TCgroup &cg, uint64_t period, uint64_t limit) {
    TError error;

    period = period / 1000; /* ns -> us */

    if (HasQuota) {
        int64_t quota = std::ceil((double)limit * period / CPU_POWER_PER_SEC);

        if (quota < 1000) /* 1ms */
            quota = 1000;

        if (!limit)
            quota = -1;

        (void)cg.Set("cpu.cfs_quota_us", std::to_string(quota));

        error = cg.Set("cpu.cfs_period_us", std::to_string(period));
        if (error)
            return error;

        error = cg.Set("cpu.cfs_quota_us", std::to_string(quota));
        if (error)
            return error;
    }
    return OK;
}

TError TCpuSubsystem::SetGuarantee(TCgroup &cg, const std::string &policy,
        double weight, uint64_t period, uint64_t guarantee) {
    TError error;

    period = period / 1000; /* ns -> us */

    if (HasReserve && config().container().enable_cpu_reserve()) {
        uint64_t reserve = std::floor((double)guarantee * period / CPU_POWER_PER_SEC);
        uint64_t shares = BaseShares, reserve_shares = BaseShares;

        shares *= weight;
        reserve_shares *= weight;

        if (policy == "rt" || policy == "high" || policy == "iso") {
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
        uint64_t shares = std::floor((double)guarantee * BaseShares / CPU_POWER_PER_SEC);

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

    return OK;
}

TError TCpuSubsystem::SetRtLimit(TCgroup &cg, uint64_t period, uint64_t limit) {
    TError error;

    period = period / 1000; /* ns -> us */

    if (HasRtGroup && config().container().rt_priority()) {
        uint64_t max = GetNumCores() * CPU_POWER_PER_SEC;
        int64_t root_runtime, root_period, runtime;

        if (RootCgroup().GetInt64("cpu.rt_period_us", root_period))
            root_period = 1000000;

        if (RootCgroup().GetInt64("cpu.rt_runtime_us", root_runtime))
            root_runtime = 950000;
        else if (root_runtime < 0)
            root_runtime = root_period;

        if (limit <= 0 || limit >= max ||
                (double)limit / max * root_period > root_runtime) {
            runtime = -1;
        } else {
            runtime = (double)limit * period / max;
            if (runtime < 1000)  /* 1ms */
                runtime = 1000;
        }

        error = cg.SetInt64("cpu.rt_period_us", period);
        if (error) {
            (void)cg.SetInt64("cpu.rt_runtime_us", runtime);
            error = cg.SetInt64("cpu.rt_period_us", period);
        }
        if (!error)
            error = cg.SetInt64("cpu.rt_runtime_us", runtime);
        if (error) {
            (void)cg.SetInt64("cpu.rt_runtime_us", 0);
            return error;
        }
    }
    return OK;
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
    return OK;
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

    return OK;
}

// Netcls

TError TNetclsSubsystem::InitializeSubsystem() {
    HasPriority = config().network().enable_netcls_priority() &&
                  RootCgroup().Has("net_cls.priority");
    if (HasPriority)
        L_CG("support netcls priority");
    return OK;
}

TError TNetclsSubsystem::SetClass(TCgroup &cg, uint32_t classid) const {
    TError error;
    uint64_t cur;

    if (!config().network().enable_host_net_classes())
        return OK;

    if (HasPriority) {
        error = cg.GetUint64("net_cls.priority", cur);
        if (error || cur != classid) {
            error = cg.SetUint64("net_cls.priority", classid);
            if (error)
                return error;
        }
    }

    error = cg.GetUint64("net_cls.classid", cur);
    if (error || cur != classid) {
        error = cg.SetUint64("net_cls.classid", classid);
        if (error)
            return error;
    }

    return OK;
}

// Blkio

TError TBlkioSubsystem::DiskName(const std::string &disk, std::string &name) const {
    TPath sym("/sys/dev/block/" + disk), dev;
    TError error = sym.ReadLink(dev);
    if (!error)
        name = dev.BaseName();
    return error;
}

/* converts absolule path or disk or partition name into "major:minor" */
TError TBlkioSubsystem::ResolveDisk(const TPath &root, const std::string &key, std::string &disk) const {
    TError error;
    int tmp = 0;

    if (!sscanf(key.c_str(), "%*d:%*d%n", &tmp) && (unsigned)tmp == key.size()) {
        disk = key;
    } else {
        dev_t dev;

        if (key[0] == '/')
            dev = TPath(key).GetDev();
        else if (key[0] == '.')
            dev = TPath(root / key).GetDev();
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

    return OK;
}

TError TBlkioSubsystem::GetIoStat(TCgroup &cg, enum IoStat stat, TUintMap &map) const {
    std::vector<std::string> lines;
    std::string knob, prev, name;
    bool summ = false, hide = false;
    bool recursive = true;
    TError error;

    if (stat & IoStat::Time)
        knob = "blkio.io_service_time_recursive";
    else if (HasThrottler && (HasSaneBehavior || !cg.IsRoot())) {
        /* get statistics from throttler if possible, it has couners for raids */
        knob = (stat & IoStat::Iops) ? "blkio.throttle.io_serviced" : "blkio.throttle.io_service_bytes";
        /* throtter is recurisve only in sane behavior */
        recursive = HasSaneBehavior;
    } else
        knob = (stat & IoStat::Iops) ? "blkio.io_serviced_recursive" : "blkio.io_service_bytes_recursive";

    error = cg.Knob(knob).ReadLines(lines);
    if (error)
        return error;

    if (!recursive) {
        std::vector<TCgroup> list;

        error = cg.ChildsAll(list);
        if (error) {
            L_WRN("Cannot get io stat {}", error);
            return error;
        }

        for (auto &child_cg: list) {
            error = child_cg.Knob(knob).ReadLines(lines);
            if (error && error.Errno != ENOENT) {
                L_WRN("Cannot get io stat {}", error);
                return error;
            }
        }
    }

    uint64_t total = 0;
    for (auto &line: lines) {
        auto word = SplitString(line, ' ');
        if (word.size() != 3)
            continue;

        if (word[1] == "Read") {
            if (stat & IoStat::Write)
                continue;
        } else if (word[1] == "Write") {
            if (stat & IoStat::Read)
                continue;
        } else
            continue;

        if (word[0] != prev) {
            if (DiskName(word[0], name))
                continue;
            prev = word[0];
            summ = StringStartsWith(name, "sd") ||
                   StringStartsWith(name, "nvme") ||
                   StringStartsWith(name, "vd");
            hide = StringStartsWith(name, "ram");
        }

        if (hide)
            continue;

        uint64_t val;
        if (!StringToUint64(word[2], val) && val) {
            map[name] += val;
            if (summ)
                total += val;
        }
    }
    map["hw"] = total;

    return OK;
}

TError TBlkioSubsystem::SetIoLimit(TCgroup &cg, const TPath &root,
                                   const TUintMap &map, bool iops) {
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

        error = ResolveDisk(root, key, disk);
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
    double bfq_weight = weight;
    TError error;

    /*
     * Cgroup v1 mess:
     * CFQ: 10..1000 default 500
     * BFQ: 1..1000 default 100
     */

    if (policy == "rt" || policy == "high") {
        weight *= 1000;
        bfq_weight *= 1000;
    } else if (policy == "" || policy == "none" || policy == "normal") {
        weight *= 500;
        bfq_weight *= 100;
    } else if (policy == "batch" || policy == "idle") {
        weight *= 10;
        bfq_weight *= 1;
    } else
        return TError(EError::InvalidValue, "unknown policy: " + policy);

    if (cg.Has(CFQ_WEIGHT)) {
        error = cg.SetUint64(CFQ_WEIGHT, std::min(std::max(weight, 10.), 1000.));
        if (error)
            return error;
    }

    if (cg.Has(BFQ_WEIGHT)) {
        error = cg.SetUint64(BFQ_WEIGHT, std::min(std::max(bfq_weight, 1.), 1000.));
        if (error)
            return error;
    }

    return OK;
}

// Devices

// Pids

TError TPidsSubsystem::GetUsage(TCgroup &cg, uint64_t &usage) const {
    return cg.GetUint64("pids.current", usage);
}

TError TPidsSubsystem::SetLimit(TCgroup &cg, uint64_t limit) const {
    if (!limit)
        return cg.Set("pids.max", "max");
    return cg.SetUint64("pids.max", limit);
}

// Systemd

TError TSystemdSubsystem::InitializeSubsystem() {
    TError error = TaskCgroup(getpid(), PortoService);
    if (!error)
        L_CG("porto service: {}", PortoService);
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
TPidsSubsystem      PidsSubsystem;
TPerfSubsystem      PerfSubsystem;
TSystemdSubsystem   SystemdSubsystem;
TCgroup2Subsystem   Cgroup2Subsystem;

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
    &PerfSubsystem,
    &SystemdSubsystem,
    &Cgroup2Subsystem,
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
        error = root.Remount(MS_NODEV | MS_NOSUID | MS_NOEXEC | MS_ALLOW_WRITE);
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
            if (mnt.Type == subsys->MountType && mnt.HasOption(subsys->TestOption())) {
                subsys->Root = mnt.Target;
                L_CG("Found cgroup subsystem {} mounted at {}", subsys->Type, subsys->Root);
                break;
            }
        }
    }

    if (config().daemon().merge_memory_blkio_controllers() &&
            !MemorySubsystem.IsDisabled() && !BlkioSubsystem.IsDisabled() &&
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
            L_CG("Cannot merge memory and blkio {}", error);
            (void)path.Rmdir();
        }
    }

    for (auto subsys: AllSubsystems) {
        if (subsys->IsDisabled() || subsys->Root)
            continue;

        TPath path = root / subsys->Type;

        L_CG("Mount cgroup subsysem {} at {}", subsys->Type, path);
        if (!path.Exists()) {
            error = path.Mkdir(0755);
            if (error) {
                L_ERR("Cannot create cgroup mountpoint: {}", error);
                continue;
            }
        }

        error = path.Mount(subsys->MountType, subsys->MountType, 0, subsys->MountOptions() );
        if (error) {
            (void)path.Rmdir();
            L_ERR("Cannot mount cgroup: {}", error);
            continue;
        }

        subsys->Root = path;
    }

    for (auto subsys: AllSubsystems) {
        if (!subsys->Root)
            continue;
        error = subsys->InitializeSubsystem();
        if (error) {
            L_ERR("Cannot initialize cgroup subsystem{}: {}", subsys->Type, error);
            subsys->Root = "";
        }
    }

    for (auto subsys: AllSubsystems) {
        if (subsys->IsDisabled()) {
            L_CG("Cgroup subsysem {} is disabled", subsys->Type);
            continue;
        }

        if (!subsys->Root) {
            if (subsys->IsOptional()) {
                L_CG("Cgroup subsystem {} is not supported", subsys->Type);
                continue;
            }
            return TError(EError::NotSupported, "Cgroup {} is not supported", subsys->Type);
        }

        error = subsys->Base.OpenDir(subsys->Root);
        if (error) {
            L_ERR("Cannot open cgroup {} root directory: {}", subsys->Type, error);
            return error;
        }

        Subsystems.push_back(subsys);

        subsys->Hierarchy = subsys;
        subsys->Controllers |= subsys->Kind;
        for (auto hy: Hierarchies) {
            if (subsys->Root == hy->Root) {
                L_CG("Cgroup subsystem {} bound to hierarchy {}", subsys->Type, hy->Type);
                subsys->Hierarchy = hy;
                hy->Controllers |= subsys->Kind;
                break;
            }
        }

        if (subsys->Hierarchy == subsys)
            Hierarchies.push_back(subsys);

        subsys->Supported = true;
    }

    for (auto subsys: AllSubsystems)
        if (subsys->Hierarchy)
            subsys->Controllers |= subsys->Hierarchy->Controllers;

    /* This piece of code should never be executed. */
    TPath("/usr/sbin/cgclear").Chmod(0);

    return error;
}

TError InitializeDaemonCgroups() {
    std::vector<TSubsystem *> DaemonSubsystems = {
        &FreezerSubsystem,
        &MemorySubsystem,
        &CpuacctSubsystem,
        &PerfSubsystem,
    };
    if (Cgroup2Subsystem.Supported)
        DaemonSubsystems.push_back(&Cgroup2Subsystem);

    for (auto subsys : DaemonSubsystems) {
        auto hy = subsys->Hierarchy;
        TError error;

        if (!hy)
            continue;

        TCgroup cg = hy->Cgroup(PORTO_DAEMON_CGROUP);
        error = cg.Recreate();
        if (error)
            return error;

        // portod
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
    error = cg.Recreate();
    if (error)
        return error;

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

    if (Cgroup2Subsystem.Supported) {
        cg = Cgroup2Subsystem.Cgroup(PORTO_CGROUP_PREFIX);
        if (!cg.Exists()) {
            error = cg.Create();
            if (error)
                return error;
        }
    }

    return OK;
}
