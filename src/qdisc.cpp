#include <algorithm>
#include <sstream>

#include "qdisc.hpp"
#include "config.hpp"
#include "util/log.hpp"
#include "util/string.hpp"

bool TTclass::Exists(std::shared_ptr<TNlLink> link) {
    TNlClass tclass(link, GetParent(), Handle);
    return tclass.Exists();
}

TError TTclass::GetStat(ETclassStat stat, std::map<std::string, uint64_t> &m) {
    for (auto &link : Net->GetLinks()) {
        uint64_t val;
        TNlClass tclass(link, GetParent(), Handle);
        TError error = tclass.GetStat(stat, val);
        if (error)
            return error;

        m[link->GetAlias()] = val;
    }

    return TError::Success();
}

uint32_t TTclass::GetParent() {
    if (ParentQdisc)
        return ParentQdisc->GetHandle();
    else
        return ParentTclass->Handle;
}

void TTclass::Prepare(std::map<std::string, uint64_t> prio,
                       std::map<std::string, uint64_t> rate,
                       std::map<std::string, uint64_t> ceil) {
    L_ACT() << "Prepare tc class 0x" << std::hex << Handle << std::dec << " prio={" << MapToStr(prio) << "} rate={" << MapToStr(rate) << "} ceil={" << MapToStr(ceil) << "}" << std::endl;
    Prio = prio;
    Rate = rate;
    Ceil = ceil;
}

TError TTclass::Create() {
    TError firstError = TError::Success();

    for (auto &link : Net->GetLinks()) {
        auto alias = link->GetAlias();
        auto prio = (Prio.find(alias) != Prio.end()) ? Prio[alias] : Prio["default"];
        auto rate = (Rate.find(alias) != Rate.end()) ? Rate[alias] : Rate["default"];
        auto ceil = (Ceil.find(alias) != Ceil.end()) ? Ceil[alias] : Ceil["default"];

        TNlClass tclass(link, GetParent(), Handle);
        if (tclass.Exists()) {
            if (!tclass.Valid(prio, rate, ceil))
                (void)tclass.Remove();
            else
                continue;
        }

        TError error = tclass.Create(prio, rate, ceil);
        if (!firstError)
            firstError = error;
    }

    return firstError;
}

TError TTclass::Remove() {
    for (auto &link : Net->GetLinks()) {
        if (!Exists(link))
            continue;

        TNlClass tclass(link, GetParent(), Handle);
        TError error = tclass.Remove();
        if (error)
            return error;
    }

    return TError::Success();
}

std::shared_ptr<TNetwork> TQdisc::GetNet() {
    return Net;
}

