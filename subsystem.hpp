#ifndef __SUBSYSTEM_HPP__
#define __SUBSYSTEM_HPP__

#include <ostream>
#include <string>
#include <memory>

class TCgroup;

class TSubsystem {
    std::string name;

protected:
    TSubsystem(const std::string &name) : name(name) {}

public:
    static std::shared_ptr<TSubsystem> Get(std::string name);
    const std::string& Name() const;

    TSubsystem(const TSubsystem&) = delete;
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

extern std::shared_ptr<TMemorySubsystem> MemorySubsystem;
extern std::shared_ptr<TFreezerSubsystem> FreezerSubsystem;
extern std::shared_ptr<TCpuSubsystem> CpuSubsystem;
extern std::shared_ptr<TCpuacctSubsystem> CpuacctSubsystem;

#endif
