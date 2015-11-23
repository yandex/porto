#include <algorithm>
#include <sstream>

#include "network.hpp"
#include "config.hpp"
#include "util/log.hpp"
#include "util/string.hpp"

bool TTclass::Exists(const TNlLink &link) {
    TNlClass tclass(GetParent(), Handle);
    return tclass.Exists(link);
}

TError TTclass::GetStat(const std::vector<std::shared_ptr<TNlLink>> &links,
                        ETclassStat stat, std::map<std::string, uint64_t> &m) {
    for (auto &link : links) {
        uint64_t val;
        TNlClass tclass(GetParent(), Handle);
        TError error = tclass.GetStat(*link, stat, val);
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

TError TTclass::Create(TNlLink &link) {
    auto alias = link.GetAlias();
    auto prio = (Prio.find(alias) != Prio.end()) ? Prio[alias] : Prio["default"];
    auto rate = (Rate.find(alias) != Rate.end()) ? Rate[alias] : Rate["default"];
    auto ceil = (Ceil.find(alias) != Ceil.end()) ? Ceil[alias] : Ceil["default"];

    TNlClass tclass(GetParent(), Handle);
    if (tclass.Exists(link)) {
        if (tclass.Valid(link, prio, rate, ceil))
            return TError::Success();

        (void)tclass.Remove(link);
    }
    return tclass.Create(link, prio, rate, ceil);
}

TError TTclass::Remove(TNlLink &link) {
    TNlClass tclass(GetParent(), Handle);
    return tclass.Remove(link);
}

TError TQdisc::Create(const TNlLink &link) {
    TNlHtb qdisc(TcRootHandle(), Handle);

    if (!qdisc.Valid(link, DefClass)) {
        (void)qdisc.Remove(link);
        return qdisc.Create(link, DefClass);
    }

    return TError::Success();
}

TError TQdisc::Remove(const TNlLink &link) {
    TNlHtb qdisc(TcRootHandle(), Handle);
    return qdisc.Remove(link);
}

class TFilter : public TNonCopyable {
    const std::shared_ptr<TQdisc> Parent;
    bool Exists(const TNlLink &link);

public:
    TFilter(const std::shared_ptr<TQdisc> parent) : Parent(parent) { }
    TError Create(TNlLink &link);
};

bool TFilter::Exists(const TNlLink &link) {
    TNlCgFilter filter(Parent->GetHandle(), 1);
    return filter.Exists(link);
}

TError TFilter::Create(TNlLink &link) {
    TNlCgFilter filter(Parent->GetHandle(), 1);
    return filter.Create(link);
}

TError TNetwork::Destroy() {
    auto lock = ScopedLock();

    auto links = GetLinks();

    L_ACT() << "Removing network..." << std::endl;

    if (Tclass) {
        for (const auto &link : links) {
            TError error = Tclass->Remove(*link);
            if (error)
                return error;
        }
        Tclass = nullptr;
    }

    if (Qdisc) {
        for (const auto &link : links) {
            TError error = Qdisc->Remove(*link);
            if (error)
                return error;
        }
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
        TError error = PrepareLink(*link);
        if (error)
            return error;
    }

    Qdisc = std::make_shared<TQdisc>(rootHandle, defClass);
    Filter = std::make_shared<TFilter>(Qdisc);
    Tclass = std::make_shared<TTclass>(Qdisc, defClass);

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
            TError error = PrepareLink(*link);
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

TError TNetwork::PrepareLink(TNlLink &link) {
    // 1:0 qdisc
    // 1:2 default class    1:1 root class
    // (unclassified        1:3 container a, 1:4 container b
    //          traffic)    1:5 container a/c

    L() << "Prepare link " << link.GetAlias() << " " << link.GetIndex() << std::endl;

    TNlHtb qdisc(TcRootHandle(), rootHandle);

    if (!qdisc.Valid(link, defClass)) {
        (void)qdisc.Remove(link);
        TError error = qdisc.Create(link, defClass);
        if (error) {
            L_ERR() << "Can't create root qdisc: " << error << std::endl;
            return error;
        }
    }

    TNlCgFilter filter(rootHandle, 1);
    if (filter.Exists(link))
        (void)filter.Remove(link);

    TError error = filter.Create(link);
    if (error) {
        L_ERR() << "Can't create tc filter: " << error << std::endl;
        return error;
    }

    TNlClass tclass(rootHandle, defClass);

    uint64_t prio, rate, ceil;
    prio = config().network().default_prio();
    rate = config().network().default_max_guarantee();
    ceil = config().network().default_limit();

    if (!tclass.Valid(link, prio, rate, ceil)) {
        (void)tclass.Remove(link);

        TError error = tclass.Create(link, prio, rate, ceil);
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
