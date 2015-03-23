#include "netlink.hpp"
#include "util/log.hpp"
#include "util/string.hpp"

// HTB shaping details:
// http://luxik.cdi.cz/~devik/qos/htb/manual/userg.htm

extern "C" {
#include <linux/if_ether.h>
#include <netinet/ether.h>
#include <netlink/route/class.h>
#include <netlink/route/classifier.h>
#include <netlink/route/cls/cgroup.h>
#include <netlink/route/qdisc.h>
#include <netlink/route/qdisc/htb.h>

#include <netlink/route/rtnl.h>
#include <netlink/route/link.h>
#include <netlink/route/link/macvlan.h>
#include <netlink/route/link/veth.h>

#include <netlink/route/route.h>
#include <netlink/route/addr.h>
}

using std::string;
using std::vector;

static bool debug = false;

uint32_t TcHandle(uint16_t maj, uint16_t min) {
    return TC_HANDLE(maj, min);
}

uint32_t TcRootHandle() {
    return TC_H_ROOT;
}

uint16_t TcMajor(uint32_t handle) {
    return (uint16_t)(handle >> 16);
}

TError TNl::Connect() {
    int ret;
    TError error;

    Sock = nl_socket_alloc();
    if (!Sock)
        return TError(EError::Unknown, string("Unable to allocate netlink socket"));

    ret = nl_connect(Sock, NETLINK_ROUTE);
    if (ret < 0) {
        error = TError(EError::Unknown, string("Unable to connect netlink socket: ") + nl_geterror(ret));
        goto free_socket;
    }

    ret = rtnl_link_alloc_cache(Sock, AF_UNSPEC, &LinkCache);
    if (ret < 0) {
        error = TError(EError::Unknown, string("Unable to allocate link cache: ") + nl_geterror(ret));
        goto close_socket;
    }

    nl_cache_mngt_provide(LinkCache);

    return TError::Success();

close_socket:
    nl_close(Sock);
free_socket:
    nl_socket_free(Sock);

    return error;
}

void TNl::Disconnect() {
    if (LinkCache) {
        nl_cache_mngt_unprovide(LinkCache);
        nl_cache_free(LinkCache);
        LinkCache = nullptr;
    }
    if (Sock) {
        nl_close(Sock);
        nl_socket_free(Sock);
        Sock = nullptr;
    }
}

std::vector<std::string> TNl::FindLink(int flags) {
    struct Iter { int flags; vector<string> devices; } data;
    data.flags = flags;
    nl_cache_foreach(LinkCache, [](struct nl_object *obj, void *data) {
                     Iter *p = (Iter *)data;
                     struct rtnl_link *l = (struct rtnl_link *)obj;

                     if (!p->flags || (rtnl_link_get_flags(l) & p->flags))
                        p->devices.push_back(rtnl_link_get_name(l));
                     }, &data);

    return data.devices;
}

bool TNl::ValidLink(const std::string &name) {
    (void)nl_cache_refill(Sock, LinkCache);

    struct Iter { const std::string name; bool found; } data = { name, false };
    nl_cache_foreach(LinkCache, [](struct nl_object *obj, void *data) {
                     Iter *p = (Iter *)data;
                     struct rtnl_link *l = (struct rtnl_link *)obj;

                     if (rtnl_link_get_name(l) == p->name)
                        p->found = true;
                     }, &data);

    return data.found;
}

void TNl::EnableDebug(bool enable) {
    debug = enable;
}

TError TNl::GetDefaultLink(std::vector<std::string> &links) {
    struct FindDevIter { string name; vector<string> ifaces; } data;
    nl_cache_foreach(GetCache(), [](struct nl_object *obj, void *data) {
                     FindDevIter *p = (FindDevIter *)data;
                     struct rtnl_link *l = (struct rtnl_link *)obj;

                     if ((rtnl_link_get_flags(l) & IFF_RUNNING) &&
                        !(rtnl_link_get_flags(l) & IFF_LOOPBACK))
                        p->ifaces.push_back(rtnl_link_get_name(l));
                     }, &data);

    if (!data.ifaces.size())
        return TError(EError::Unknown, "Can't find appropriate link");

    links = data.ifaces;

    return TError::Success();
}

