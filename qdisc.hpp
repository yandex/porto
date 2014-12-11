#ifndef __QDISC_H__
#define __QDISC_H__

#include <memory>
#include <string>

#include "common.hpp"
#include "util/netlink.hpp"

// TODO: Links -> Net

class TQdisc {
    NO_COPY_CONSTRUCT(TQdisc);
    std::vector<std::shared_ptr<TNlLink>> Links;
    const uint32_t Handle;
    const uint32_t DefClass;

public:
    TQdisc(const std::vector<std::shared_ptr<TNlLink>> &links, uint32_t handle, uint32_t defClass) : Links(links), Handle(handle), DefClass(defClass) { }

    TError Create();
    TError Remove();
    uint32_t GetHandle() { return Handle; }
    std::vector<std::shared_ptr<TNlLink>> GetLinks();
};

class TTclass {
    NO_COPY_CONSTRUCT(TTclass);
    const std::shared_ptr<TQdisc> ParentQdisc;
    const std::shared_ptr<TTclass> ParentTclass;
    const uint32_t Handle;
    std::vector<std::shared_ptr<TNlLink>> GetLinks();
    bool Exists(std::shared_ptr<TNlLink> link);

public:
    TTclass(const std::shared_ptr<TQdisc> qdisc, uint32_t handle) : ParentQdisc(qdisc), Handle(handle) { }
    TTclass(const std::shared_ptr<TTclass> tclass, uint32_t handle) : ParentTclass(tclass), Handle(handle) { }

    TError Create(std::map<std::string, uint64_t> prio, std::map<std::string, uint64_t> rate, std::map<std::string, uint64_t> ceil);
    TError Remove();
    uint32_t GetParent();
    uint32_t GetHandle() { return Handle; }
    TError GetStat(ETclassStat stat, std::map<std::string, uint64_t> &m);
};

class TFilter {
    NO_COPY_CONSTRUCT(TFilter);
    const std::shared_ptr<TQdisc> Parent;
    std::vector<std::shared_ptr<TNlLink>> GetLinks();
    bool Exists(std::shared_ptr<TNlLink> link);

public:
    TFilter(const std::shared_ptr<TQdisc> parent) : Parent(parent) { }
    TError Create();
};

class TNetwork {
    NO_COPY_CONSTRUCT(TNetwork);

    std::shared_ptr<TNl> Nl;
    std::vector<std::shared_ptr<TNlLink>> Links;
    std::shared_ptr<TQdisc> Qdisc;
    std::shared_ptr<TTclass> Tclass;
    std::shared_ptr<TFilter> Filter;

    TError PrepareTc();

public:
    TNetwork();
    ~TNetwork();
    TError Prepare();
    TError Update();
    TError OpenLinks(std::vector<std::shared_ptr<TNlLink>> &links);

    std::shared_ptr<TNl> GetNl() { return Nl; }
    std::vector<std::shared_ptr<TNlLink>> GetLinks() { return Links; }
    std::shared_ptr<TQdisc> GetQdisc() { return Qdisc; }
    std::shared_ptr<TTclass> GetTclass() { return Tclass; }
    std::shared_ptr<TFilter> GetFilter() { return Filter; }
    bool Empty() { return Links.size() == 0; }
};

#endif
