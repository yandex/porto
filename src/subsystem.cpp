#include "subsystem.hpp"
#include "config.hpp"
#include "cgroup.hpp"
#include "util/log.hpp"
#include "util/file.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"

extern "C" {
#include <unistd.h>
}

using std::string;
using std::shared_ptr;
using std::vector;

shared_ptr<TMemorySubsystem> memorySubsystem(new TMemorySubsystem);
shared_ptr<TFreezerSubsystem> freezerSubsystem(new TFreezerSubsystem);
shared_ptr<TCpuSubsystem> cpuSubsystem(new TCpuSubsystem);
shared_ptr<TCpuacctSubsystem> cpuacctSubsystem(new TCpuacctSubsystem);
shared_ptr<TNetclsSubsystem> netclsSubsystem(new TNetclsSubsystem);
shared_ptr<TBlkioSubsystem> blkioSubsystem(new TBlkioSubsystem);
shared_ptr<TDevicesSubsystem> devicesSubsystem(new TDevicesSubsystem);

static const std::map<std::string, std::shared_ptr<TSubsystem>> subsystems = {
    { "memory", memorySubsystem },
    { "freezer", freezerSubsystem },
    { "cpu", cpuSubsystem },
    { "cpuacct", cpuacctSubsystem },
    { "net_cls", netclsSubsystem },
    { "blkio", blkioSubsystem },
    { "devices", devicesSubsystem },
};

// TSubsystem
shared_ptr<TSubsystem> TSubsystem::Get(const std::string &name) {
    if (subsystems.find(name) == subsystems.end())
        return nullptr;

    return subsystems.at(name);
}

const string& TSubsystem::GetName() const {
    return Name;
}

std::shared_ptr<TCgroup> TSubsystem::GetRootCgroup(std::shared_ptr<TMount> mount) {
    if (RootCgroup)
        return RootCgroup;

    if (mount) {
        // several controllers may be mounted into one directory
        for (auto &kv : subsystems) {
            auto &subsys = kv.second;

            if (!subsys->RootCgroup)
                continue;

            if (subsys->RootCgroup->GetMount() == mount) {
                RootCgroup = subsys->RootCgroup;
                break;
            }
        }
    }

    if (!RootCgroup) {
        TCgroup *root = new TCgroup({shared_from_this()}, mount);
        RootCgroup = std::shared_ptr<TCgroup>(root);
    }
    return RootCgroup;
}

// Memory
TError TMemorySubsystem::Usage(shared_ptr<TCgroup> cg, uint64_t &value) const {
    string s;
    TError error = cg->GetKnobValue("memory.usage_in_bytes", s);
    if (error)
        return error;
    return StringToUint64(s, value);
}

TError TMemorySubsystem::Statistics(std::shared_ptr<TCgroup> cg,
                                    const std::string &name,
                                    uint64_t &val) const {
    vector<string> lines;
    TError error = cg->GetKnobValueAsLines("memory.stat", lines);
    if (error)
        return error;

    for (auto &line : lines) {
        vector<string> tokens;
        error = SplitString(line, ' ', tokens);
        if (error)
            return error;

        if (tokens.size() != 2)
            continue;

        if (tokens[0] == name)
            return StringToUint64(tokens[1], val);
    }

    return TError(EError::InvalidValue, "Invalid memory cgroup stat: " + name);
}

TError TMemorySubsystem::UseHierarchy(std::shared_ptr<TCgroup> cg, bool enable) const {
    return TError(cg->SetKnobValue("memory.use_hierarchy", enable ? "1" : "0"));
}

TError TMemorySubsystem::GetSoftLimit(std::shared_ptr<TCgroup> cg, uint64_t &limit) {
    std::string v;
    TError error = cg->GetKnobValue("memory.soft_limit_in_bytes", v);
    if (error)
        return error;

    return StringToUint64(v, limit);
}

TError TMemorySubsystem::SetSoftLimit(std::shared_ptr<TCgroup> cg, uint64_t limit) {
    return cg->SetKnobValue("memory.soft_limit_in_bytes", std::to_string(limit), false);
}

TError TMemorySubsystem::SetGuarantee(std::shared_ptr<TCgroup> cg, uint64_t guarantee) {
    if (!SupportGuarantee())
        return TError::Success();

    return cg->SetKnobValue("memory.low_limit_in_bytes", std::to_string(guarantee), false);
}