int TNl::GetFd() {
    return nl_socket_get_fd(Sock);
}

TError TNl::SubscribeToLinkUpdates() {
    int ret = nl_socket_add_membership(Sock, RTNLGRP_LINK);
    if (ret < 0)
        return TError(EError::Unknown, string("Unable to set subscribe to group: ") + nl_geterror(ret));

    return TError::Success();
}

void TNl::FlushEvents() {
    nl_recvmsgs_default(Sock);
}

TNlLink::~TNlLink() {
    if (Link)
        rtnl_link_put(Link);
}

TError TNlLink::SetDefaultGw(const TNlAddr &addr) {
    struct rtnl_route *route;
    struct rtnl_nexthop *nh;
    int ret;

    struct nl_addr *a;

    route = rtnl_route_alloc();
    if (!route)
        return TError(EError::Unknown, "Unable to allocate route");

    ret = nl_addr_parse("default", rtnl_route_get_family(route), &a);
    if (ret < 0) {
        rtnl_route_put(route);
        return TError(EError::Unknown, string("Unable to parse default address: ") + nl_geterror(ret));
    }

    ret = rtnl_route_set_dst(route, a);
    nl_addr_put(a);
    if (ret < 0) {
        rtnl_route_put(route);
        return TError(EError::Unknown, string("Unable to set route destination: ") + nl_geterror(ret));
    }

    nh = rtnl_route_nh_alloc();
    if (!route) {
        rtnl_route_put(route);
        return TError(EError::Unknown, "Unable to allocate next hop");
    }

    rtnl_route_nh_set_gateway(nh, addr.GetAddr());
    rtnl_route_nh_set_ifindex(nh, GetIndex());
    rtnl_route_add_nexthop(route, nh);

    ret = rtnl_route_add(GetSock(), route, NLM_F_MATCH);
    rtnl_route_put(route);
    if (ret < 0) {
        return TError(EError::Unknown, string("Unable to set default gateway: ") + nl_geterror(ret) + string(" ") + std::to_string(ret));
    }

    return TError::Success();
}

bool TNlLink::HasQueue() {
    return !(rtnl_link_get_flags(Link) & (IFF_LOOPBACK | IFF_POINTOPOINT | IFF_SLAVE));
}

TError TNlLink::SetIpAddr(const TNlAddr &addr, const int prefix) {
    int ret;
    struct rtnl_addr *a = rtnl_addr_alloc();
    if (!a)
        return TError(EError::Unknown, "Unable to allocate address");

    rtnl_addr_set_link(a, Link);
    rtnl_addr_set_family(a, nl_addr_get_family(addr.GetAddr()));

    ret = rtnl_addr_set_local(a, addr.GetAddr());
    if (ret < 0) {
        rtnl_addr_put(a);
        return TError(EError::Unknown, string("Unable to set local address: ") + nl_geterror(ret));
    }
    rtnl_addr_set_prefixlen(a, prefix);

    ret = rtnl_addr_add(GetSock(), a, 0);
    if (ret < 0) {
        rtnl_addr_put(a);
        return TError(EError::Unknown, string("Unable to add address: ") + nl_geterror(ret) + string(" ") + std::to_string(prefix));
    }

    rtnl_addr_put(a);

    return TError::Success();
}

TError TNlLink::Remove() {
    TError error = TError::Success();
    struct rtnl_link *hostLink = rtnl_link_alloc();
    rtnl_link_set_name(hostLink, Name.c_str());
    LogObj("remove", hostLink);
    int ret = rtnl_link_delete(GetSock(), hostLink);
    if (ret < 0)
        error = TError(EError::Unknown, string("Unable to remove macvlan: ") + nl_geterror(ret));
    rtnl_link_put(hostLink);

    return error;
}

