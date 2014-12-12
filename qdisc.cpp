#include <algorithm>

#include "qdisc.hpp"
#include "config.hpp"
#include "util/log.hpp"

bool TTclass::Exists(std::shared_ptr<TNlLink> link) {
    TNlClass tclass(link, GetParent(), Handle);
    return tclass.Exists();
}

TError TTclass::GetStat(ETclassStat stat, std::map<std::string, uint64_t> &m) {
    if (!config().network().enabled())
        return TError(EError::Unknown, "Network support is disabled");

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
    if (!config().network().enabled())
        return 0;

    if (ParentQdisc)
        return ParentQdisc->GetHandle();
    else
        return ParentTclass->Handle;
}

void TTclass::Prepare(std::map<std::string, uint64_t> prio,
                       std::map<std::string, uint64_t> rate,
                       std::map<std::string, uint64_t> ceil) {
    Prio = prio;
    Rate = rate;
    Ceil = ceil;
}

TError TTclass::Create(bool fallback) {
    if (!config().network().enabled())
        return TError::Success();

    for (auto &link : Net->GetLinks()) {
        if (Prio.find(link->GetAlias()) == Prio.end()) {
            if (fallback)
                Prio[link->GetAlias()] = config().container().default_cpu_prio();
            else
                return TError(EError::Unknown, "Unknown interface in net_priority");
        }

        if (Rate.find(link->GetAlias()) == Rate.end()) {
            if (fallback)
                Rate[link->GetAlias()] = config().network().default_guarantee();
            else
                return TError(EError::Unknown, "Unknown interface in net_guarantee");
        }

        if (Ceil.find(link->GetAlias()) == Ceil.end()) {
            if (fallback)
                Ceil[link->GetAlias()] = config().network().default_limit();
            else
                return TError(EError::Unknown, "Unknown interface in net_limit");
        }

        if (config().network().dynamic_ifaces() &&
            ParentTclass && !ParentTclass->Exists(link)) {
            TError error = ParentTclass->Create(true);
            if (error) {
                L_ERR() << "Can't create parent tc class: " << error << std::endl;
                return error;
            }
        }

        TNlClass tclass(link, GetParent(), Handle);
        TError error = tclass.Create(Prio[link->GetAlias()],
                                     Rate[link->GetAlias()],
                                     Ceil[link->GetAlias()]);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TTclass::Remove() {
    if (!config().network().enabled())
        return TError::Success();

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
    if (!config().network().enabled())
        return TError::Success();

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
    if (!config().network().enabled())
        return TError::Success();

    for (auto &link : Net->GetLinks()) {
        TNlHtb qdisc(link, TcRootHandle(), Handle);
        TError error = qdisc.Remove();
        if (error)
            return error;
    }

    return TError::Success();
}

bool TFilter::Exists(std::shared_ptr<TNlLink> link) {
    TNlCgFilter filter(link, Parent->GetHandle(), 1);
    return filter.Exists();
}

TError TFilter::Create() {
    if (!config().network().enabled())
        return TError::Success();

    for (auto &link : Net->GetLinks()) {
        TNlCgFilter filter(link, Parent->GetHandle(), 1);
        TError error = filter.Create();
        if (error)
            return error;
    }

    return TError::Success();
}

TError TNetwork::Destroy() {
    L() << "Removing network..." << std::endl;

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

TNetwork::~TNetwork() {
    (void)Destroy();
}

TError TNetwork::Prepare() {
#ifdef PORTOD
    PORTO_ASSERT(Qdisc == nullptr);
    PORTO_ASSERT(Tclass == nullptr);
    PORTO_ASSERT(Filter == nullptr);
    PORTO_ASSERT(Links.size() == 0);
#endif

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
    if (!config().network().dynamic_ifaces())
        return TError::Success();

    std::vector<std::shared_ptr<TNlLink>> newLinks;

    TError error = OpenLinks(newLinks);
    if (error)
        return error;

    for (auto link : newLinks) {
        auto i = std::find_if(Links.begin(), Links.end(),
                              [link](std::shared_ptr<TNlLink> i) {
                                 return i->GetAlias() == link->GetAlias();
                              });

        if (i == Links.end()) {
            TError error = PrepareLink(link);
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

    TNlHtb qdisc(link, TcRootHandle(), rootHandle);

    if (qdisc.Valid(defClass))
        return TError::Success();

    (void)qdisc.Remove();

    TError error = qdisc.Create(defClass);
    if (error) {
        L_ERR() << "Can't create root qdisc: " << error << std::endl;
        return error;
    }

    TNlCgFilter filter(link, rootHandle, 1);
    error = filter.Create();
    if (error) {
        L_ERR() << "Can't create tc filter: " << error << std::endl;
        return error;
    }

    TNlClass tclass(link, rootHandle, defClass);

    uint64_t prio, rate, ceil;
    prio = config().container().default_cpu_prio();
    rate = config().network().default_guarantee();
    ceil = config().network().default_limit();

    error = tclass.Create(prio, rate, ceil);
    if (error) {
        L_ERR() << "Can't create default tclass: " << error << std::endl;
    }

    return TError::Success();
}

TError TNetwork::OpenLinks(std::vector<std::shared_ptr<TNlLink>> &links) {
    std::vector<std::string> devices;
    for (auto &device : config().network().devices())
        devices.push_back(device);

    if (!Nl) {
        Nl = std::make_shared<TNl>();
        if (!Nl)
            throw std::bad_alloc();
    }

    TError error = Nl->Connect();
    if (error) {
        L_ERR() << "Can't open link: " << error << std::endl;
        return error;
    }

    if (!devices.size()) {
        error = Nl->GetDefaultLink(devices);
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

        error = l->Load();
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
