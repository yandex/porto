#include <algorithm>
#include <sstream>

#include "network.hpp"
#include "config.hpp"
#include "util/log.hpp"
#include "util/string.hpp"

extern "C" {
#include <netlink/route/link.h>
#include <netlink/route/tc.h>
#include <netlink/route/class.h>
#include <netlink/route/qdisc/htb.h>
}

TError TNetwork::Destroy() {
    auto lock = ScopedLock();
    TError error;

    L_ACT() << "Removing network..." << std::endl;

    for (auto &iface: ifaces) {
        TNlLink link(Nl, iface.first);

        error = link.Load();
        if (error) {
            L_ERR() << "Cannot open link: " << error << std::endl;
            continue;
        }

        TNlHtb htb(TC_H_ROOT, TC_HANDLE(ROOT_TC_MAJOR, ROOT_TC_MINOR));
        error = htb.Remove(link);
        if (error)
            L_ERR() << "Cannot remove htb: " << error << std::endl;
    }

    return TError::Success();
}

TError TNetwork::Prepare() {
    TError error;
    auto lock = ScopedLock();

    error = UpdateInterfaces();
    if (error)
        return error;

    for (auto iface: ifaces) {
        error = PrepareLink(iface.second, iface.first);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TNetwork::PrepareLink(int index, std::string name) {
    TError error;

    //
    // 1:0 qdisc
    //  |
    // 1:1 / class
    //  |
    //  +- 1:2 default class
    //  |
    //  +- 1:3 /porto class
    //      |
    //      +- 1:4 container a
    //      |   |
    //      |   +- 1:5 container a/b
    //      |
    //      +- 1:6 container b
    //

    L() << "Prepare link " << name << " " << index << std::endl;

    TNlLink link(Nl, name);
    error = link.Load();
    if (error) {
        L_ERR() << "Can't open link: " << error << std::endl;
        return error;
    }

    TNlHtb qdisc(TC_H_ROOT, TC_HANDLE(ROOT_TC_MAJOR, ROOT_TC_MINOR));

    if (!qdisc.Valid(link, TC_HANDLE(ROOT_TC_MAJOR, DEFAULT_TC_MINOR))) {
        (void)qdisc.Remove(link);
        TError error = qdisc.Create(link, TC_HANDLE(ROOT_TC_MAJOR, DEFAULT_TC_MINOR));
        if (error) {
            L_ERR() << "Can't create root qdisc: " << error << std::endl;
            return error;
        }
    }

    TNlCgFilter filter(TC_HANDLE(ROOT_TC_MAJOR, ROOT_TC_MINOR), 1);
    if (filter.Exists(link))
        (void)filter.Remove(link);

    error = filter.Create(link);
    if (error) {
        L_ERR() << "Can't create tc filter: " << error << std::endl;
        return error;
    }

    uint64_t prio = config().network().default_prio();
    uint64_t rate = config().network().default_max_guarantee();
    uint64_t ceil = config().network().default_limit();

    error = AddTrafficClass(index,
                            TC_HANDLE(ROOT_TC_MAJOR, ROOT_TC_MINOR),
                            TC_HANDLE(ROOT_TC_MAJOR, ROOT_CONTAINER_ID),
                            prio, rate, ceil);
    if (error) {
        L_ERR() << "Can't create root tclass: " << error << std::endl;
        return error;
    }

    error = AddTrafficClass(index,
                            TC_HANDLE(ROOT_TC_MAJOR, ROOT_CONTAINER_ID),
                            TC_HANDLE(ROOT_TC_MAJOR, DEFAULT_TC_MINOR),
                            prio, rate, ceil);
    if (error) {
        L_ERR() << "Can't create default tclass: " << error << std::endl;
        return error;
    }

    error = AddTrafficClass(index,
                            TC_HANDLE(ROOT_TC_MAJOR, ROOT_CONTAINER_ID),
                            TC_HANDLE(ROOT_TC_MAJOR, PORTO_ROOT_CONTAINER_ID),
                            prio, rate, ceil);
    if (error) {
        L_ERR() << "Can't create porto tclass: " << error << std::endl;
        return error;
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

TError TNetwork::GetTrafficCounters(int minor, ETclassStat stat,
                                    std::map<std::string, uint64_t> &result) {
    uint32_t handle = TC_HANDLE(ROOT_TC_MAJOR, minor);
    rtnl_tc_stat rtnlStat;

    switch (stat) {
    case ETclassStat::Packets:
        rtnlStat = RTNL_TC_PACKETS;
        break;
    case ETclassStat::Bytes:
        rtnlStat = RTNL_TC_BYTES;
        break;
    case ETclassStat::Drops:
        rtnlStat = RTNL_TC_DROPS;
        break;
    case ETclassStat::Overlimits:
        rtnlStat = RTNL_TC_OVERLIMITS;
        break;
    case ETclassStat::BPS:
        rtnlStat = RTNL_TC_RATE_BPS;
        break;
    case ETclassStat::PPS:
        rtnlStat = RTNL_TC_RATE_PPS;
        break;
    default:
        return TError(EError::Unknown, "Unsupported netlink statistics");
    }

    for (auto iface: ifaces) {
        struct nl_cache *cache;
        struct rtnl_class *cls;

        /* TODO optimize this stuff */
        int ret = rtnl_class_alloc_cache(rtnl, iface.second, &cache);
        if (ret < 0)
            return NetlinkError(ret, "Cannot allocate class cache");

        cls = rtnl_class_get(cache, iface.second, handle);
        if (!cls) {
            nl_cache_free(cache);
            return TError(EError::Unknown, "Cannot find class statistics for " + iface.first);
        }

        result[iface.first] = rtnl_tc_get_stat(TC_CAST(cls), rtnlStat);
        rtnl_class_put(cls);
        nl_cache_free(cache);
    }

    return TError::Success();
}

TError TNetwork::AddTrafficClass(int ifIndex, uint32_t parent, uint32_t handle,
                                 uint64_t prio, uint64_t rate, uint64_t ceil) {
    struct rtnl_class *cls;
    TError error;
    int ret;

    cls = rtnl_class_alloc();
    if (!cls)
        return TError(EError::Unknown, "Cannot allocate rtnl_class object");

    rtnl_tc_set_ifindex(TC_CAST(cls), ifIndex);
    rtnl_tc_set_parent(TC_CAST(cls), parent);
    rtnl_tc_set_handle(TC_CAST(cls), handle);

    ret = rtnl_tc_set_kind(TC_CAST(cls), "htb");
    if (ret < 0) {
        error = NetlinkError(ret, "Cannot set HTB class");
        goto free_class;
    }

    /*
     * TC doesn't allow to set 0 rate, but Porto does (because we call them
     * net_guarantee). So, just map 0 to 1, minimal valid guarantee.
     */
    rtnl_htb_set_rate(cls, rate ?: 1);

    if (prio)
        rtnl_htb_set_prio(cls, prio);

    if (ceil)
        rtnl_htb_set_ceil(cls, ceil);

    rtnl_htb_set_quantum(cls, 10000);

    /*
       rtnl_htb_set_rbuffer(tclass, burst);
       rtnl_htb_set_cbuffer(tclass, cburst);
       */

    DumpNetlinkObject("add", cls);
    ret = rtnl_class_add(rtnl, cls, NLM_F_CREATE | NLM_F_REPLACE);
    if (ret < 0)
        error = NetlinkError(ret, "Cannot add traffic class to " + std::to_string(ifIndex));

free_class:
    rtnl_class_put(cls);
    return error;
}

TError TNetwork::DelTrafficClass(int ifIndex, uint32_t handle) {
    struct rtnl_class *cls;
    TError error;
    int ret;

    cls = rtnl_class_alloc();
    if (!cls)
        return TError(EError::Unknown, "Cannot allocate rtnl_class object");

    rtnl_tc_set_ifindex(TC_CAST(cls), ifIndex);
    rtnl_tc_set_handle(TC_CAST(cls), handle);

    DumpNetlinkObject("del", cls);
    ret = rtnl_class_delete(rtnl, cls);

    /* If busy -> remove recursively */
    if (ret == -NLE_BUSY) {
        std::vector<uint32_t> handles({handle});
        struct nl_cache *cache;

        ret = rtnl_class_alloc_cache(rtnl, ifIndex, &cache);
        if (ret < 0) {
            error = NetlinkError(ret, "Cannot allocate class cache");
            goto out;
        }

        for (int i = 0; i < (int)handles.size(); i++) {
            for (auto obj = nl_cache_get_first(cache); obj;
                      obj = nl_cache_get_next(obj)) {
                uint32_t handle = rtnl_tc_get_handle(TC_CAST(obj));
                uint32_t parent = rtnl_tc_get_parent(TC_CAST(obj));
                if (parent == handles[i])
                    handles.push_back(handle);
            }
        }

        nl_cache_free(cache);

        for (int i = handles.size() - 1; i >= 0; i--) {
            rtnl_tc_set_handle(TC_CAST(cls), handles[i]);
            DumpNetlinkObject("del", cls);
            ret = rtnl_class_delete(rtnl, cls);
            if (ret < 0)
                break;
        }
    }

    if (ret < 0)
        error = NetlinkError(ret, "Cannot remove traffic class");
out:
    rtnl_class_put(cls);
    return error;
}

TError TNetwork::UpdateTrafficClasses(int parent, int minor,
        std::map<std::string, uint64_t> &Prio,
        std::map<std::string, uint64_t> &Rate,
        std::map<std::string, uint64_t> &Ceil) {
    TError error;

    for (auto iface: ifaces) {
        auto name = iface.first;
        auto prio = (Prio.find(name) != Prio.end()) ? Prio[name] : Prio["default"];
        auto rate = (Rate.find(name) != Rate.end()) ? Rate[name] : Rate["default"];
        auto ceil = (Ceil.find(name) != Ceil.end()) ? Ceil[name] : Ceil["default"];
        error = AddTrafficClass(iface.second,
                                TC_HANDLE(ROOT_TC_MAJOR, parent),
                                TC_HANDLE(ROOT_TC_MAJOR, minor),
                                prio, rate, ceil);
        if (error)
            return error;
    }

    return TError::Success();
}

TError TNetwork::RemoveTrafficClasses(int minor) {
    TError error;

    for (auto iface: ifaces) {
        error = DelTrafficClass(iface.second, TC_HANDLE(ROOT_TC_MAJOR, minor));
        if (error)
            return error;
    }
    return TError::Success();
}

TError TNetwork::OpenLinks(std::vector<std::shared_ptr<TNlLink>> &links) {
    TError error;

    for (auto &iface : ifaces) {
        auto l = std::make_shared<TNlLink>(Nl, iface.first);
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


void TNetCfg::Reset() {
    /* default - create new empty netns */
    NewNetNs = true;
    Host = false;
    Inherited = false;
    HostIface.clear();
    MacVlan.clear();
    IpVlan.clear();
    Veth.clear();
    NetNsName = "";
    NetCtName = "";
}

TError TNetCfg::ParseNet(std::vector<std::string> lines) {
    bool none = false;
    int idx = 0;

    Reset();

    if (lines.size() == 0)
        return TError(EError::InvalidValue, "Configuration is not specified");

    for (auto &line : lines) {
        std::vector<std::string> settings;

        TError error = SplitEscapedString(line, ' ', settings);
        if (error)
            return error;

        if (settings.size() == 0)
            return TError(EError::InvalidValue, "Invalid net in: " + line);

        std::string type = StringTrim(settings[0]);

        if (type == "none") {
            none = true;
        } else if (type == "inherited") {
            NewNetNs = false;
            Inherited = true;
        } else if (type == "host") {
            THostNetCfg hnet;

            if (settings.size() > 2)
                return TError(EError::InvalidValue, "Invalid net in: " + line);

            if (settings.size() == 1) {
                NewNetNs = false;
                Host = true;
            } else {
                hnet.Dev = StringTrim(settings[1]);
                HostIface.push_back(hnet);
            }
        } else if (type == "container") {
            if (settings.size() != 2)
                return TError(EError::InvalidValue, "Invalid net in: " + line);
            NewNetNs = false;
            NetCtName = StringTrim(settings[1]);
        } else if (type == "macvlan") {
            if (settings.size() < 3)
                return TError(EError::InvalidValue, "Invalid macvlan in: " + line);

            std::string master = StringTrim(settings[1]);
            std::string name = StringTrim(settings[2]);
            std::string type = "bridge";
            std::string hw = "";
            int mtu = -1;

            if (settings.size() > 3) {
                type = StringTrim(settings[3]);
                if (!TNlLink::ValidMacVlanType(type))
                    return TError(EError::InvalidValue,
                            "Invalid macvlan type " + type);
            }

            if (settings.size() > 4) {
                TError error = StringToInt(settings[4], mtu);
                if (error)
                    return TError(EError::InvalidValue,
                            "Invalid macvlan mtu " + settings[4]);
            }

            if (settings.size() > 5) {
                hw = StringTrim(settings[5]);
                if (!TNlLink::ValidMacAddr(hw))
                    return TError(EError::InvalidValue,
                            "Invalid macvlan address " + hw);
            }

            TMacVlanNetCfg mvlan;
            mvlan.Master = master;
            mvlan.Name = name;
            mvlan.Type = type;
            mvlan.Hw = hw;
            mvlan.Mtu = mtu;

            MacVlan.push_back(mvlan);
        } else if (type == "ipvlan") {
            if (settings.size() < 3)
                return TError(EError::InvalidValue, "Invalid ipvlan in: " + line);

            std::string master = StringTrim(settings[1]);
            std::string name = StringTrim(settings[2]);
            std::string mode = "l2";
            int mtu = -1;

            if (settings.size() > 3) {
                mode = StringTrim(settings[3]);
                if (!TNlLink::ValidIpVlanMode(mode))
                    return TError(EError::InvalidValue,
                            "Invalid ipvlan mode " + mode);
            }

            if (settings.size() > 4) {
                TError error = StringToInt(settings[4], mtu);
                if (error)
                    return TError(EError::InvalidValue,
                            "Invalid ipvlan mtu " + settings[4]);
            }

            TIpVlanNetCfg ipvlan;
            ipvlan.Master = master;
            ipvlan.Name = name;
            ipvlan.Mode = mode;
            ipvlan.Mtu = mtu;

            IpVlan.push_back(ipvlan);
        } else if (type == "veth") {
            if (settings.size() < 3)
                return TError(EError::InvalidValue, "Invalid veth in: " + line);
            std::string name = StringTrim(settings[1]);
            std::string bridge = StringTrim(settings[2]);
            std::string hw = "";
            int mtu = -1;

            if (settings.size() > 3) {
                TError error = StringToInt(settings[3], mtu);
                if (error)
                    return TError(EError::InvalidValue,
                            "Invalid veth mtu " + settings[3]);
            }

            if (settings.size() > 4) {
                hw = StringTrim(settings[4]);
                if (!TNlLink::ValidMacAddr(hw))
                    return TError(EError::InvalidValue,
                            "Invalid veth address " + hw);
            }

            TVethNetCfg veth;
            veth.Bridge = bridge;
            veth.Name = name;
            veth.Hw = hw;
            veth.Mtu = mtu;
            veth.Peer = "portove-" + std::to_string(Id) + "-" + std::to_string(idx++);

            Veth.push_back(veth);
        } else if (type == "netns") {
            if (settings.size() != 2)
                return TError(EError::InvalidValue, "Invalid netns in: " + line);
            std::string name = StringTrim(settings[1]);
            TPath path("/var/run/netns/" + name);
            if (!path.Exists())
                return TError(EError::InvalidValue, "net namespace not found: " + name);
            NewNetNs = false;
            NetNsName = name;
        } else {
            return TError(EError::InvalidValue, "Configuration is not specified");
        }
    }

    int single = none + Host + Inherited;
    int mixed = HostIface.size() + MacVlan.size() + IpVlan.size() + Veth.size();

    if (single > 1 || (single == 1 && mixed))
        return TError(EError::InvalidValue, "none/host/inherited can't be mixed with other types");

    return TError::Success();
}

TError TNetCfg::ParseIp(std::vector<std::string> lines) {
    IpVec.clear();
    for (auto &line : lines) {
        std::vector<std::string> settings;
        TError error = SplitEscapedString(line, ' ', settings);
        if (error)
            return error;

        if (settings.size() != 2)
            return TError(EError::InvalidValue, "Invalid ip address/prefix in: " + line);

        TIpVec ip;
        ip.Iface = settings[0];
        error = ParseIpPrefix(settings[1], ip.Addr, ip.Prefix);
        if (error)
            return error;
        IpVec.push_back(ip);
    }
    return TError::Success();
}

TError TNetCfg::ParseGw(std::vector<std::string> lines) {
    GwVec.clear();
    for (auto &line : lines) {
        std::vector<std::string> settings;
        TError error = SplitEscapedString(line, ' ', settings);
        if (error)
            return error;

        if (settings.size() != 2)
            return TError(EError::InvalidValue, "Invalid gateway address/prefix in: " + line);

        TGwVec gw;
        gw.Iface = settings[0];
        error = gw.Addr.Parse(settings[1]);
        if (error)
            return error;
        GwVec.push_back(gw);
    }
    return TError::Success();
}