TError TNlLink::Up() {
    int ret;
    struct rtnl_link *oldLink = rtnl_link_alloc();
    if (!oldLink)
        return TError(EError::Unknown, "Unable to allocate link");
    struct rtnl_link *newLink = rtnl_link_alloc();
    if (!newLink) {
        rtnl_link_put(oldLink);
        return TError(EError::Unknown, "Unable to allocate link");
    }

    rtnl_link_set_name(oldLink, Name.c_str());
    rtnl_link_set_name(newLink, Name.c_str());
    rtnl_link_set_flags(newLink, IFF_UP);

    LogObj("up", newLink);

    ret = rtnl_link_change(GetSock(), oldLink, newLink, 0);
    if (ret < 0) {
        rtnl_link_put(oldLink);
        rtnl_link_put(newLink);
        return TError(EError::Unknown, "Unable to change " + Name + " status: " + nl_geterror(ret));
    }

    rtnl_link_put(newLink);
    rtnl_link_put(oldLink);

    return TError::Success();
}

TError TNlLink::ChangeNs(const std::string &newName, int pid) {
    int ret;
    struct rtnl_link *newLink;
    struct rtnl_link *oldLink = rtnl_link_get_by_name(Nl->GetCache(), Name.c_str());
    if (!oldLink)
        return TError(EError::Unknown, "Invalid link " + Name);

    newLink = rtnl_link_alloc();
    if (!newLink) {
        rtnl_link_put(oldLink);
        return TError(EError::Unknown, "Can't allocate link");
    }

    rtnl_link_set_ifindex(newLink, rtnl_link_get_ifindex(oldLink));
    rtnl_link_set_name(newLink, newName.c_str());
    rtnl_link_set_ns_pid(newLink, pid);

    LogObj("change old", oldLink);
    LogObj("change new", newLink);

    ret = rtnl_link_change(GetSock(), oldLink, newLink, 0);
    if (ret < 0) {
        rtnl_link_put(oldLink);
        rtnl_link_put(newLink);
        return TError(EError::Unknown, "Unable to change " + Name + " namespace: " + nl_geterror(ret));
    }

    rtnl_link_put(newLink);
    rtnl_link_put(oldLink);

    return TError::Success();
}

bool TNlLink::Valid() {
    struct rtnl_link *l = rtnl_link_get_by_name(Nl->GetCache(), Name.c_str());
    rtnl_link_put(l);

    return l != nullptr;
}

int TNlLink::GetIndex() {
    return rtnl_link_get_ifindex(Link);
}

int TNlLink::FindIndex(const std::string &device) {
    struct Iter { string name; int idx; } data = { device, -1 };
    nl_cache_foreach(Nl->GetCache(), [](struct nl_object *obj, void *data) {
                     Iter *p = (Iter *)data;
                     struct rtnl_link *l = (struct rtnl_link *)obj;

                     if (p->idx >= 0)
                         return;

                     if (strncmp(rtnl_link_get_name(l), p->name.c_str(),
                                 p->name.length()) == 0 &&
                         rtnl_link_get_flags(l) & IFF_RUNNING)

                         p->idx = rtnl_link_get_ifindex(l);
                     }, &data);
    return data.idx;
}

TError TNl::RefillCache() {
    TError error;

    int ret = nl_cache_refill(GetSock(), GetCache());
    if (ret < 0) {
        error = TError(EError::Unknown, string("Can't refill cache: ") + nl_geterror(ret));
        L_ERR() << error << std::endl;
    }

    return error;
}

TError TNlLink::RefillCache() {
    return Nl->RefillCache();
}

TError TNlLink::AddMacVlan(const std::string &master,
                           const std::string &type, const std::string &hw,
                           int mtu) {
    TError error = TError::Success();
    struct rtnl_link *hostLink = rtnl_link_macvlan_alloc();
    int mode = rtnl_link_macvlan_str2mode(type.c_str());
    if (mode < 0)
        return TError(EError::Unknown, "Invalid MAC VLAN type " + type);

    struct ether_addr *ea = nullptr;
    if (hw.length()) {
        ea = ether_aton(hw.c_str());
        if (!ea)
            return TError(EError::Unknown, "Invalid MAC VLAN mac address " + hw);
    }

    int ret;
    int masterIdx = FindIndex(master);

    rtnl_link_set_link(hostLink, masterIdx);
    rtnl_link_set_name(hostLink, Name.c_str());

    if (mtu > 0)
        rtnl_link_set_mtu(hostLink, (unsigned int)mtu);

    if (ea) {
        struct nl_addr *addr = nl_addr_build(AF_LLC, ea, ETH_ALEN);
        rtnl_link_set_addr(hostLink, addr);
        nl_addr_put(addr);
    }

    rtnl_link_macvlan_set_mode(hostLink, mode);

    LogObj("add", hostLink);

    ret = rtnl_link_add(GetSock(), hostLink, NLM_F_CREATE);
    if (ret < 0)
        error = TError(EError::Unknown, string("Unable to add macvlan: ") + nl_geterror(ret));

    rtnl_link_put(hostLink);

    if (!error)
        error = RefillCache();

    return error;
}

