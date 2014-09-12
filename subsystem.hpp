#ifndef __SUBSYSTEM_HPP__
#define __SUBSYSTEM_HPP__

#include <ostream>
#include <string>
#include <memory>

#include "cgroup.hpp"

class TCgroup;

class TSubsystem : public std::enable_shared_from_this<TSubsystem> {
    std::string Name;
    std::shared_ptr<TCgroup> RootCgroup;

protected:
    TSubsystem(const std::string &name) : Name(name) {}

public:
    static std::shared_ptr<TSubsystem> Get(std::string name);
    const std::string& GetName() const;

    TSubsystem(const TSubsystem &) = delete;
    TSubsystem &operator=(const TSubsystem &) = delete;

    std::shared_ptr<TCgroup> GetRootCgroup(std::shared_ptr<TMount> mount=nullptr) {
        if (!RootCgroup) {
            TCgroup *root = new TCgroup({shared_from_this()}, mount);
            RootCgroup = std::shared_ptr<TCgroup>(root);
        }
        return RootCgroup;
    }
};

class TMemorySubsystem : public TSubsystem {
public:
    TMemorySubsystem() : TSubsystem("memory") {}
    TError Usage(std::shared_ptr<TCgroup> &cg, uint64_t &value) const;
    TError UseHierarchy(TCgroup &cg) const;
};

class TFreezerSubsystem : public TSubsystem {
public:
    TFreezerSubsystem() : TSubsystem("freezer") {}

    TError WaitState(TCgroup &cg, const std::string &state) const;
    TError Freeze(TCgroup &cg) const;
    TError Unfreeze(TCgroup &cg) const;
};

class TCpuSubsystem : public TSubsystem {
public:
    TCpuSubsystem() : TSubsystem("cpu") {}
};

class TCpuacctSubsystem : public TSubsystem {
public:
    TCpuacctSubsystem() : TSubsystem("cpuacct") {}
    TError Usage(std::shared_ptr<TCgroup> &cg, uint64_t &value) const;
};

class TNetclsSubsystem : public TSubsystem {
public:
    TNetclsSubsystem() : TSubsystem("net_cls") {}
};

extern std::shared_ptr<TMemorySubsystem> memorySubsystem;
extern std::shared_ptr<TFreezerSubsystem> freezerSubsystem;
extern std::shared_ptr<TCpuSubsystem> cpuSubsystem;
extern std::shared_ptr<TCpuacctSubsystem> cpuacctSubsystem;
extern std::shared_ptr<TNetclsSubsystem> netclsSubsystem;

#endif