TError TMemorySubsystem::SetLimit(std::shared_ptr<TCgroup> cg, uint64_t limit) {
    if (limit == 0)
        return TError::Success();

    TError error = cg->SetKnobValue("memory.limit_in_bytes", std::to_string(limit), false);
    if (error)
        return error;

    if (SupportSwap()) {
        error = cg->SetKnobValue("memory.memsw.limit_in_bytes", std::to_string(limit), false);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TMemorySubsystem::RechargeOnPgfault(std::shared_ptr<TCgroup> cg, bool enable) {
    if (!SupportRechargeOnPgfault())
        return TError::Success();

    string value = enable ? "1" : "0";
    return cg->SetKnobValue("memory.recharge_on_pgfault", value, false);
}

bool TMemorySubsystem::SupportGuarantee() {
    return GetRootCgroup()->HasKnob("memory.low_limit_in_bytes");
}

bool TMemorySubsystem::SupportRechargeOnPgfault() {
    return GetRootCgroup()->HasKnob("memory.recharge_on_pgfault");
}

bool TMemorySubsystem::SupportSwap() {
    return GetRootCgroup()->HasKnob("memory.memsw.limit_in_bytes");
}

bool TMemorySubsystem::SupportIoLimit() {
    return GetRootCgroup()->HasKnob("memory.fs_bps_limit");
}

bool TMemorySubsystem::SupportDirtyLimit() {
    return GetRootCgroup()->HasKnob("memory.dirty_limit_in_bytes");
}

TError TMemorySubsystem::SetIoLimit(std::shared_ptr<TCgroup> cg, uint64_t limit) {
    if (!SupportIoLimit())
        return TError::Success();

    return cg->SetKnobValue("memory.fs_bps_limit", std::to_string(limit), false);
}

TError TMemorySubsystem::SetDirtyLimit(std::shared_ptr<TCgroup> cg, uint64_t limit) {
    if (!SupportDirtyLimit())
        return TError::Success();

    if (limit)
        return cg->SetKnobValue("memory.dirty_limit_in_bytes", std::to_string(limit), false);
    else
        return cg->SetKnobValue("memory.dirty_ratio", "50", false);
}

// Freezer
TError TFreezerSubsystem::WaitState(std::shared_ptr<TCgroup> cg,
                                    const std::string &state) const {

    int ret = RetryFailed(config().daemon().freezer_wait_timeout_s() * 10, 100, [&]{
        string s;
        TError error = cg->GetKnobValue("freezer.state", s);
        if (error)
            L_ERR() << "Can't freeze cgroup: " << error << std::endl;

        return StringTrim(s) != state;
    });

    if (ret) {
        string s = "?";
        (void)cg->GetKnobValue("freezer.state", s);

        TError error(EError::Unknown, "Can't wait " + std::to_string(config().daemon().freezer_wait_timeout_s()) + "s for freezer state " + state + ", current state is " + s);
        if (error)
            L_ERR() << cg->Relpath() << ": " << error << std::endl;
        return error;
    }
    return TError::Success();
}

TError TFreezerSubsystem::Freeze(std::shared_ptr<TCgroup> cg) const {
    return cg->SetKnobValue("freezer.state", "FROZEN");
}

TError TFreezerSubsystem::Unfreeze(std::shared_ptr<TCgroup> cg) const {
    return cg->SetKnobValue("freezer.state", "THAWED");
}

TError TFreezerSubsystem::WaitForFreeze(std::shared_ptr<TCgroup> cg) const {
    return WaitState(cg, "FROZEN");
}

TError TFreezerSubsystem::WaitForUnfreeze(std::shared_ptr<TCgroup> cg) const {
    return WaitState(cg, "THAWED");
}

bool TFreezerSubsystem::IsFrozen(std::shared_ptr<TCgroup> cg) const {
    string s;
    TError error = cg->GetKnobValue("freezer.state", s);
    if (error)
        return false;
    return StringTrim(s) != "THAWED";
}

// Cpu
TError TCpuSubsystem::SetPolicy(std::shared_ptr<TCgroup> cg, const std::string &policy) {
    if (!SupportSmart())
        return TError::Success();

    if (policy == "normal") {
        TError error = cg->SetKnobValue("cpu.smart", "0", false);
        if (error) {
            L_ERR() << "Can't disable smart: " << error << std::endl;
            return error;
        }
    } else if (policy == "rt") {
        TError error = cg->SetKnobValue("cpu.smart", "1", false);
        if (error) {
            L_ERR() << "Can't set enable smart: " << error << std::endl;
            return error;
        }
    }

    return TError::Success();
}

TError TCpuSubsystem::SetLimit(std::shared_ptr<TCgroup> cg, const uint64_t limit) {
    if (!SupportLimit())
        return TError::Success();

    if (limit == 100)
        return cg->SetKnobValue("cpu.cfs_quota_us", "-1", false);

    std::string periodStr;
    TError error = cg->GetKnobValue("cpu.cfs_period_us", periodStr);
    if (error)
        return error;

    uint64_t period;
    error = StringToUint64(periodStr, period);
    if (error)
        return TError(EError::Unknown, "Can't parse cpu.cfs_period_us");

    uint64_t runtime = GetNumCores() * period * limit / 100;
    const uint64_t minQuota = 1000;
    if (runtime < minQuota)
        runtime = minQuota;
    return cg->SetKnobValue("cpu.cfs_quota_us", std::to_string(runtime), false);
}

TError TCpuSubsystem::SetGuarantee(std::shared_ptr<TCgroup> cg, uint64_t guarantee) {
    if (!SupportGuarantee())
        return TError::Success();

    uint64_t rootShares;
    std::string str;
    TError error = GetRootCgroup()->GetKnobValue("cpu.shares", str);
    if (error)
        return TError(EError::Unknown, "Can't get root cpu.shares");
    error = StringToUint64(str, rootShares);
    if (error)
        return TError(EError::Unknown, "Can't parse root cpu.shares");

    if (guarantee == 0)
        guarantee = 1;

    return cg->SetKnobValue("cpu.shares", std::to_string(guarantee * rootShares), false);
}

bool TCpuSubsystem::SupportSmart() {
    return GetRootCgroup()->HasKnob("cpu.smart");
}

bool TCpuSubsystem::SupportLimit() {
    return GetRootCgroup()->HasKnob("cpu.cfs_period_us");
}

bool TCpuSubsystem::SupportGuarantee() {
    return GetRootCgroup()->HasKnob("cpu.shares");
}

// Cpuacct
TError TCpuacctSubsystem::Usage(shared_ptr<TCgroup> cg, uint64_t &value) const {
    string s;
    TError error = cg->GetKnobValue("cpuacct.usage", s);
    if (error)
        return error;
    return StringToUint64(s, value);
}

// Netcls

// Blkio

TError TBlkioSubsystem::GetStatLine(const vector<string> &lines,
                                    const size_t i,
                                    const std::string &name,
                                    uint64_t &val) const {
    vector<string> tokens;
    TError error = SplitString(lines[i], ' ', tokens);
    if (error)
        return error;

    if (tokens.size() < 3 || tokens[1] != name)
        return TError(EError::Unknown, "Unexpected field in blkio statistics");

    return StringToUint64(tokens[2], val);
}

TError TBlkioSubsystem::GetDevice(const std::string &majmin,
                                  std::string &device) const {
    TFile f("/sys/dev/block/" + majmin + "/uevent");
    vector<string> lines;
    TError error = f.AsLines(lines);
    if (error)
        return error;

    for (auto &line : lines) {
        vector<string> tokens;
        error = SplitString(line, '=', tokens);
        if (error)
            return error;

        if (tokens.size() != 2)
            continue;

        if (tokens[0] == "DEVNAME") {
            device = tokens[1];
            return TError::Success();
        }
    }

    return TError(EError::Unknown, "Unable to convert device maj+min to name");
}

TError TBlkioSubsystem::Statistics(std::shared_ptr<TCgroup> cg,
                                   const std::string &file,
                                   std::vector<BlkioStat> &stat) const {
    vector<string> lines;
    TError error = cg->GetKnobValueAsLines(file, lines);
    if (error)
        return error;

    BlkioStat s;
    for (size_t i = 0; i < lines.size(); i += 5) {
        vector<string> tokens;
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

TError TBlkioSubsystem::SetPolicy(std::shared_ptr<TCgroup> cg, bool batch) {
    if (!SupportPolicy())
        return TError::Success();

    std::string rootWeight;
    if (!batch) {
        TError error = GetRootCgroup()->GetKnobValue("blkio.weight", rootWeight);
        if (error)
            return TError(EError::Unknown, "Can't get root blkio.weight");
    }

    return cg->SetKnobValue("blkio.weight", batch ? std::to_string(config().container().batch_io_weight()) : rootWeight, false);
}

bool TBlkioSubsystem::SupportPolicy() {
    return GetRootCgroup()->HasKnob("blkio.weight");
}

// Devices

TError TDevicesSubsystem::AllowDevices(std::shared_ptr<TCgroup> cg, const std::vector<std::string> &allowed) {
    vector<string> lines;

    TError error = cg->GetKnobValueAsLines("devices.list", lines);
    if (error)
        return error;

    bool needUpdate = lines.size() != allowed.size();
    if (!needUpdate) {
        for (auto &line : lines) {
            for (auto &dev : allowed) {
                if (StringTrim(line) != StringTrim(dev)) {
                    needUpdate = true;
                    break;
                }
            }
            if (needUpdate)
                break;
        }

        if (!needUpdate) {
            L() << "Don't update allowed devices" << std::endl;
            return TError::Success();
        }
    }

    error = cg->SetKnobValue("devices.deny", "a", false);
    if (error)
        return error;

    for (auto &dev : allowed) {
        error = cg->SetKnobValue("devices.allow", dev, false);
        if (error)
            return error;
    }

    return TError::Success();
}