TError TNlLink::Enslave(const std::string &name) {
    int ret;
    struct rtnl_link *slave = rtnl_link_get_by_name(Nl->GetCache(), name.c_str());
    if (!slave)
        return TError(EError::Unknown, "Invalid link " + name);

    ret = rtnl_link_enslave(GetSock(), Link, slave);
    rtnl_link_put(slave);
    if (ret < 0)
        return TError(EError::Unknown, string("Unable to enslave interface: ") + nl_geterror(ret));

    return TError::Success();
}

TError TNlLink::AddVeth(const std::string &name, const std::string &peerName, const std::string &hw, int mtu, int nsPid) {
	struct rtnl_link *veth, *peer;
    int ret;
    TError error;

    struct ether_addr *ea = nullptr;
    if (hw.length()) {
        ea = ether_aton(hw.c_str());
        if (!ea)
            return TError(EError::Unknown, "Invalid VETH mac address " + hw);
    }

	veth = rtnl_link_veth_alloc();
    if (!veth)
        return TError(EError::Unknown, "Unable to allocate veth");

	peer = rtnl_link_veth_get_peer(veth);

    rtnl_link_set_name(peer, name.c_str());
    rtnl_link_set_ns_pid(peer, nsPid);
    rtnl_link_set_name(veth, peerName.c_str());

    if (mtu > 0) {
        rtnl_link_set_mtu(peer, (unsigned int)mtu);
        rtnl_link_set_mtu(veth, (unsigned int)mtu);
    }

    if (ea) {
        struct nl_addr *addr = nl_addr_build(AF_LLC, ea, ETH_ALEN);
        rtnl_link_set_addr(peer, addr);
        rtnl_link_set_addr(veth, addr);
        nl_addr_put(addr);
    }

	ret = rtnl_link_add(GetSock(), veth, NLM_F_CREATE | NLM_F_EXCL);
    if (ret < 0) {
        rtnl_link_put(veth);
        return TError(EError::Unknown, string("Unable to add veth: ") + nl_geterror(ret));
    }

    error = RefillCache();
    if (error) {
        (void)rtnl_link_delete(GetSock(), peer);
        rtnl_link_put(veth);
        return error;
    }

    error = Enslave(peerName);
    if (error) {
        (void)rtnl_link_delete(GetSock(), peer);
        rtnl_link_put(veth);
        return error;
    }

    rtnl_link_put(veth);
    return TError::Success();
}

const std::string &TNlLink::GetAlias() {
    if (Alias.length())
        return Alias;
    else
        return Name;
}

bool TNlLink::ValidMacVlanType(const std::string &type) {
    return rtnl_link_macvlan_str2mode(type.c_str()) >= 0;
}

bool TNlLink::ValidMacAddr(const std::string &hw) {
    return ether_aton(hw.c_str()) != nullptr;
}

TError TNlLink::Load() {
    LogCache(Nl->GetCache());

    Link = rtnl_link_get_by_name(Nl->GetCache(), Name.c_str());
    if (!Link)
        return TError(EError::Unknown, string("Invalid link ") + Name);

    return TError::Success();
}

