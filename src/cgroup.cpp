#include <algorithm>
#include <csignal>

#include "cgroup.hpp"
#include "device.hpp"
#include "config.hpp"
#include "value.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "util/mount.hpp"

extern "C" {
#include <fcntl.h>
#include <unistd.h>
#include <sys/eventfd.h>
}

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
        for (int i = 0; i < 100; i++) {
            usleep(config().daemon().cgroup_remove_timeout_s() * 10000);
            error = Path().Rmdir();
            if (!error || error.GetErrno() != EBUSY)
                break;
        }
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
        value = string != "0";
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
        // FIXME Ignore non-porto subtrees
        if (IsRoot() && "/" + name != PORTO_ROOT_CGROUP)
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

    file = fopen(Knob(knob).c_str(), "r");;
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
    std::string str_limit = limit ? std::to_string(limit) : "-1";
    uint64_t memswap;

    /* Memory limit cannot be bigger than Memory+Swap limit. */
    if (SupportSwap() && !cg.GetUint64(MEM_SWAP_LIMIT, memswap) &&
            (!limit || memswap < limit))
        (void)cg.Set(MEM_SWAP_LIMIT, str_limit);

    /*
     * Maxumum value depends on arch, kernel version and bugs
     * "-1" works everywhere since 2.6.31
     */
    TError error = cg.Set(LIMIT, str_limit);

    if (!error && SupportSwap())
        cg.Set(MEM_SWAP_LIMIT, str_limit);

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
                stat["unevictable"] +
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

TError TMemorySubsystem::SetupOOMEvent(TCgroup &cg, int &fd) {
    TError error;
    int cfd;

    fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (fd < 0)
        return TError(EError::Unknown, errno, "Cannot create eventfd");

    cfd = open(cg.Knob(OOM_CONTROL).c_str(), O_RDONLY | O_CLOEXEC);
    if (cfd < 0) {
        close(fd);
        return TError(EError::Unknown, errno, "Cannot open oom_control");
    }

    error = cg.Set(EVENT_CONTROL, std::to_string(fd) + " " + std::to_string(cfd));
    if (error)
        close(fd);
    close(cfd);
    return error;
}

// Freezer
TError TFreezerSubsystem::WaitState(TCgroup &cg,
                                    const std::string &state) const {
    int ret;
    if (!RetryIfFailed([&] {
                std::string s;
                TError error = cg.Get("freezer.state", s);
                if (error)
                    L_ERR() << "Can't freeze cgroup: " << error << std::endl;

                return StringTrim(s) != state;
            }, ret, config().daemon().freezer_wait_timeout_s() * 10, 100) || ret) {
        std::string s = "?";
        (void)cg.Get("freezer.state", s);

        TError error(EError::Unknown, "Can't wait " + std::to_string(config().daemon().freezer_wait_timeout_s()) + "s for freezer state " + state + ", current state is " + s);
        if (error)
            L_ERR() << cg << ": " << error << std::endl;
        return error;
    }
    return TError::Success();
}

TError TFreezerSubsystem::Freeze(TCgroup &cg) const {
    return cg.Set("freezer.state", "FROZEN");
}

TError TFreezerSubsystem::Unfreeze(TCgroup &cg) const {
    return cg.Set("freezer.state", "THAWED");
}

TError TFreezerSubsystem::WaitForFreeze(TCgroup &cg) const {
    return WaitState(cg, "FROZEN");
}

TError TFreezerSubsystem::WaitForUnfreeze(TCgroup &cg) const {
    return WaitState(cg, "THAWED");
}

bool TFreezerSubsystem::IsFrozen(TCgroup &cg) const {
    std::string s;
    TError error = cg.Get("freezer.state", s);
    if (error)
        return false;
    return StringTrim(s) != "THAWED";
}

// Cpu
TError TCpuSubsystem::SetPolicy(TCgroup &cg, const std::string &policy) {
    if (!SupportSmart())
        return TError::Success();

    if (policy == "normal") {
        TError error = cg.Set("cpu.smart", "0");
        if (error) {
            L_ERR() << "Can't disable smart: " << error << std::endl;
            return error;
        }
    } else if (policy == "rt") {
        TError error = cg.Set("cpu.smart", "1");
        if (error) {
            L_ERR() << "Can't set enable smart: " << error << std::endl;
            return error;
        }
    }

    return TError::Success();
}

