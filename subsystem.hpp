#ifndef __SUBSYSTEM_HPP__
#define __SUBSYSTEM_HPP__

#include <ostream>
#include <string>
#include <memory>

#include "cgroup.hpp"

class TCgroup;

class TSubsystem : public std::enable_shared_from_this<TSubsystem> {
    std::string name;
    std::shared_ptr<TCgroup> root_cgroup;

protected:
    TSubsystem(const std::string &name) : name(name) {}

public:
    static std::shared_ptr<TSubsystem> Get(std::string name);
    const std::string& Name() const;

    TSubsystem(const TSubsystem&) = delete;

    std::shared_ptr<TCgroup> GetRootCgroup(std::shared_ptr<TMount> mount=nullptr) {
        if (!root_cgroup) {
            TCgroup *root = new TCgroup({shared_from_this()}, mount);
            root_cgroup = std::shared_ptr<TCgroup>(root);
        }
        return root_cgroup;
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

extern std::shared_ptr<TMemorySubsystem> MemorySubsystem;
extern std::shared_ptr<TFreezerSubsystem> FreezerSubsystem;
extern std::shared_ptr<TCpuSubsystem> CpuSubsystem;
extern std::shared_ptr<TCpuacctSubsystem> CpuacctSubsystem;

#endif