void TNlLink::LogObj(const std::string &prefix, void *obj) {
    static std::function<void(struct nl_dump_params *, char *)> handler;

    struct nl_dump_params dp = {};
    dp.dp_cb = [](struct nl_dump_params *params, char *buf) { handler(params, buf); };

    auto &str = L();
    handler = [&](struct nl_dump_params *params, char *buf) { str << buf; };

    if (Link)
        str << "netlink " << rtnl_link_get_name(Link) << ": " << prefix << " ";
    else
        str << "netlink: " << prefix << " ";
    nl_object_dump(OBJ_CAST(obj), &dp);
}

void TNlLink::LogCache(struct nl_cache *cache) {
    if (!debug)
        return;

    static std::function<void(struct nl_dump_params *, char *)> handler;

    struct nl_dump_params dp = {};
    dp.dp_cb = [](struct nl_dump_params *params, char *buf) { handler(params, buf); };
    dp.dp_type = NL_DUMP_DETAILS;

    auto &str = L();
    handler = [&](struct nl_dump_params *params, char *buf) { str << buf; };

    if (Link)
        str << "netlink " << rtnl_link_get_name(Link) << " cache: ";
    else
        str << "netlink cache: ";
    nl_cache_dump(cache, &dp);
}

TError TNlClass::Create(uint32_t prio, uint32_t rate, uint32_t ceil) {
    TError error = TError::Success();
    int ret;
    struct rtnl_class *tclass;

    if (!rate)
        return TError(EError::Unknown, string("tc classifier rate is not specified"));

    tclass = rtnl_class_alloc();
    if (!tclass)
        return TError(EError::Unknown, string("Unable to allocate tclass object"));

    rtnl_tc_set_link(TC_CAST(tclass), Link->GetLink());
    rtnl_tc_set_parent(TC_CAST(tclass), Parent);
    rtnl_tc_set_handle(TC_CAST(tclass), Handle);

    ret = rtnl_tc_set_kind(TC_CAST(tclass), "htb");
    if (ret < 0) {
        error = TError(EError::Unknown, string("Unable to set HTB to tclass: ") + nl_geterror(ret));
        goto free_class;
    }

    rtnl_htb_set_rate(tclass, rate);

    if (prio)
        rtnl_htb_set_prio(tclass, prio);

    if (ceil)
        rtnl_htb_set_ceil(tclass, ceil);

    rtnl_htb_set_quantum(tclass, 10000);

    /*
       rtnl_htb_set_rbuffer(tclass, burst);
       rtnl_htb_set_cbuffer(tclass, cburst);
       */

    Link->LogObj("add", tclass);

    ret = rtnl_class_add(Link->GetSock(), tclass, NLM_F_CREATE);
    if (ret < 0)
        error = TError(EError::Unknown, "Unable to add tclass for link " + Link->GetAlias() + ": " + nl_geterror(ret));

free_class:
    rtnl_class_put(tclass);

    return error;
}

TError TNlClass::Remove() {
    TError error = TError::Success();
    int ret;
    struct rtnl_class *tclass;

    tclass = rtnl_class_alloc();
    if (!tclass)
        return TError(EError::Unknown, string("Unable to allocate tclass object"));

    rtnl_tc_set_link(TC_CAST(tclass), Link->GetLink());
    rtnl_tc_set_parent(TC_CAST(tclass), Parent);
    rtnl_tc_set_handle(TC_CAST(tclass), Handle);

    ret = rtnl_class_delete(Link->GetSock(), tclass);
    if (ret < 0)
        error = TError(EError::Unknown, string("Unable to remove tclass: ") + nl_geterror(ret));

    Link->LogObj("remove", tclass);

    rtnl_class_put(tclass);

    return error;
}

TError TNlClass::GetStat(ETclassStat stat, uint64_t &val) {
    int ret;
    struct nl_cache *classCache;
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

    ret = rtnl_class_alloc_cache(Link->GetSock(), Link->GetIndex(), &classCache);
    if (ret < 0)
        return TError(EError::Unknown, string("Unable to allocate class cache: ") + nl_geterror(ret));

    Link->LogCache(classCache);

    struct rtnl_class *tclass = rtnl_class_get(classCache, Link->GetIndex(), Handle);
    if (!tclass) {
        nl_cache_free(classCache);
        return TError(EError::Unknown, "Can't get class statistics");
    }

    val = rtnl_tc_get_stat(TC_CAST(tclass), (rtnl_tc_stat)rtnlStat);
    rtnl_class_put(tclass);
    nl_cache_free(classCache);

    return TError::Success();
}

