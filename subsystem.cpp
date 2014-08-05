#include "registry.hpp"
#include "subsystem.hpp"

using namespace std;

shared_ptr<TSubsystem> TSubsystem::Memory() {
    return TRegistry<TSubsystem>::Get(TSubsystem("memory"));
}

shared_ptr<TSubsystem> TSubsystem::Freezer() {
    return TRegistry<TSubsystem>::Get(TSubsystem("freezer"));
}

shared_ptr<TSubsystem> TSubsystem::Cpu() {
    return TRegistry<TSubsystem>::Get(TSubsystem("cpu"));
}


TSubsystem::TSubsystem(string name) : name(name) {
}

string TSubsystem::Name() {
    return name;
}
