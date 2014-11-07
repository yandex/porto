#include "qdisc.hpp"
#include "util/log.hpp"

std::vector<std::shared_ptr<TNlLink>> TTclass::GetLink() {
    if (ParentQdisc)
        return ParentQdisc->GetLink();
    else
        return ParentTclass->GetLink();
}

bool TTclass::Exists(std::shared_ptr<TNlLink> Link) {
    TNlClass tclass(Link, GetParent(), Handle);
    return tclass.Exists();
}

TError TTclass::GetStat(ETclassStat stat, std::map<std::string, uint64_t> &m) {
    if (!config().network().enabled())
        return TError(EError::Unknown, "Network support is disabled");

    for (auto &link : GetLink()) {
        uint64_t val;
        TNlClass tclass(link, GetParent(), Handle);
        TError error = tclass.GetStat(stat, val);
        if (error)
            return error;

        m[link->GetName()] = val;
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

    for (auto &link : GetLink()) {
        if (prio.find(link->GetName()) == prio.end())
            return TError(EError::Unknown, "Unknown interface in net_priority");

        if (rate.find(link->GetName()) == rate.end())
            return TError(EError::Unknown, "Unknown interface in net_guarantee");

        if (ceil.find(link->GetName()) == ceil.end())
            return TError(EError::Unknown, "Unknown interface in net_limit");

        TNlClass tclass(link, GetParent(), Handle);
        TError error = tclass.Create(prio[link->GetName()],
                                     rate[link->GetName()],
                                     ceil[link->GetName()]);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TTclass::Remove() {
    if (!config().network().enabled())
        return TError::Success();

    for (auto &link : GetLink()) {
        if (!Exists(link))
            return TError::Success();

        TNlClass tclass(link, GetParent(), Handle);
        TError error = tclass.Remove();
        if (error)
            return error;
    }

    return TError::Success();
}

std::vector<std::shared_ptr<TNlLink>> TQdisc::GetLink() {
    return Link;
}

TError TQdisc::Create() {
    if (!config().network().enabled())
        return TError::Success();

    for (auto &link : GetLink()) {
        TNlHtb qdisc(link, TcRootHandle(), Handle);
        TError error = qdisc.Create(DefClass);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TQdisc::Remove() {
    if (!config().network().enabled())
        return TError::Success();

    for (auto &link : GetLink()) {
        TNlHtb qdisc(link, TcRootHandle(), Handle);
        TError error = qdisc.Remove();
        if (error)
            return error;
    }

    return TError::Success();
}

std::vector<std::shared_ptr<TNlLink>> TFilter::GetLink() {
    return Parent->GetLink();
}

bool TFilter::Exists(std::shared_ptr<TNlLink> Link) {
    TNlCgFilter filter(Link, Parent->GetHandle(), 1);
    return filter.Exists();
}

TError TFilter::Create() {
    if (!config().network().enabled())
        return TError::Success();

    for (auto &link : GetLink()) {
        TNlCgFilter filter(link, Parent->GetHandle(), 1);
        TError error = filter.Create();
        if (error)
            return error;
    }

    return TError::Success();
}

TError TFilter::Remove() {
    if (!config().network().enabled())
        return TError::Success();

    for (auto &link : GetLink()) {
        if (!Exists(link))
            return TError::Success();

        TNlCgFilter filter(link, Parent->GetHandle(), 1);
        TError error = filter.Remove();
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
        TLogger::LogError(error, "Couldn't open link!");
        return linkVec;
    }

    if (!devices.size()) {
        error = nl->GetDefaultLink(devices);
        if (error) {
            TLogger::LogError(error, "Couldn't open link!");
            return linkVec;
        }
    }

    for (auto &name : devices) {
        auto l = std::make_shared<TNlLink>(nl, name);
        if (!l)
            throw std::bad_alloc();

        error = l->Load();
        if (error) {
            TLogger::LogError(error, "Couldn't open link!");
            return linkVec;
        }

        linkVec.push_back(l);
    }

    return linkVec;
}