TError TNlClass::GetProperties(uint32_t &prio, uint32_t &rate, uint32_t &ceil) {
    int ret;
    struct nl_cache *classCache;

    ret = rtnl_class_alloc_cache(Link->GetSock(), Link->GetIndex(), &classCache);
    if (ret < 0)
        return TError(EError::Unknown, string("Unable to allocate class cache: ") + nl_geterror(ret));

    Link->LogCache(classCache);

    struct rtnl_class *tclass = rtnl_class_get(classCache, Link->GetIndex(), Handle);
    if (!tclass) {
        nl_cache_free(classCache);
        return TError(EError::Unknown, "Can't find tc cass");
    }

    prio = rtnl_htb_get_prio(tclass);
    rate = rtnl_htb_get_rate(tclass);
    ceil = rtnl_htb_get_ceil(tclass);

    rtnl_class_put(tclass);
    nl_cache_free(classCache);

    return TError::Success();
}

bool TNlClass::Valid(uint32_t prio, uint32_t rate, uint32_t ceil) {
    int ret;
    struct nl_cache *classCache;
    bool valid = true;

    ret = rtnl_class_alloc_cache(Link->GetSock(),
                                 Link->GetIndex(),
                                 &classCache);
    if (ret < 0)
        return false;

    Link->LogCache(classCache);

    struct rtnl_class *tclass = rtnl_class_get(classCache, Link->GetIndex(), Handle);
    if (tclass) {
        if (rtnl_tc_get_link(TC_CAST(tclass)) != Link->GetLink())
            valid = false;
        else if (rtnl_tc_get_parent(TC_CAST(tclass)) != Parent)
            valid = false;
        else if (rtnl_tc_get_handle(TC_CAST(tclass)) != Handle)
            valid = false;
        else if (rtnl_tc_get_kind(TC_CAST(tclass)) != string("htb"))
            valid = false;
        else if (rtnl_htb_get_rate(tclass) != rate)
            valid = false;
        else if (prio && rtnl_htb_get_prio(tclass) != prio)
            valid = false;
        else if (valid && rtnl_htb_get_ceil(tclass) != ceil)
            valid = false;
    } else {
        valid = false;
    }

    rtnl_class_put(tclass);
    nl_cache_free(classCache);

    return valid;
}

bool TNlClass::Exists() {
    int ret;
    struct nl_cache *classCache;

    ret = rtnl_class_alloc_cache(Link->GetSock(),
                                 Link->GetIndex(),
                                 &classCache);
    if (ret < 0)
        return false;

    Link->LogCache(classCache);

    struct rtnl_class *tclass = rtnl_class_get(classCache,
                                               Link->GetIndex(),
                                               Handle);
    rtnl_class_put(tclass);
    nl_cache_free(classCache);

    return tclass != nullptr;
}

TError TNlHtb::Create(uint32_t defaultClass) {
    TError error = TError::Success();
    int ret;
    struct rtnl_qdisc *qdisc;

    qdisc = rtnl_qdisc_alloc();
    if (!qdisc)
        return TError(EError::Unknown, string("Unable to allocate qdisc object"));

    rtnl_tc_set_link(TC_CAST(qdisc), Link->GetLink());
    rtnl_tc_set_parent(TC_CAST(qdisc), Parent);
    rtnl_tc_set_handle(TC_CAST(qdisc), Handle);

    ret = rtnl_tc_set_kind(TC_CAST(qdisc), "htb");
    if (ret < 0) {
        error = TError(EError::Unknown, string("Unable to set qdisc type: ") + nl_geterror(ret));
        goto free_qdisc;
    }

    rtnl_htb_set_defcls(qdisc, TcHandle(1, defaultClass));
    rtnl_htb_set_rate2quantum(qdisc, 10);

    Link->LogObj("add", qdisc);

    ret = rtnl_qdisc_add(Link->GetSock(), qdisc, NLM_F_CREATE);
    if (ret < 0)
        error = TError(EError::Unknown, string("Unable to add qdisc: ") + nl_geterror(ret));

free_qdisc:
    rtnl_qdisc_put(qdisc);

    return error;
}

