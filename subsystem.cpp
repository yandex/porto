#include "cgroup.hpp"
#include "subsystem.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"

using std::string;
using std::shared_ptr;

shared_ptr<TMemorySubsystem> memorySubsystem(new TMemorySubsystem);
shared_ptr<TFreezerSubsystem> freezerSubsystem(new TFreezerSubsystem);
shared_ptr<TCpuSubsystem> cpuSubsystem(new TCpuSubsystem);
shared_ptr<TCpuacctSubsystem> cpuacctSubsystem(new TCpuacctSubsystem);
shared_ptr<TNetclsSubsystem> netclsSubsystem(new TNetclsSubsystem);

static const std::map<std::string, std::shared_ptr<TSubsystem>> subsystems = {
    { "memory", memorySubsystem },
    { "freezer", freezerSubsystem },
    { "cpu", cpuSubsystem },
    { "cpuacct", cpuacctSubsystem },
    { "net_cls", netclsSubsystem },
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

TError TMemorySubsystem::UseHierarchy(TCgroup &cg) const {
    return TError(cg.SetKnobValue("memory.use_hierarchy", "1"));
}

// Freezer
TError TFreezerSubsystem::WaitState(TCgroup &cg, const std::string &state) const {

    int ret = RetryFailed(FREEZER_WAIT_TIMEOUT_S * 10, 100, [&]{
        string s;
        TError error = cg.GetKnobValue("freezer.state", s);
        if (error)
            TLogger::LogError(error, "Can't freeze cgroup");

        return s != state;
    });

    if (ret) {
        TError error(EError::Unknown, "Can't wait for freezer state " + state);
        TLogger::LogError(error, cg.Relpath());
        return error;
    }
    return TError::Success();
}

TError TFreezerSubsystem::Freeze(TCgroup &cg) const {
    TError error(cg.SetKnobValue("freezer.state", "FROZEN"));
    if (error)
        return error;

    return WaitState(cg, "FROZEN\n");
}

TError TFreezerSubsystem::Unfreeze(TCgroup &cg) const {
    TError error(cg.SetKnobValue("freezer.state", "THAWED"));
    if (error)
        return error;

    return WaitState(cg, "THAWED\n");
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
