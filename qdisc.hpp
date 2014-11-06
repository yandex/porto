#ifndef __QDISC_H__
#define __QDISC_H__

#include <memory>
#include <string>

#include "porto.hpp"
#include "error.hpp"
#include "util/netlink.hpp"

#include "util/log.hpp"

class TQdisc {
    NO_COPY_CONSTRUCT(TQdisc);
    std::vector<std::shared_ptr<TNlLink>> Link;
    const uint32_t Handle;
    const uint32_t DefClass;

public:
    TQdisc(const std::vector<std::shared_ptr<TNlLink>> &link, uint32_t handle, uint32_t defClass) : Link(link), Handle(handle), DefClass(defClass) { }

    TError Create();
    TError Remove();
    uint32_t GetHandle() { return Handle; }
    std::vector<std::shared_ptr<TNlLink>> GetLink();
};

class TTclass {
    NO_COPY_CONSTRUCT(TTclass);
    const std::shared_ptr<TQdisc> ParentQdisc;
    const std::shared_ptr<TTclass> ParentTclass;
    const uint32_t Handle;
    std::vector<std::shared_ptr<TNlLink>> GetLink();
    bool Exists(std::shared_ptr<TNlLink> Link);

public:
    TTclass(const std::shared_ptr<TQdisc> qdisc, uint32_t handle) : ParentQdisc(qdisc), Handle(handle) { }
    TTclass(const std::shared_ptr<TTclass> tclass, uint32_t handle) : ParentTclass(tclass), Handle(handle) { }

    TError Create(uint32_t prio, uint32_t rate, uint32_t ceil);
    TError Remove();
    uint32_t GetParent();
    uint32_t GetHandle() { return Handle; }
    TError GetStat(ETclassStat stat, std::map<std::string, uint64_t> &m);
};

class TFilter {
    NO_COPY_CONSTRUCT(TFilter);
    const std::shared_ptr<TQdisc> Parent;
    std::vector<std::shared_ptr<TNlLink>> GetLink();
    bool Exists(std::shared_ptr<TNlLink> Link);

public:
    TFilter(const std::shared_ptr<TQdisc> parent) : Parent(parent) { }
    TError Create();
    TError Remove();
};

#endif
