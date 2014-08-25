#ifndef __SUBSYSTEM_HPP__
#define __SUBSYSTEM_HPP__

#include <ostream>
#include <string>
#include <memory>

class TCgroup;
class TMemorySubsystem;
class TFreezerSubsystem;
class TCpuSubsystem;
class TCpuacctSubsystem;

class TSubsystem {
    std::string name;

public:
    static std::shared_ptr<TSubsystem> Get(std::string name);

    static std::shared_ptr<TMemorySubsystem> Memory();
    static std::shared_ptr<TFreezerSubsystem> Freezer();
    static std::shared_ptr<TCpuSubsystem> Cpu();
    static std::shared_ptr<TCpuacctSubsystem> Cpuacct();
    
    TSubsystem(const std::string &name) : name(name) { }
    std::string Name();

    friend bool operator==(const TSubsystem& c1, const TSubsystem& c2) {
        return c1.name == c2.name;
    }
};

class TMemorySubsystem : public TSubsystem {
public:
    TMemorySubsystem() : TSubsystem("memory") {}
    TError Usage(std::shared_ptr<TCgroup> &cg, uint64_t &value);
    TError UseHierarchy(TCgroup &cg);
};

class TFreezerSubsystem : public TSubsystem {
public:
    TFreezerSubsystem() : TSubsystem("freezer") {}

    TError WaitState(TCgroup &cg, const std::string &state);
    TError Freeze(TCgroup &cg);
    TError Unfreeze(TCgroup &cg);
};

class TCpuSubsystem : public TSubsystem {
public:
    TCpuSubsystem() : TSubsystem("cpu") {}
};

class TCpuacctSubsystem : public TSubsystem {
public:
    TCpuacctSubsystem() : TSubsystem("cpuacct") {}
    TError Usage(std::shared_ptr<TCgroup> &cg, uint64_t &value);
};

#endif
