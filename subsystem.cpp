#include <unordered_map>

#include "cgroup.hpp"
#include "subsystem.hpp"
#include "log.hpp"
#include "util/string.hpp"

using namespace std;

static unordered_map<string, shared_ptr<TSubsystem>> subsystems;

// TSubsystem
shared_ptr<TSubsystem> TSubsystem::Get(std::string name) {
    if (subsystems.find(name) == subsystems.end()) {
        if (name == "memory")
            subsystems[name] = make_shared<TMemorySubsystem>();
        else if (name == "freezer")
            subsystems[name] = make_shared<TFreezerSubsystem>();
        else if (name == "cpu")
            subsystems[name] = make_shared<TCpuSubsystem>();
        else
            subsystems[name] = make_shared<TSubsystem>(name);
    }

    return subsystems[name];
}

string TSubsystem::Name() {
    return name;
}

// Memory
shared_ptr<TMemorySubsystem> TSubsystem::Memory() {
    return static_pointer_cast<TMemorySubsystem>(Get("memory"));
}

#include <iostream>

TError TMemorySubsystem::Usage(shared_ptr<TCgroup> &cg, uint64_t &value) {
    string s;
    TError error = cg->GetKnobValue("memory.usage_in_bytes", s);
    if (error)
        return error;
    return StringToUint64(s, value);
}

// Freezer
shared_ptr<TFreezerSubsystem> TSubsystem::Freezer() {
    return static_pointer_cast<TFreezerSubsystem>(Get("freezer"));
}

void TFreezerSubsystem::WaitState(TCgroup &cg, const std::string &state) {
    string s;
    do {
        TError error = cg.GetKnobValue("freezer.state", s);
        if (error)
            TLogger::LogError(error, "Can't freeze cgroup");
    } while (s != state);
}

void TFreezerSubsystem::Freeze(TCgroup &cg) {
    cg.SetKnobValue("freezer.state", "FROZEN");
    WaitState(cg, "FROZEN\n");
}

void TFreezerSubsystem::Unfreeze(TCgroup &cg) {
    cg.SetKnobValue("freezer.state", "THAWED");
    WaitState(cg, "THAWED\n");
}

// Cpu
shared_ptr<TCpuSubsystem> TSubsystem::Cpu() {
    return static_pointer_cast<TCpuSubsystem>(Get("cpu"));
}

// Cpuacct
shared_ptr<TCpuacctSubsystem> TSubsystem::Cpuacct() {
    return static_pointer_cast<TCpuacctSubsystem>(Get("cpuacct"));
}

TError TCpuacctSubsystem::Usage(shared_ptr<TCgroup> &cg, uint64_t &value) {
    string s;
    TError error = cg->GetKnobValue("cpuacct.usage", s);
    if (error)
        return error;
    return StringToUint64(s, value);
}
