#include "cgroup.hpp"
#include "subsystem.hpp"
#include "util/log.hpp"
#include "util/file.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"

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
TError TMemorySubsystem::Usage(shared_ptr<TCgroup> &cg, uint64_t &value) const {
    string s;
    TError error = cg->GetKnobValue("memory.usage_in_bytes", s);
    if (error)
        return error;
    return StringToUint64(s, value);
}

TError TMemorySubsystem::Statistics(std::shared_ptr<TCgroup> &cg, const std::string &name, uint64_t &val) const {
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

TError TMemorySubsystem::UseHierarchy(TCgroup &cg) const {
    return TError(cg.SetKnobValue("memory.use_hierarchy", "1"));
}

// Freezer
TError TFreezerSubsystem::WaitState(TCgroup &cg, const std::string &state) const {

    int ret = RetryFailed(config().daemon().freezer_wait_timeout_s() * 10, 100, [&]{
        string s;
        TError error = cg.GetKnobValue("freezer.state", s);
        if (error)
            L_ERR() << "Can't freeze cgroup: " << error << std::endl;

        return StringTrim(s) != state;
    });

    if (ret) {
        string s = "?";
        (void)cg.GetKnobValue("freezer.state", s);

        TError error(EError::Unknown, "Can't wait for freezer state " + state + ", current state is " + s);
        if (error)
            L_ERR() << cg.Relpath() << ": " << error << std::endl;
        return error;
    }
    return TError::Success();
}

TError TFreezerSubsystem::Freeze(TCgroup &cg) const {
    TError error(cg.SetKnobValue("freezer.state", "FROZEN"));
    if (error)
        return error;

    return WaitState(cg, "FROZEN");
}

TError TFreezerSubsystem::Unfreeze(TCgroup &cg) const {
    TError error(cg.SetKnobValue("freezer.state", "THAWED"));
    if (error)
        return error;

    return WaitState(cg, "THAWED");
}

bool TFreezerSubsystem::IsFreezed(TCgroup &cg) const {
    string s;
    TError error = cg.GetKnobValue("freezer.state", s);
    if (error)
        L_ERR() << "Can't get freezer status: " << error << std::endl;
    return StringTrim(s) != "THAWED";
}

// Cpu


// Cpuacct
TError TCpuacctSubsystem::Usage(shared_ptr<TCgroup> &cg, uint64_t &value) const {
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

TError TBlkioSubsystem::Statistics(std::shared_ptr<TCgroup> &cg, const std::string &file, std::vector<BlkioStat> &stat) const {
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

// Devices

TError TDevicesSubsystem::AllowDevices(std::shared_ptr<TCgroup> &cg, const std::vector<std::string> &allowed) {
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

        if (!needUpdate)
            return TError::Success();
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
