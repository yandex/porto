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

std::vector<std::shared_ptr<TNlLink>> OpenLinks() {
    std::vector<std::string> devices;
    for (auto &device : config().network().devices())
        devices.push_back(device);

    std::vector<std::shared_ptr<TNlLink>> linkVec;

    auto nl = std::make_shared<TNl>();
    if (!nl)
        throw std::bad_alloc();

    TError error = nl->Connect();
    if (error) {
        L_ERR() << "Can't open link: " << error << std::endl;
        return linkVec;
    }

    if (!devices.size()) {
        error = nl->GetDefaultLink(devices);
        if (error) {
            L_ERR() << "Can't open link: " << error << std::endl;
            return linkVec;
        }
    }

    std::map<std::string, std::string> aliasMap;
    for (auto &alias : config().network().alias())
        aliasMap[alias.iface()] = alias.name();

    for (auto &name : devices) {
        auto l = std::make_shared<TNlLink>(nl, name);
        if (!l)
            throw std::bad_alloc();

        error = l->Load();
        if (error) {
            L_ERR() << "Can't open link: " << error << std::endl;
            return linkVec;
        }

        if (aliasMap.find(name) != aliasMap.end())
            l->SetAlias(aliasMap.at(name));

        linkVec.push_back(l);
    }

    return linkVec;
}