TError TCpuSubsystem::SetLimit(TCgroup &cg, double limit) {
    uint64_t period, quota;

    if (!SupportLimit())
        return TError::Success();

    if (limit >= GetNumCores())
        return cg.Set("cpu.cfs_quota_us", "-1");

    TError error = cg.GetUint64("cpu.cfs_period_us", period);
    if (error)
        return error;

    quota = std::ceil(limit * period);

    const uint64_t minQuota = 1000;
    if (quota < minQuota)
        quota = minQuota;

    return cg.SetUint64("cpu.cfs_quota_us", quota);
}

TError TCpuSubsystem::SetGuarantee(TCgroup &cg, double guarantee) {
    uint64_t base, shares;

    if (!SupportGuarantee())
        return TError::Success();

    TError error = RootCgroup().GetUint64("cpu.shares", base);
    if (error)
        return error;

    shares = std::floor(guarantee * base);
    return cg.SetUint64("cpu.shares", shares);
}

bool TCpuSubsystem::SupportSmart() {
    return RootCgroup().Has("cpu.smart");
}

bool TCpuSubsystem::SupportLimit() {
    return RootCgroup().Has("cpu.cfs_period_us");
}

bool TCpuSubsystem::SupportGuarantee() {
    return RootCgroup().Has("cpu.shares");
}

// Cpuacct
TError TCpuacctSubsystem::Usage(TCgroup &cg, uint64_t &value) const {
    std::string s;
    TError error = cg.Get("cpuacct.usage", s);
    if (error)
        return error;
    return StringToUint64(s, value);
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

TError TBlkioSubsystem::SetPolicy(TCgroup &cg, bool batch) {
    if (!SupportPolicy())
        return TError::Success();

    std::string rootWeight;
    if (!batch) {
        TError error = RootCgroup().Get("blkio.weight", rootWeight);
        if (error)
            return TError(EError::Unknown, "Can't get root blkio.weight");
    }

    return cg.Set("blkio.weight", batch ? std::to_string(config().container().batch_io_weight()) : rootWeight);
}

bool TBlkioSubsystem::SupportPolicy() {
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
        "c 10:237 rmw", // /dev/loop-control FIXME
        "b 7:* rmw"     // /dev/loop*        FIXME
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
TNetclsSubsystem    NetclsSubsystem;
TBlkioSubsystem     BlkioSubsystem;
TDevicesSubsystem   DevicesSubsystem;

std::vector<TSubsystem *> AllSubsystems = {
    { &MemorySubsystem   },
    { &FreezerSubsystem  },
    { &CpuSubsystem      },
    { &CpuacctSubsystem  },
    { &NetclsSubsystem   },
    { &BlkioSubsystem    },
    { &DevicesSubsystem  },
};

std::vector<TSubsystem *> Subsystems;
std::vector<TSubsystem *> Hierarchies;


TError InitializeCgroups() {
    TPath root(config().daemon().sysfs_root());
    std::vector<std::shared_ptr<TMount>> mounts;
    TMount mount;
    TError error;

    error = mount.Find(root);
    if (error) {
        L_ERR() << "Cannot find cgroups root mount: " << error << std::endl;
        return error;
    }

    if (mount.GetMountpoint() != root) {
        error = root.Mount("cgroup", "tmpfs", 0, {});
        if (error) {
            L_ERR() << "Cannot mount cgroups root: " << error << std::endl;
            return error;
        }
    }

    error = TMount::Snapshot(mounts);
    if (error) {
        L_ERR() << "Can't create mount snapshot: " << error << std::endl;
        return error;
    }

    for (auto subsys: AllSubsystems) {
        for (auto &mnt: mounts) {
            auto data = mnt->GetData();
            if (mnt->GetType() == "cgroup" &&
                    std::find(data.begin(), data.end(), subsys->Type) != data.end()) {
                subsys->Root = mnt->GetMountpoint();
                L() << "Found cgroup subsystem " << subsys->Type << " mounted at " << subsys->Root << std::endl;
                break;
            }
        }

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
            if (error) {
                L_ERR() << "Cannot mount cgroup: " << error << std::endl;
                (void)subsys->Root.Rmdir();
                return error;
            }
        }

        Subsystems.push_back(subsys);

        subsys->Hierarchy = subsys;
        for (auto hy: Hierarchies) {
            if (subsys->Root == hy->Root) {
                L() << "Cgroup subsystem " << subsys->Type
                    << " bound to hierarchy " << hy->Type << std::endl;
                subsys->Hierarchy = hy;
                break;
            }
        }
        if (subsys->Hierarchy == subsys)
            Hierarchies.push_back(subsys);
    }

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

    return TError::Success();
}
