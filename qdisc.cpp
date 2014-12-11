#include "qdisc.hpp"
#include "config.hpp"
#include "util/log.hpp"

std::vector<std::shared_ptr<TNlLink>> TTclass::GetLinks() {
    if (ParentQdisc)
        return ParentQdisc->GetLinks();
    else
        return ParentTclass->GetLinks();
}

bool TTclass::Exists(std::shared_ptr<TNlLink> link) {
    TNlClass tclass(link, GetParent(), Handle);
    return tclass.Exists();
}

TError TTclass::GetStat(ETclassStat stat, std::map<std::string, uint64_t> &m) {
    if (!config().network().enabled())
        return TError(EError::Unknown, "Network support is disabled");

    for (auto &link : GetLinks()) {
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

TError TTclass::Create(std::map<std::string, uint64_t> prio, std::map<std::string, uint64_t> rate, std::map<std::string, uint64_t> ceil) {
    if (!config().network().enabled())
        return TError::Success();

    for (auto &link : GetLinks()) {
        if (prio.find(link->GetAlias()) == prio.end())
            return TError(EError::Unknown, "Unknown interface in net_priority");

        if (rate.find(link->GetAlias()) == rate.end())
            return TError(EError::Unknown, "Unknown interface in net_guarantee");

        if (ceil.find(link->GetAlias()) == ceil.end())
            return TError(EError::Unknown, "Unknown interface in net_limit");

        TNlClass tclass(link, GetParent(), Handle);
        TError error = tclass.Create(prio[link->GetAlias()],
                                     rate[link->GetAlias()],
                                     ceil[link->GetAlias()]);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TTclass::Remove() {
    if (!config().network().enabled())
        return TError::Success();

    for (auto &link : GetLinks()) {
        if (!Exists(link))
            continue;

        TNlClass tclass(link, GetParent(), Handle);
        TError error = tclass.Remove();
        if (error)
            return error;
    }

    return TError::Success();
}

std::vector<std::shared_ptr<TNlLink>> TQdisc::GetLinks() {
    return Links;
}

TError TQdisc::Create() {
    if (!config().network().enabled())
        return TError::Success();

    for (auto &link : GetLinks()) {
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

    for (auto &link : GetLinks()) {
        TNlHtb qdisc(link, TcRootHandle(), Handle);
        TError error = qdisc.Remove();
        if (error)
            return error;
    }

    return TError::Success();
}

std::vector<std::shared_ptr<TNlLink>> TFilter::GetLinks() {
    return Parent->GetLinks();
}

bool TFilter::Exists(std::shared_ptr<TNlLink> link) {
    TNlCgFilter filter(link, Parent->GetHandle(), 1);
    return filter.Exists();
}

TError TFilter::Create() {
    if (!config().network().enabled())
        return TError::Success();

    for (auto &link : GetLinks()) {
        TNlCgFilter filter(link, Parent->GetHandle(), 1);
        TError error = filter.Create();
        if (error)
            return error;
    }

    return TError::Success();
}

TNetwork::TNetwork() {
}

TNetwork::~TNetwork() {
    if (Tclass) {
        TError error = Tclass->Remove();
        if (error)
            L_ERR() << "Can't remove default tc class: " << error << std::endl;
    }

    if (Qdisc) {
        TError error = Qdisc->Remove();
        if (error)
            L_ERR() << "Can't remove tc qdisc: " << error << std::endl;
    }
}

TError TNetwork::Prepare() {
    Links.clear();
    Qdisc = nullptr;
    Tclass = nullptr;
    Filter = nullptr;

    TError error = OpenLinks(Links);
    if (error)
        return error;

    return PrepareTc();
}

TError TNetwork::Update() {
    // TODO:

    return TError::Success();
}

TError TNetwork::PrepareTc() {
    // 1:0 qdisc
    // 1:2 default class    1:1 root class
    // (unclassified        1:3 container a, 1:4 container b
    //          traffic)    1:5 container a/c

    uint32_t defHandle = TcHandle(1, 2);
    uint32_t rootHandle = TcHandle(1, 0);

    Qdisc = std::make_shared<TQdisc>(Links, rootHandle, defHandle);
    TError error = Qdisc->Create();
    if (error) {
        L_ERR() << "Can't create root qdisc: " << error << std::endl;
        return error;
    }

    Filter = std::make_shared<TFilter>(Qdisc);
    error = Filter->Create();
    if (error) {
        L_ERR() << "Can't create tc filter: " << error << std::endl;
        return error;
    }

    Tclass = std::make_shared<TTclass>(Qdisc, defHandle);

    std::map<std::string, uint64_t> prio, rate, ceil;
    for (auto &link : Links) {
        prio[link->GetAlias()] = config().container().default_cpu_prio();
        rate[link->GetAlias()] = config().network().default_guarantee();
        ceil[link->GetAlias()] = config().network().default_limit();
    }

    error = Tclass->Create(prio, rate, ceil);
    if (error) {
        L_ERR() << "Can't create default tclass: " << error << std::endl;
        return error;
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
