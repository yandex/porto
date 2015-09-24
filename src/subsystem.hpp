#pragma once

#include <ostream>
#include <string>
#include <memory>
#include <map>

#include "common.hpp"

class TCgroup;
class TMount;

class TSubsystem : public std::enable_shared_from_this<TSubsystem>,
                   public TNonCopyable {
    std::string Name;
    std::shared_ptr<TCgroup> RootCgroup;

protected:
    TSubsystem(const std::string &name) : Name(name) {}

public:
    static std::shared_ptr<TSubsystem> Get(const std::string &name);
    const std::string& GetName() const;
    std::shared_ptr<TCgroup> GetRootCgroup(std::shared_ptr<TMount> mount=nullptr);
};

class TMemorySubsystem : public TSubsystem {
public:
    TMemorySubsystem() : TSubsystem("memory") {}
    TError Usage(std::shared_ptr<TCgroup> cg, uint64_t &value) const;
    TError Statistics(std::shared_ptr<TCgroup> cg,
                      const std::string &name,
                      uint64_t &val) const;
    TError UseHierarchy(std::shared_ptr<TCgroup> cg, bool enable) const;
    TError GetSoftLimit(std::shared_ptr<TCgroup> cg, uint64_t &limit);
    TError SetSoftLimit(std::shared_ptr<TCgroup> cg, uint64_t limit);
    bool SupportGuarantee();
    TError SetGuarantee(std::shared_ptr<TCgroup> cg, uint64_t guarantee);
    bool SupportIoLimit();
    bool SupportDirtyLimit();
    TError SetLimit(std::shared_ptr<TCgroup> cg, uint64_t limit);
    bool SupportRechargeOnPgfault();
    bool SupportSwap();
    TError RechargeOnPgfault(std::shared_ptr<TCgroup> cg, bool enable);
    TError SetIoLimit(std::shared_ptr<TCgroup> cg, uint64_t limit);
    TError SetDirtyLimit(std::shared_ptr<TCgroup> cg, uint64_t limit);
};

class TFreezerSubsystem : public TSubsystem {
public:
    TFreezerSubsystem() : TSubsystem("freezer") {}

    TError WaitState(std::shared_ptr<TCgroup> cg,
                     const std::string &state) const;
    TError Freeze(std::shared_ptr<TCgroup> cg) const;
    TError Unfreeze(std::shared_ptr<TCgroup> cg) const;
    bool IsFrozen(std::shared_ptr<TCgroup> cg) const;

    TError WaitForFreeze(std::shared_ptr<TCgroup> cg) const;
    TError WaitForUnfreeze(std::shared_ptr<TCgroup> cg) const;
};

class TCpuSubsystem : public TSubsystem {
public:
    TCpuSubsystem() : TSubsystem("cpu") {}
    TError SetPolicy(std::shared_ptr<TCgroup> cg, const std::string &policy);
    TError SetLimit(std::shared_ptr<TCgroup> cg, double limit);
    TError SetGuarantee(std::shared_ptr<TCgroup> cg, double guarantee);
    bool SupportSmart();
    bool SupportLimit();
    bool SupportGuarantee();
};

class TCpuacctSubsystem : public TSubsystem {
public:
    TCpuacctSubsystem() : TSubsystem("cpuacct") {}
    TError Usage(std::shared_ptr<TCgroup> cg, uint64_t &value) const;
};

class TNetclsSubsystem : public TSubsystem {
public:
    TNetclsSubsystem() : TSubsystem("net_cls") {}
};

struct BlkioStat {
    std::string Device;
    uint64_t Read;
    uint64_t Write;
    uint64_t Sync;
    uint64_t Async;
};

class TBlkioSubsystem : public TSubsystem {
    TError GetStatLine(const std::vector<std::string> &lines,
                       const size_t i,
                       const std::string &name,
                       uint64_t &val) const;
    TError GetDevice(const std::string &majmin,
                     std::string &device) const;
public:
    TBlkioSubsystem() : TSubsystem("blkio") {}
    TError Statistics(std::shared_ptr<TCgroup> cg,
                      const std::string &file,
                      std::vector<BlkioStat> &stat) const;
    TError SetPolicy(std::shared_ptr<TCgroup> cg, bool batch);
    bool SupportPolicy();
};

class TDevicesSubsystem : public TSubsystem {
public:
    TDevicesSubsystem() : TSubsystem("devices") {}
    TError AllowDevices(std::shared_ptr<TCgroup> cg, const std::vector<std::string> &allowed);
};

extern std::shared_ptr<TMemorySubsystem> memorySubsystem;
extern std::shared_ptr<TFreezerSubsystem> freezerSubsystem;
extern std::shared_ptr<TCpuSubsystem> cpuSubsystem;
extern std::shared_ptr<TCpuacctSubsystem> cpuacctSubsystem;
extern std::shared_ptr<TNetclsSubsystem> netclsSubsystem;
extern std::shared_ptr<TBlkioSubsystem> blkioSubsystem;
extern std::shared_ptr<TDevicesSubsystem> devicesSubsystem;