TError TNlHtb::Remove() {
    struct rtnl_qdisc *qdisc;

    qdisc = rtnl_qdisc_alloc();
    if (!qdisc)
        return TError(EError::Unknown, string("Unable to allocate qdisc object"));

    rtnl_tc_set_link(TC_CAST(qdisc), Link->GetLink());
    rtnl_tc_set_parent(TC_CAST(qdisc), Parent);

    Link->LogObj("remove", qdisc);

    rtnl_qdisc_delete(Link->GetSock(), qdisc);
    rtnl_qdisc_put(qdisc);

    return TError::Success();
}

bool TNlHtb::Exists() {
    int ret;
    struct nl_cache *qdiscCache;
    bool exists;

    ret = rtnl_qdisc_alloc_cache(Link->GetSock(), &qdiscCache);
    if (ret < 0)
        return false;

    Link->LogCache(qdiscCache);

    struct rtnl_qdisc *qdisc = rtnl_qdisc_get(qdiscCache, Link->GetIndex(), Handle);
    exists = qdisc != nullptr;
    rtnl_qdisc_put(qdisc);
    nl_cache_free(qdiscCache);

    return exists;
}

bool TNlHtb::Valid(uint32_t defaultClass) {
    int ret;
    struct nl_cache *qdiscCache;
    bool valid = true;

    ret = rtnl_qdisc_alloc_cache(Link->GetSock(), &qdiscCache);
    if (ret < 0)
        return false;

    Link->LogCache(qdiscCache);

    struct rtnl_qdisc *qdisc = rtnl_qdisc_get(qdiscCache, Link->GetIndex(), Handle);
    if (qdisc) {
        if (rtnl_tc_get_link(TC_CAST(qdisc)) != Link->GetLink())
            valid = false;
        else if (rtnl_tc_get_parent(TC_CAST(qdisc)) != Parent)
            valid = false;
        else if (rtnl_tc_get_handle(TC_CAST(qdisc)) != Handle)
            valid = false;
        else if (rtnl_tc_get_kind(TC_CAST(qdisc)) != string("htb"))
            valid = false;
        else if (rtnl_htb_get_defcls(qdisc) != defaultClass)
            valid = false;
    } else {
        valid = false;
    }

    rtnl_qdisc_put(qdisc);
    nl_cache_free(qdiscCache);

    return valid;
}

TError TNlCgFilter::Create() {
    TError error = TError::Success();
    struct nl_msg *msg;
    int ret;
	struct tcmsg tchdr;

    tchdr.tcm_family = AF_UNSPEC;
    tchdr.tcm_ifindex = Link->GetIndex();
    tchdr.tcm_handle = Handle;
    tchdr.tcm_parent = Parent;
	tchdr.tcm_info = TC_H_MAKE(FilterPrio << 16, htons(ETH_P_IP));

	msg = nlmsg_alloc_simple(RTM_NEWTFILTER, NLM_F_EXCL|NLM_F_CREATE);
	if (!msg)
        return TError(EError::Unknown, "Unable to add filter: no memory");

    ret = nlmsg_append(msg, &tchdr, sizeof(tchdr), NLMSG_ALIGNTO);
    if (ret < 0) {
        error = TError(EError::Unknown, string("Unable to add filter: ") + nl_geterror(ret));
		goto free_msg;
    }

    ret = nla_put(msg, TCA_KIND, strlen(FilterType) + 1, FilterType);
    if (ret < 0) {
        error = TError(EError::Unknown, string("Unable to add filter: ") + nl_geterror(ret));
		goto free_msg;
    }

    ret = nla_put(msg, TCA_OPTIONS, 0, NULL);
    if (ret < 0) {
        error = TError(EError::Unknown, string("Unable to add filter: ") + nl_geterror(ret));
		goto free_msg;
    }

    L() << "netlink " << rtnl_link_get_name(Link->GetLink()) << ": create tfilter id 0x" << std::hex << Handle << " parent 0x" << Parent << std::dec  << std::endl;

    ret = nl_send_sync(Link->GetSock(), msg);
    if (ret) {
        error = TError(EError::Unknown, string("Unable to add filter: ") + nl_geterror(ret));
        goto free_msg;
    }

    if (!Exists())
        error = TError(EError::Unknown, "BUG: created filter doesn't exist");

    return error;

free_msg:
	nlmsg_free(msg);

    return error;
}

