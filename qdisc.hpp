#ifndef __QDISC_H__
#define __QDISC_H__

#include <memory>
#include <string>

#include "error.hpp"
#include "util/netlink.hpp"

class TQdisc {
    const std::string Device;
    const uint32_t Handle;
    const uint32_t DefClass;

public:
    TQdisc(const std::string &device, uint32_t handle, uint32_t defClass) : Device(device), Handle(handle), DefClass(defClass) { }
    ~TQdisc() { Remove(); }

    TError Create();
    TError Remove();
    uint32_t GetHandle() { return Handle; }
    const std::string &GetDevice() { return Device; }

};

class TTclass {
    const std::shared_ptr<TQdisc> ParentQdisc;
    const std::shared_ptr<TTclass> ParentTclass;
    const uint32_t Handle;

public:
    TTclass(const std::shared_ptr<TQdisc> qdisc, uint32_t handle) : ParentQdisc(qdisc), Handle(handle) { }
    TTclass(const std::shared_ptr<TTclass> tclass, uint32_t handle) : ParentTclass(tclass), Handle(handle) { }
    ~TTclass() { Remove(); }

    TError Create(uint32_t prio, uint32_t rate, uint32_t ceil);
    TError Remove();
    const std::string &GetDevice();
    uint32_t GetParent();
    uint32_t GetHandle() { return Handle; }
    TError GetStat(ETclassStat stat, uint64_t &val);
};

class TFilter {
    const std::shared_ptr<TQdisc> Parent;

public:
    TFilter(const std::shared_ptr<TQdisc> parent) : Parent(parent) { }
    TError Create();
    TError Remove();
};

#endif
