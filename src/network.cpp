#include <algorithm>
#include <sstream>

#include "network.hpp"
#include "container.hpp"
#include "holder.hpp"
#include "config.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/crc32.hpp"

extern "C" {
#include <fnmatch.h>
#include <netlink/route/addr.h>
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

TError TNetwork::PrepareLinks() {
    TError error;

    for (auto &iface: ifaces) {
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

    uint64_t prio = NET_DEFAULT_PRIO;
    uint64_t rate = NET_MAX_GUARANTEE;
    uint64_t ceil = NET_MAX_LIMIT;

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

TNetwork::TNetwork() : NatBitmap(0, 0) {
    Nl = std::make_shared<TNl>();
    PORTO_ASSERT(Nl != nullptr);
}

TNetwork::~TNetwork() {
}

TError TNetwork::Connect() {
    return Nl->Connect();
}

TError TNetwork::ConnectNetns(TNamespaceFd &netns) {
    TNamespaceFd my_netns;
    TError error;

    error = my_netns.Open(GetTid(), "ns/net");
    if (error)
        return error;

    error = netns.SetNs(CLONE_NEWNET);
    if (error)
        return error;

    error = Connect();

    TError error2 = my_netns.SetNs(CLONE_NEWNET);
    PORTO_ASSERT(!error2);

    return error;
}

TError TNetwork::ConnectNew(TNamespaceFd &netns) {
    TNamespaceFd my_netns;
    TError error;

    error = my_netns.Open(GetTid(), "ns/net");
    if (error)
        return error;

    if (unshare(CLONE_NEWNET))
        return TError(EError::Unknown, errno, "unshare(CLONE_NEWNET)");

    error = netns.Open(GetTid(), "ns/net");
    if (!error) {
        error = Connect();
        if (error)
            netns.Close();
    }

    if (!error)
        error = SetSysctl("net.ipv6.conf.all.accept_dad", "0");
    if (!error)
        error = SetSysctl("net.ipv6.conf.default.accept_dad", "0");

    TError error2 = my_netns.SetNs(CLONE_NEWNET);
    PORTO_ASSERT(!error2);

    return error;
}

TError TNetwork::UpdateInterfaces() {
    struct nl_cache *cache;
    int ret;

    ret = rtnl_link_alloc_cache(GetSock(), AF_UNSPEC, &cache);
    if (ret < 0)
        return Nl->Error(ret, "Cannot allocate link cache");

    ifaces.clear();

    for (auto obj = nl_cache_get_first(cache); obj; obj = nl_cache_get_next(obj)) {
        auto link = (struct rtnl_link *)obj;
        int ifindex = rtnl_link_get_ifindex(link);
        int flags = rtnl_link_get_flags(link);
        const char *name = rtnl_link_get_name(link);
        const char *type = rtnl_link_get_type(link);

        if ((flags & IFF_LOOPBACK) || !(flags & IFF_RUNNING))
            continue;

        if (type && !strcmp(type, "veth") &&
                (!strncmp(name, "portove-", 8) ||
                 !strncmp(name, "L3-", 3)))
            continue;

        Nl->Dump("managed link", link);
        ifaces.push_back(std::make_pair(std::string(name), ifindex));
    }

    nl_cache_free(cache);
    return TError::Success();
}

TError TNetwork::GetGateAddress(std::vector<TNlAddr> addrs,
                                TNlAddr &gate4, TNlAddr &gate6, int &mtu) {
    struct nl_cache *cache, *lcache;
    int ret;

    ret = rtnl_addr_alloc_cache(GetSock(), &cache);
    if (ret < 0)
        return Nl->Error(ret, "Cannot allocate addr cache");

    ret = rtnl_link_alloc_cache(GetSock(), AF_UNSPEC, &lcache);
    if (ret < 0) {
        nl_cache_free(cache);
        return Nl->Error(ret, "Cannot allocate link cache");
    }

    for (auto obj = nl_cache_get_first(cache); obj; obj = nl_cache_get_next(obj)) {
         auto addr = (struct rtnl_addr *)obj;
         auto local = rtnl_addr_get_local(addr);

         if (!local || rtnl_addr_get_scope(addr) == RT_SCOPE_HOST)
             continue;

         for (auto &a: addrs) {

             if (nl_addr_get_family(a.Addr) == nl_addr_get_family(local)) {

                 /* get any gate of required family */
                 if (nl_addr_get_family(local) == AF_INET && !gate4.Addr)
                     gate4 = TNlAddr(local);

                 if (nl_addr_get_family(local) == AF_INET6 && !gate6.Addr)
                     gate6 = TNlAddr(local);
             }

             if (nl_addr_cmp_prefix(local, a.Addr) == 0) {

                 /* choose best matching gate address */
                 if (nl_addr_get_family(local) == AF_INET &&
                         nl_addr_cmp_prefix(gate4.Addr, a.Addr) != 0)
                     gate4 = TNlAddr(local);

                 if (nl_addr_get_family(local) == AF_INET6 &&
                         nl_addr_cmp_prefix(gate6.Addr, a.Addr) != 0)
                     gate6 = TNlAddr(local);

                 auto link = rtnl_link_get(lcache, rtnl_addr_get_ifindex(addr));
                 if (link) {
                     int link_mtu = rtnl_link_get_mtu(link);

                     if (mtu < 0 || link_mtu < mtu)
                         mtu = link_mtu;

                     rtnl_link_put(link);
                 }
             }
         }
    }

    nl_cache_free(lcache);
    nl_cache_free(cache);

    if (gate4.Addr)
        nl_addr_set_prefixlen(gate4.Addr, 32);

    if (gate6.Addr)
        nl_addr_set_prefixlen(gate6.Addr, 128);

    return TError::Success();
}

TError TNetwork::AddAnnounce(const TNlAddr &addr, std::string master) {
    struct nl_cache *cache;
    TError error;
    int ret;

    if (master != "") {
        int index = InterfaceIndex(master);
        if (index)
            return Nl->ProxyNeighbour(index, addr, true);
        return TError(EError::InvalidValue, "Master link not found: " + master);
    }

    ret = rtnl_addr_alloc_cache(GetSock(), &cache);
    if (ret < 0)
        return Nl->Error(ret, "Cannot allocate addr cache");

    for (auto &iface : ifaces) {
        bool reachable = false;

        for (auto obj = nl_cache_get_first(cache); obj;
                obj = nl_cache_get_next(obj)) {
            auto raddr = (struct rtnl_addr *)obj;
            auto local = rtnl_addr_get_local(raddr);

            if (rtnl_addr_get_ifindex(raddr) == iface.second &&
                    local && nl_addr_cmp_prefix(local, addr.Addr) == 0) {
                reachable = true;
                break;
            }
        }

        /* Add proxy entry only if address is directly reachable */
        if (reachable) {
            error = Nl->ProxyNeighbour(iface.second, addr, true);
            if (error)
                break;
        }
    }

    nl_cache_free(cache);

    return error;
}

TError TNetwork::DelAnnounce(const TNlAddr &addr) {
    TError error;

    for (auto &iface : ifaces)
        error = Nl->ProxyNeighbour(iface.second, addr, false);

    return error;
}

TError TNetwork::GetNatAddress(std::vector<TNlAddr> &addrs) {
    TError error;
    int offset;

    error = NatBitmap.Get(offset);
    if (error)
        return TError(error, "Cannot allocate NAT address");

    if (!NatBaseV4.IsEmpty()) {
        TNlAddr addr = NatBaseV4;
        addr.AddOffset(offset);
        addrs.push_back(addr);
    }

    if (!NatBaseV6.IsEmpty()) {
        TNlAddr addr = NatBaseV6;
        addr.AddOffset(offset);
        addrs.push_back(addr);
    }

    return TError::Success();
}

TError TNetwork::PutNatAddress(const std::vector<TNlAddr> &addrs) {

    for (auto &addr: addrs) {
        if (addr.Family() == AF_INET && !NatBaseV4.IsEmpty()) {
            uint64_t offset =  addr.GetOffset(NatBaseV4);
            return NatBitmap.Put(offset);
        }
        if (addr.Family() == AF_INET6 && !NatBaseV6.IsEmpty()) {
            uint64_t offset =  addr.GetOffset(NatBaseV6);
            return NatBitmap.Put(offset);
        }
    }

    return TError::Success();
}

std::string TNetwork::GetIfaceName(const std::string &prefix) {
    for (int retry = 0; retry < 100; retry++) {
        std::string name = prefix + std::to_string(IfaceName++);
        TNlLink link(Nl, name);
        if (link.Load())
            return name;
    }
    return prefix + "0";
}

std::string TNetwork::MatchIface(const std::string &pattern) {
    for (auto &iface: ifaces) {
        if (!fnmatch(pattern.c_str(), iface.first.c_str(), 0))
            return iface.first;
    }
    return pattern;
}

TError TNetwork::GetInterfaceCounters(ETclassStat stat,
                                      std::map<std::string, uint64_t> &result) {
    struct nl_cache *cache;
    rtnl_link_stat_id_t id;

    switch (stat) {
        case ETclassStat::RxBytes:
            id = RTNL_LINK_RX_BYTES;
            break;
        case ETclassStat::RxPackets:
            id = RTNL_LINK_RX_PACKETS;
            break;
        case ETclassStat::RxDrops:
            id = RTNL_LINK_RX_DROPPED;
            break;
        default:
            return TError(EError::Unknown, "Unsupported netlink statistics");
    }

    int ret = rtnl_link_alloc_cache(GetSock(), AF_UNSPEC, &cache);
    if (ret < 0)
        return Nl->Error(ret, "Cannot allocate link cache");

    for (auto &iface: ifaces) {
        auto link = rtnl_link_get(cache, iface.second);
        if (link)
            result[iface.first] = rtnl_link_get_stat(link, id);
        rtnl_link_put(link);
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
    case ETclassStat::RxBytes:
    case ETclassStat::RxPackets:
    case ETclassStat::RxDrops:
        return GetInterfaceCounters(stat, result);
    default:
        return TError(EError::Unknown, "Unsupported netlink statistics");
    }

    for (auto &iface: ifaces) {
        struct nl_cache *cache;
        struct rtnl_class *cls;

        /* TODO optimize this stuff */
        int ret = rtnl_class_alloc_cache(GetSock(), iface.second, &cache);
        if (ret < 0)
            return Nl->Error(ret, "Cannot allocate class cache");

        cls = rtnl_class_get(cache, iface.second, handle);
        if (cls) {
            result[iface.first] = rtnl_tc_get_stat(TC_CAST(cls), rtnlStat);
            rtnl_class_put(cls);
        } else {
            L_WRN() << "Cannot find tc class " << minor << " at "
                    << iface.second << ":" << iface.first << std::endl;
        }
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
        error = Nl->Error(ret, "Cannot set HTB class");
        goto free_class;
    }

    /*
     * TC doesn't allow to set 0 rate, but Porto does (because we call them
     * net_guarantee). So, just map 0 to 1, minimal valid guarantee.
     */
    rtnl_htb_set_rate(cls, rate ?: 1);

    rtnl_htb_set_prio(cls, prio);

    /*
     * Zero ceil must be no limit.
     * Libnl set default ceil equal to rate.
     */
    if (ceil)
        rtnl_htb_set_ceil(cls, ceil);
    else
        rtnl_htb_set_ceil(cls, UINT32_MAX);

    rtnl_htb_set_quantum(cls, 10000);

    /*
       rtnl_htb_set_rbuffer(tclass, burst);
       rtnl_htb_set_cbuffer(tclass, cburst);
       */

    Nl->Dump("add", cls);
    ret = rtnl_class_add(GetSock(), cls, NLM_F_CREATE | NLM_F_REPLACE);
    if (ret < 0)
        error = Nl->Error(ret, "Cannot add traffic class to " + std::to_string(ifIndex));

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

    Nl->Dump("del", cls);
    ret = rtnl_class_delete(GetSock(), cls);

    /* If busy -> remove recursively */
    if (ret == -NLE_BUSY) {
        std::vector<uint32_t> handles({handle});
        struct nl_cache *cache;

        ret = rtnl_class_alloc_cache(GetSock(), ifIndex, &cache);
        if (ret < 0) {
            error = Nl->Error(ret, "Cannot allocate class cache");
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
            Nl->Dump("del", cls);
            ret = rtnl_class_delete(GetSock(), cls);
            if (ret < 0)
                break;
        }
    }

    if (ret < 0)
        error = Nl->Error(ret, "Cannot remove traffic class");
out:
    rtnl_class_put(cls);
    return error;
}

TError TNetwork::UpdateTrafficClasses(int parent, int minor,
        std::map<std::string, uint64_t> &Prio,
        std::map<std::string, uint64_t> &Rate,
        std::map<std::string, uint64_t> &Ceil) {
    TError error, result;

    for (auto &i: Prio) {
        if (i.first != "default" && !InterfaceIndex(i.first))
            L_WRN() <<  "Interface " + i.first + " not found" << std::endl;
    }

    for (auto &i: Rate) {
        if (i.first != "default" && !InterfaceIndex(i.first))
            L_WRN() <<  "Interface " + i.first + " not found" << std::endl;
    }

    for (auto &i: Ceil) {
        if (i.first != "default" && !InterfaceIndex(i.first))
            L_WRN() <<  "Interface " + i.first + " not found" << std::endl;
    }

    for (auto &iface: ifaces) {
        auto name = iface.first;
        auto prio = (Prio.find(name) != Prio.end()) ? Prio[name] : Prio["default"];
        auto rate = (Rate.find(name) != Rate.end()) ? Rate[name] : Rate["default"];
        auto ceil = (Ceil.find(name) != Ceil.end()) ? Ceil[name] : Ceil["default"];
        error = AddTrafficClass(iface.second,
                                TC_HANDLE(ROOT_TC_MAJOR, parent),
                                TC_HANDLE(ROOT_TC_MAJOR, minor),
                                prio, rate, ceil);
        if (error) {
            L_WRN() << "Cannot add tc class " << iface.first << " " << error << std::endl;
            if (!result)
                result = error;
        }
    }

    return result;
}

TError TNetwork::RemoveTrafficClasses(int minor) {
    TError error;

    for (auto &iface: ifaces) {
        error = DelTrafficClass(iface.second, TC_HANDLE(ROOT_TC_MAJOR, minor));
        if (error)
            return error;
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
    L3lan.clear();
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

        } else if (type == "L3") {
            TL3NetCfg l3;

            l3.Name = "eth0";
            l3.Nat = false;
            if (settings.size() > 1)
                l3.Name = StringTrim(settings[1]);

            l3.Mtu = -1;
            if (settings.size() > 2)
                l3.Master = StringTrim(settings[2]);

            L3lan.push_back(l3);

        } else if (type == "NAT") {
            TL3NetCfg nat;

            nat.Nat = true;
            nat.Name = "eth0";
            nat.Mtu = -1;

            if (settings.size() > 1)
                nat.Name = StringTrim(settings[1]);

            L3lan.push_back(nat);

        } else if (type == "MTU") {
            if (settings.size() != 3)
                return TError(EError::InvalidValue, "Invalid MTU in: " + line);

            int mtu;
            TError error = StringToInt(settings[2], mtu);
            if (error)
                return error;

            for (auto &link: L3lan) {
                if (link.Name == settings[1]) {
                    link.Mtu = mtu;
                    return TError::Success();
                }
            }

            for (auto &link: Veth) {
                if (link.Name == settings[1]) {
                    link.Mtu = mtu;
                    return TError::Success();
                }
            }

            for (auto &link: MacVlan) {
                if (link.Name == settings[1]) {
                    link.Mtu = mtu;
                    return TError::Success();
                }
            }

            for (auto &link: IpVlan) {
                if (link.Name == settings[1]) {
                    link.Mtu = mtu;
                    return TError::Success();
                }
            }

            return TError(EError::InvalidValue, "Link not found: " + settings[1]);

        } else if (type == "autoconf") {
            if (settings.size() != 2)
                return TError(EError::InvalidValue, "Invalid autoconf in: " + line);
            Autoconf.push_back(StringTrim(settings[1]));
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
    int mixed = HostIface.size() + MacVlan.size() + IpVlan.size() + Veth.size() + L3lan.size();

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
        error = ip.Addr.Parse(AF_UNSPEC, settings[1]);
        if (error)
            return error;
        IpVec.push_back(ip);

        for (auto &l3: L3lan) {
            if (l3.Name == ip.Iface) {
                if (!ip.Addr.IsHost())
                    return TError(EError::InvalidValue, "Invalid ip prefix for L3 network");
                l3.Addrs.push_back(ip.Addr);
            }
        }
    }
    return TError::Success();
}

TError TNetCfg::FormatIp(std::vector<std::string> &lines) {
    for (auto &ip: IpVec)
        lines.push_back(ip.Iface + " " + ip.Addr.Format());
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
        error = gw.Addr.Parse(AF_UNSPEC, settings[1]);
        if (error)
            return error;
        GwVec.push_back(gw);
    }
    return TError::Success();
}

std::string TNetCfg::GenerateHw(const std::string &name) {
    uint32_t n = Crc32(name);
    uint32_t h = Crc32(Hostname);

    return StringFormat("02:%02x:%02x:%02x:%02x:%02x",
            (n & 0x000000FF) >> 0,
            (h & 0xFF000000) >> 24,
            (h & 0x00FF0000) >> 16,
            (h & 0x0000FF00) >> 8,
            (h & 0x000000FF) >> 0);
}

TError TNetCfg::ConfigureVeth(TVethNetCfg &veth) {
    auto parentNl = ParentNet->GetNl();
    TNlLink peer(parentNl, ParentNet->GetIfaceName("portove-"));
    TError error;

    std::string hw = veth.Hw;
    if (hw.empty() && !Hostname.empty())
        hw = GenerateHw(veth.Name + veth.Peer);

    error = peer.AddVeth(veth.Name, hw, veth.Mtu, NetNs.GetFd());
    if (error)
        return error;

    if (!veth.Bridge.empty()) {
        TNlLink bridge(parentNl, veth.Bridge);
        error = bridge.Load();
        if (error)
            return error;

        error = bridge.Enslave(peer.GetName());
        if (error)
            return error;
    }

    return TError::Success();
}

TError TNetCfg::ConfigureL3(TL3NetCfg &l3) {
    std::string peerName = ParentNet->GetIfaceName("L3-");
    auto parentNl = ParentNet->GetNl();
    TNlLink peer(parentNl, peerName);
    TNlAddr gate4, gate6;
    TError error;

    if (l3.Nat && l3.Addrs.empty()) {
        error = ParentNet->GetNatAddress(l3.Addrs);
        if (error)
            return error;

        for (auto &addr: l3.Addrs) {
            TIpVec ip;
            ip.Iface = l3.Name;
            ip.Addr = addr;
            IpVec.push_back(ip);
        }

        SaveIp = true;
    }

    error = ParentNet->GetGateAddress(l3.Addrs, gate4, gate6, l3.Mtu);
    if (error)
        return error;

    for (auto &addr : l3.Addrs) {
        if (addr.Family() == AF_INET && gate4.IsEmpty())
            return TError(EError::InvalidValue, "Ipv4 gateway not found");
        if (addr.Family() == AF_INET6 && gate6.IsEmpty())
            return TError(EError::InvalidValue, "Ipv6 gateway not found");
    }

    error = peer.AddVeth(l3.Name, "", l3.Mtu, NetNs.GetFd());
    if (error)
        return error;

    TNlLink link(Net->GetNl(), l3.Name);
    error = link.Load();
    if (error)
        return error;

    error = link.Up();
    if (error)
        return error;

    if (!gate4.IsEmpty()) {
        error = parentNl->ProxyNeighbour(peer.GetIndex(), gate4, true);
        if (error)
            return error;
        error = link.AddDirectRoute(gate4);
        if (error)
            return error;
        error = link.SetDefaultGw(gate4);
        if (error)
            return error;
    }

    if (!gate6.IsEmpty()) {
        error = parentNl->ProxyNeighbour(peer.GetIndex(), gate6, true);
        if (error)
            return error;
        error = link.AddDirectRoute(gate6);
        if (error)
            return error;
        error = link.SetDefaultGw(gate6);
        if (error)
            return error;
    }

    for (auto &addr : l3.Addrs) {
        error = peer.AddDirectRoute(addr);
        if (error)
            return error;

        error = ParentNet->AddAnnounce(addr, ParentNet->MatchIface(l3.Master));
        if (error)
            return error;
    }

    return TError::Success();
}

TError TNetCfg::ConfigureInterfaces() {
    std::vector<std::string> links;
    auto parent_lock = ParentNet->ScopedLock();
    auto source_nl = ParentNet->GetNl();
    auto target_nl = Net->GetNl();
    TError error;

    for (auto &host : HostIface) {
        TNlLink link(source_nl, host.Dev);
        error = link.ChangeNs(host.Dev, NetNs.GetFd());
        if (error)
            return error;
        links.emplace_back(host.Dev);
    }

    for (auto &ipvlan : IpVlan) {
        std::string master = ParentNet->MatchIface(ipvlan.Master);

        TNlLink link(source_nl, "piv" + std::to_string(GetTid()));
        error = link.AddIpVlan(master, ipvlan.Mode, ipvlan.Mtu);
        if (error)
            return error;

        error = link.ChangeNs(ipvlan.Name, NetNs.GetFd());
        if (error) {
            (void)link.Remove();
            return error;
        }
        links.emplace_back(ipvlan.Name);
    }

    for (auto &mvlan : MacVlan) {
        std::string master = ParentNet->MatchIface(mvlan.Master);

        std::string hw = mvlan.Hw;
        if (hw.empty() && !Hostname.empty())
            hw = GenerateHw(master + mvlan.Name);

        TNlLink link(source_nl, "pmv" + std::to_string(GetTid()));
        error = link.AddMacVlan(master, mvlan.Type, hw, mvlan.Mtu);
        if (error)
                return error;

        error = link.ChangeNs(mvlan.Name, NetNs.GetFd());
        if (error) {
            (void)link.Remove();
            return error;
        }
        links.emplace_back(mvlan.Name);
    }

    for (auto &veth : Veth) {
        error = ConfigureVeth(veth);
        if (error)
            return error;
        links.emplace_back(veth.Name);
    }

    for (auto &l3 : L3lan) {
        error = ConfigureL3(l3);
        if (error)
            return error;
        links.emplace_back(l3.Name);
    }

    parent_lock.unlock();

    TNlLink loopback(target_nl, "lo");
    error = loopback.Load();
    if (error)
        return error;
    error = loopback.Up();
    if (error)
        return error;

    for (auto &name: links) {
        TNlLink link(target_nl, name);
        bool hasConfig = false;

        error = link.Load();
        if (error)
            return error;

        for (auto &ip: IpVec)
            if (ip.Iface == name)
                hasConfig = true;

        for (auto &gw: GwVec)
            if (gw.Iface == name)
                hasConfig = true;

        for (auto &ac: Autoconf)
            if (ac == name)
                hasConfig = true;

        if (NetUp || hasConfig) {
            error = link.Up();
            if (error)
                return error;
        }

        for (auto &ip: IpVec) {
            if (ip.Iface == name) {
                error = link.AddAddress(ip.Addr);
                if (error)
                    return error;
            }
        }

        for (auto &gw: GwVec) {
            if (gw.Iface == name) {
                error = link.SetDefaultGw(gw.Addr);
                if (error)
                    return error;
            }
        }

        Net->AddInterface(link);
    }

    return TError::Success();
}

TError TNetCfg::PrepareNetwork() {
    TError error;

    if (Inherited) {

        Net = Parent->Net;
        ParentId = Parent->GetId();
        error = Parent->OpenNetns(NetNs);
        if (error)
            return error;

    } else if (NewNetNs) {

        Net = std::make_shared<TNetwork>();
        ParentId = PORTO_ROOT_CONTAINER_ID;

        error = Net->ConnectNew(NetNs);
        if (error)
            return error;

        error = ConfigureInterfaces();
        if (error) {
            (void)DestroyNetwork();
            return error;
        }

        error = Net->PrepareLinks();
        if (error)
            return error;

    } else if (Host) {

        Net = std::make_shared<TNetwork>();
        ParentId = ROOT_TC_MINOR;

        error = Net->Connect();
        if (error)
            return error;

        error = NetNs.Open(GetTid(), "ns/net");
        if (error)
            return error;

        Holder->AddToNsMap(NetNs.GetInode(), Net);

        error = Net->UpdateInterfaces();
        if (error)
            return error;

        error = Net->PrepareLinks();
        if (error)
            return error;

        if (config().network().has_nat_first_ipv4())
            Net->NatBaseV4.Parse(AF_INET, config().network().nat_first_ipv4());
        if (config().network().has_nat_first_ipv6())
            Net->NatBaseV6.Parse(AF_INET6, config().network().nat_first_ipv6());
        if (config().network().has_nat_count())
            Net->NatBitmap.Resize(config().network().nat_count());

    } else if (NetNsName != "") {

        error = NetNs.Open("/var/run/netns/" + NetNsName);
        if (error)
            return error;

        Net = Holder->SearchInNsMap(NetNs.GetInode());
        if (!Net) {
            Net = std::make_shared<TNetwork>();

            error = Net->ConnectNetns(NetNs);
            if (error)
                return error;

            error = Net->UpdateInterfaces();
            if (error)
                return error;

            error = Net->PrepareLinks();
            if (error)
                return error;

            Holder->AddToNsMap(NetNs.GetInode(), Net);
        }

        ParentId = PORTO_ROOT_CONTAINER_ID;

    } else if (NetCtName != "") {

        std::shared_ptr<TContainer> target;
        error = Holder->Get(NetCtName, target);
        if (error)
            return error;

        error = target->CheckPermission(OwnerCred);
        if (error)
            return TError(error, "net container " + NetCtName);

        error = target->OpenNetns(NetNs);
        if (error)
            return error;

        Net = target->Net;
        ParentId = target->GetId();

    }

    return TError::Success();
}

TError TNetCfg::DestroyNetwork() {
    TError error;

    if (!ParentNet)
        return TError::Success();

    for (auto &l3 : L3lan) {
        auto lock = ParentNet->ScopedLock();
        for (auto &addr : l3.Addrs) {
            error = ParentNet->DelAnnounce(addr);
            if (error)
                L_ERR() << "Cannot remove announce " << addr.Format()
                        << " : " << error << std::endl;
        }
        if (l3.Nat) {
            error = ParentNet->PutNatAddress(l3.Addrs);
            if (error)
                L_ERR() << "Cannot put NAT address : " << error << std::endl;

            auto ip = IpVec.begin();
            while (ip != IpVec.end()) {
                if (ip->Iface == l3.Name)
                    ip = IpVec.erase(ip);
                else
                    ++ip;
            }
            SaveIp = true;
        }
    }

    return error;
}