bool TNlCgFilter::Exists() {
    int ret;
    struct nl_cache *clsCache;

    ret = rtnl_cls_alloc_cache(Link->GetSock(), Link->GetIndex(), Parent, &clsCache);
    if (ret < 0) {
        L() << "Can't allocate filter cache: " << nl_geterror(ret) << std::endl;
        return false;
    }

    Link->LogCache(clsCache);

    struct CgFilterIter {
        uint32_t parent;
        uint32_t handle;
        bool exists;
    } data = { Parent, Handle, false };
    nl_cache_foreach(clsCache, [](struct nl_object *obj, void *data) {
                     CgFilterIter *p = (CgFilterIter *)data;
                     if (rtnl_tc_get_handle(TC_CAST(obj)) == p->handle &&
                         rtnl_tc_get_parent(TC_CAST(obj)) == p->parent)
                         p->exists = true;
                     }, &data);

    nl_cache_free(clsCache);
    return data.exists;
}

TError TNlCgFilter::Remove() {
    TError error = TError::Success();
    struct rtnl_cls *cls;
    int ret;

    cls = rtnl_cls_alloc();
    if (!cls)
        return TError(EError::Unknown, string("Unable to allocate filter object"));

    rtnl_tc_set_link(TC_CAST(cls), Link->GetLink());
    rtnl_tc_set_handle(TC_CAST(cls), Handle);

    ret = rtnl_tc_set_kind(TC_CAST(cls), FilterType);
    if (ret < 0) {
        error = TError(EError::Unknown, string("Unable to set filter type: ") + nl_geterror(ret));
        goto free_cls;
    }

    rtnl_cls_set_prio(cls, FilterPrio);
    rtnl_cls_set_protocol(cls, ETH_P_IP);
    rtnl_tc_set_parent(TC_CAST(cls), Parent);

    Link->LogObj("remove", cls);

    ret = rtnl_cls_delete(Link->GetSock(), cls, 0);
    if (ret < 0)
        error = TError(EError::Unknown, string("Unable to remove filter: ") + nl_geterror(ret));

free_cls:
    rtnl_cls_put(cls);

    return error;
}

TNlAddr::TNlAddr(const TNlAddr &other) {
    if (other.Addr)
        Addr = nl_addr_clone(other.Addr);
    else
        Addr = nullptr;
}

TNlAddr &TNlAddr::operator=(const TNlAddr &other) {
    if (this != &other) {
        nl_addr_put(Addr);
        if (other.Addr)
            Addr = nl_addr_clone(other.Addr);
        else
            Addr = nullptr;
    }
    return *this;
}

TNlAddr::~TNlAddr() {
    nl_addr_put(Addr);
}

bool TNlAddr::IsEmpty() {
    return !Addr || nl_addr_iszero(Addr);
}

TError TNlAddr::Parse(const std::string &s) {
    nl_addr_put(Addr);
    Addr = nullptr;

    int ret = nl_addr_parse(s.c_str(), AF_UNSPEC, &Addr);
    if (ret)
        return TError(EError::InvalidValue, "Unable to parse IP address " + s);

    return TError::Success();
}

TError ParseIpPrefix(const std::string &s, TNlAddr &addr, int &prefix) {
    std::vector<std::string> lines;
    TError error = SplitString(s, '/', lines);
    if (error)
        return error;

    if (lines.size() != 2)
        return TError(EError::InvalidValue, "Invalid IP address/prefix " + s);

    error = addr.Parse(lines[0]);
    if (error)
        return error;

    error = StringToInt(lines[1], prefix);
    if (error)
        return TError(EError::InvalidValue, "Invalid IP address/prefix " + s);

    return TError::Success();
}
