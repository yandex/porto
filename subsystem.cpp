#include "subsystem.hpp"

using namespace std;

TSubsystem::TSubsystem(string name) : name(name) {
}

string TSubsystem::Name() {
    return name;
}