TError TQdisc::Create() {
    for (auto &link : Net->GetLinks()) {
        TNlHtb qdisc(link, TcRootHandle(), Handle);

        if (qdisc.Valid(DefClass))
            continue;

        (void)qdisc.Remove();

        TError error = qdisc.Create(DefClass);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TQdisc::Remove() {
    for (auto &link : Net->GetLinks()) {
        TNlHtb qdisc(link, TcRootHandle(), Handle);
        TError error = qdisc.Remove();
        if (error)
            return error;
    }

    return TError::Success();
}

class TFilter : public TNonCopyable {
    std::shared_ptr<TNetwork> Net;
    const std::shared_ptr<TQdisc> Parent;
    bool Exists(std::shared_ptr<TNlLink> link);

public:
    TFilter(std::shared_ptr<TNetwork> net, const std::shared_ptr<TQdisc> parent) : Net(net), Parent(parent) { }
    TError Create();
};

bool TFilter::Exists(std::shared_ptr<TNlLink> link) {
    TNlCgFilter filter(link, Parent->GetHandle(), 1);
    return filter.Exists();
}

TError TFilter::Create() {
    for (auto &link : Net->GetLinks()) {
        TNlCgFilter filter(link, Parent->GetHandle(), 1);
        TError error = filter.Create();
        if (error)
            return error;
    }

    return TError::Success();
}

TError TNetwork::Destroy() {
    auto lock = ScopedLock();

    L_ACT() << "Removing network..." << std::endl;

    if (Tclass) {
        TError error = Tclass->Remove();
        if (error)
            return error;
        Tclass = nullptr;
    }

    if (Qdisc) {
        TError error = Qdisc->Remove();
        if (error)
            return error;
        Qdisc = nullptr;
    }

    return TError::Success();
}

TError TNetwork::Prepare() {
    PORTO_ASSERT(Qdisc == nullptr);
    PORTO_ASSERT(Tclass == nullptr);
    PORTO_ASSERT(Filter == nullptr);
    PORTO_ASSERT(Links.size() == 0);

    auto lock = ScopedLock();

    TError error = OpenLinks(Links);
    if (error)
        return error;

    for (auto link : Links) {
        TError error = PrepareLink(link);
        if (error)
            return error;
    }

    Qdisc = std::make_shared<TQdisc>(shared_from_this(), rootHandle, defClass);
    Filter = std::make_shared<TFilter>(shared_from_this(), Qdisc);
    Tclass = std::make_shared<TTclass>(shared_from_this(), Qdisc, defClass);

    return TError::Success();
}

TError TNetwork::Update() {
    L() << "Update network" << std::endl;

    std::vector<std::shared_ptr<TNlLink>> newLinks;

    auto net_lock = ScopedLock();

    TError error = OpenLinks(newLinks);
    if (error)
        return error;

    for (auto link : newLinks) {
        auto i = std::find_if(Links.begin(), Links.end(),
                              [link](std::shared_ptr<TNlLink> i) {
                                 return i->GetAlias() == link->GetAlias();
                              });

        if (i == Links.end()) {
            L() << "Found new link: " << link->GetAlias() << std::endl;
            TError error = PrepareLink(link);
            if (error)
                return error;
        } else {
            L() << "Found existing link: " << link->GetAlias() << std::endl;
            TError error = link->RefillClassCache();
            if (error)
                return error;
        }
    }

    Links = newLinks;
    return TError::Success();
}

TError TNetwork::PrepareLink(std::shared_ptr<TNlLink> link) {
    // 1:0 qdisc
    // 1:2 default class    1:1 root class
    // (unclassified        1:3 container a, 1:4 container b
    //          traffic)    1:5 container a/c

    L() << "Prepare link " << link->GetAlias() << " " << link->GetIndex() << std::endl;

    TNlHtb qdisc(link, TcRootHandle(), rootHandle);

    if (!qdisc.Valid(defClass)) {
        (void)qdisc.Remove();
        TError error = qdisc.Create(defClass);
        if (error) {
            L_ERR() << "Can't create root qdisc: " << error << std::endl;
            return error;
        }
    }

    TNlCgFilter filter(link, rootHandle, 1);
    if (filter.Exists())
        (void)filter.Remove();

    TError error = filter.Create();
    if (error) {
        L_ERR() << "Can't create tc filter: " << error << std::endl;
        return error;
    }

    TNlClass tclass(link, rootHandle, defClass);

    uint64_t prio, rate, ceil;
    prio = config().network().default_prio();
    rate = config().network().default_max_guarantee();
    ceil = config().network().default_limit();

    if (!tclass.Valid(prio, rate, ceil)) {
        (void)tclass.Remove();

        TError error = tclass.Create(prio, rate, ceil);
        if (error) {
            L_ERR() << "Can't create default tclass: " << error << std::endl;
            return error;
        }
    }

    return TError::Success();
}

TNetwork::TNetwork() {
    Nl = std::make_shared<TNl>();
    if (!Nl)
        throw std::bad_alloc();
}

TError TNetwork::Connect(int fd) {
    return Nl->Connect(fd);
}

TError TNetwork::OpenLinks(std::vector<std::shared_ptr<TNlLink>> &links) {
    std::vector<std::string> devices;
    for (auto &device : config().network().devices())
        devices.push_back(device);

    TError error = Nl->RefillCache();
    if (error) {
        L_ERR() << "Can't refill link cache: " << error << std::endl;
        return error;
    }

    if (!devices.size()) {
        TError error = Nl->GetDefaultLink(devices);
        if (error) {
            L_ERR() << "Can't open link: " << error << std::endl;
            return error;
        }
    }

    std::map<std::string, std::string> aliasMap;
    for (auto &alias : config().network().alias())
        aliasMap[alias.iface()] = alias.name();

    for (auto &name : devices) {
        auto l = std::make_shared<TNlLink>(Nl, name);
        if (!l)
            throw std::bad_alloc();

        TError error = l->Load();
        if (error) {
            L_ERR() << "Can't open link: " << error << std::endl;
            return error;
        }

        if (aliasMap.find(name) != aliasMap.end())
            l->SetAlias(aliasMap.at(name));

        links.push_back(l);
    }

    return TError::Success();
}
