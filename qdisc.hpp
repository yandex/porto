#ifndef __QDISC_H__
#define __QDISC_H__

#include <memory>
#include <string>

#include "error.hpp"

struct nl_sock;
struct rtnl_link;
struct nl_cache;

class TNetlink {
    struct nl_sock *sock;
    struct rtnl_link *link;
    struct nl_cache *cache;

public:
    TError Open(const std::string &device);
    void Close();
    void Log(const std::string &prefix, void *obj);
    TError AddClass(uint32_t parent, uint32_t handle, uint32_t prio, uint32_t rate, uint32_t ceil);
    TError RemoveClass(uint32_t parent, uint32_t handle);
    TError AddHTB(uint32_t parent, uint32_t handle, uint32_t defaultClass);
    TError RemoveHTB(uint32_t parent);
    ~TNetlink() { Close(); }
};

uint32_t TcHandle(uint16_t maj, uint16_t min);

class TQdisc {
    const std::string Device;
    const uint32_t Handle;
    const uint32_t DefClass;

public:
    TQdisc(const std::string &device, uint32_t handle, uint32_t defClass) : Device(device), Handle(handle), DefClass(defClass) { }
    ~TQdisc() { Remove(); }

    TError Create();
    TError Remove();
    uint32_t GetHandle();
    const std::string &GetDevice();
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
    uint16_t GetMajor();
};

#endif
