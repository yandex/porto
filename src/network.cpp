#include <algorithm>
#include <sstream>

#include "network.hpp"
#include "config.hpp"
#include "util/log.hpp"
#include "util/string.hpp"

extern "C" {
#include <netlink/route/link.h>
}

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

TError TNetwork::Destroy() {
    auto lock = ScopedLock();

    auto links = GetLinks();

    L_ACT() << "Removing network..." << std::endl;

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
    PORTO_ASSERT(Links.size() == 0);

    TError error;
    auto lock = ScopedLock();

    error = UpdateInterfaces();
    if (error)
        return error;

    error = OpenLinks(Links);
    if (error)
        return error;

    for (auto link : Links) {
        TError error = PrepareLink(*link);
        if (error)
            return error;
    }

    Qdisc = std::make_shared<TQdisc>(rootHandle, defClass);

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
    TNlClass porto_tclass(rootHandle, TcHandle(TcMajor(rootHandle), PORTO_ROOT_CONTAINER_ID));

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

    if (!porto_tclass.Valid(link, prio, rate, ceil)) {
        (void)porto_tclass.Remove(link);

        TError error = porto_tclass.Create(link, prio, rate, ceil);
        if (error) {
            L_ERR() << "Can't create porto tclass: " << error << std::endl;
            return error;
        }
    }

    return TError::Success();
}

TNetwork::TNetwork() {
    Nl = std::make_shared<TNl>();
    PORTO_ASSERT(Nl != nullptr);
}

TNetwork::~TNetwork() {
}

TError TNetwork::Connect() {
    TError error = Nl->Connect();
    if (!error)
        rtnl = Nl->GetSock();
    return error;
}

TError TNetwork::NetlinkError(int error, const std::string description) {
    return TError(EError::Unknown, description + ": " +
                                   std::string(nl_geterror(error)));
}

void TNetwork::DumpNetlinkObject(const std::string &prefix, void *obj) {
    std::stringstream ss;
    struct nl_dump_params dp = {};

    dp.dp_type = NL_DUMP_DETAILS;
    dp.dp_data = &ss;
    dp.dp_cb = [](struct nl_dump_params *dp, char *buf) {
            auto ss = (std::stringstream *)dp->dp_data;
            *ss << std::string(buf);
    };
    nl_object_dump(OBJ_CAST(obj), &dp);

    L() << "netlink " << prefix << " " << ss.str() << std::endl;
}

TError TNetwork::UpdateInterfaces() {
    struct nl_cache *cache;
    int ret;

    ret = rtnl_link_alloc_cache(rtnl, AF_UNSPEC, &cache);
    if (ret < 0)
        return NetlinkError(ret, "Cannot allocate link cache");

    ifaces.clear();

    for (auto obj = nl_cache_get_first(cache); obj; obj = nl_cache_get_next(obj)) {
        auto link = (struct rtnl_link *)obj;
        int ifindex = rtnl_link_get_ifindex(link);
        int flags = rtnl_link_get_flags(link);
        const char *name = rtnl_link_get_name(link);

        DumpNetlinkObject("link", link);

        if ((flags & IFF_LOOPBACK) || !(flags & IFF_RUNNING))
            continue;

        ifaces.push_back(std::make_pair(std::string(name), ifindex));
    }

    nl_cache_free(cache);
    return TError::Success();
}

TError TNetwork::OpenLinks(std::vector<std::shared_ptr<TNlLink>> &links) {
    std::vector<std::string> devices;
    TError error;

    error = Nl->RefillCache();
    if (error) {
        L_ERR() << "Can't refill link cache: " << error << std::endl;
        return error;
    }

    error = Nl->GetDefaultLink(devices);
    if (error) {
        L_ERR() << "Can't open link: " << error << std::endl;
        return error;
    }

    for (auto &name : devices) {
        auto l = std::make_shared<TNlLink>(Nl, name);
        PORTO_ASSERT(l != nullptr);

        TError error = l->Load();
        if (error) {
            L_ERR() << "Can't open link: " << error << std::endl;
            return error;
        }

        links.push_back(l);
    }

    return TError::Success();
}
