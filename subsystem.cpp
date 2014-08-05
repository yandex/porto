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
    return static_pointer_cast<TMemorySubsystem>(subsystems["memory"]);
}

// Freezer
shared_ptr<TFreezerSubsystem> TSubsystem::Freezer() {
    return static_pointer_cast<TFreezerSubsystem>(subsystems["freezer"]);
}

void TFreezerSubsystem::Freeze(TCgroup &cg) {
    // TODO
}

void TFreezerSubsystem::Unfreeze(TCgroup &cg) {
    // TODO
}

// Cpu
shared_ptr<TCpuSubsystem> TSubsystem::Cpu() {
    return static_pointer_cast<TCpuSubsystem>(subsystems["cpu"]);
}
