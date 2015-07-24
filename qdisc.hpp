#pragma once

#include <memory>
#include <string>
#include <mutex>

#include "common.hpp"
#include "util/netlink.hpp"
#include "util/locks.hpp"

class TNetwork;
class TFilter;

class TQdisc : public TNonCopyable {
    std::shared_ptr<TNetwork> Net;
    const uint32_t Handle;
    const uint32_t DefClass;

public:
    TQdisc(std::shared_ptr<TNetwork> net, uint32_t handle, uint32_t defClass) : Net(net), Handle(handle), DefClass(defClass) { }

    TError Create();
    TError Remove();
    uint32_t GetHandle() { return Handle; }
    std::shared_ptr<TNetwork> GetNet();
};

class TTclass : public TNonCopyable {
    std::shared_ptr<TNetwork> Net;
    const std::shared_ptr<TQdisc> ParentQdisc;
    const std::shared_ptr<TTclass> ParentTclass;
    const uint32_t Handle;
    bool Exists(std::shared_ptr<TNlLink> link);
    uint32_t GetParent();

    std::map<std::string, uint64_t> Prio;
    std::map<std::string, uint64_t> Rate;
    std::map<std::string, uint64_t> Ceil;

public:
    TTclass(std::shared_ptr<TNetwork> net, const std::shared_ptr<TQdisc> qdisc, uint32_t handle) : Net(net), ParentQdisc(qdisc), Handle(handle) { }
    TTclass(std::shared_ptr<TNetwork> net, const std::shared_ptr<TTclass> tclass, uint32_t handle) : Net(net), ParentTclass(tclass), Handle(handle) { }

    void Prepare(std::map<std::string, uint64_t> prio, std::map<std::string, uint64_t> rate, std::map<std::string, uint64_t> ceil);
    TError Create();
    TError Remove();
    uint32_t GetHandle() { return Handle; }
    TError GetStat(ETclassStat stat, std::map<std::string, uint64_t> &m);
};

class TNetwork : public std::enable_shared_from_this<TNetwork>,
                 public TNonCopyable,
                 public TLockable {
    std::shared_ptr<TNl> Nl;
    std::vector<std::shared_ptr<TNlLink>> Links;
    std::shared_ptr<TQdisc> Qdisc;
    std::shared_ptr<TTclass> Tclass;
    std::shared_ptr<TFilter> Filter;

    const uint32_t defClass = TcHandle(1, 2);
    const uint32_t rootHandle = TcHandle(1, 0);

    TError PrepareLink(std::shared_ptr<TNlLink> link);

public:
    TError Prepare();
    TError Update();
    // OpenLinks doesn't lock TNetwork
    TError OpenLinks(std::vector<std::shared_ptr<TNlLink>> &links);
    TError Destroy();

    std::shared_ptr<TNl> GetNl() { return Nl; }
    std::vector<std::shared_ptr<TNlLink>> GetLinks() { return Links; }
    std::shared_ptr<TQdisc> GetQdisc() { return Qdisc; }
    bool Empty() { return Links.size() == 0; }
};
