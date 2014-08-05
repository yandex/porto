#include <unordered_map>

#include "cgroup.hpp"
#include "subsystem.hpp"

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

TSubsystem::TSubsystem(string name) : name(name) {
}

string TSubsystem::Name() {
    return name;
}

// Memory
shared_ptr<TMemorySubsystem> TSubsystem::Memory() {
    return static_pointer_cast<TMemorySubsystem>(Get("memory"));
}

unsigned long TMemorySubsystem::Usage(TCgroup &cg) {
    return stoul(cg.GetKnobValue("memory.usage_in_bytes"));
}

// Freezer
shared_ptr<TFreezerSubsystem> TSubsystem::Freezer() {
    return static_pointer_cast<TFreezerSubsystem>(Get("freezer"));
}

void TFreezerSubsystem::Freeze(TCgroup &cg) {
    cg.SetKnobValue("freezer.state", "FROZEN");
}

void TFreezerSubsystem::Unfreeze(TCgroup &cg) {
    cg.SetKnobValue("freezer.state", "THAWED");
}

// Cpu
shared_ptr<TCpuSubsystem> TSubsystem::Cpu() {
    return static_pointer_cast<TCpuSubsystem>(Get("cpu"));
}
