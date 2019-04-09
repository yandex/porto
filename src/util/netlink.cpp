#include <sstream>
#include <algorithm>
#include <cmath>

#include "netlink.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "config.hpp"

// HTB shaping details:
// http://luxik.cdi.cz/~devik/qos/htb/manual/userg.htm

extern "C" {
#include <unistd.h>
#include <linux/if.h>
#include <linux/if_ether.h>
#include <linux/if_addrlabel.h>
#include <linux/pkt_sched.h>
#include <netinet/ether.h>
#include <netlink/route/class.h>
#include <netlink/route/classifier.h>
#include <netlink/route/cls/cgroup.h>
#include <netlink/route/cls/u32.h>
#include <netlink/route/qdisc.h>
#include <netlink/route/qdisc/fifo.h>
#include <netlink/route/qdisc/htb.h>
#include <netlink/route/qdisc/sfq.h>
#include <netlink/route/qdisc/fq_codel.h>
#define class cls
#include <netlink/route/qdisc/hfsc.h>
#undef class

#include <netlink/route/rtnl.h>
#include <netlink/route/link.h>
#include <netlink/route/link/macvlan.h>
#include <netlink/route/link/veth.h>
#include <netlink/route/neighbour.h>

#include <netlink/route/route.h>
#include <netlink/route/addr.h>

/* FIXME: kludge for libnl3-3.2.27, remove this and
   above one after upgrade to > 3.2.29 or libnl3 drop */
#define _cplusplus __cplusplus
#include <netlink/route/link/ip6tnl.h>
#undef _cplusplus
}

#ifndef TC_LINKLAYER_MASK
# define TC_LINKLAYER_ETHERNET 1
# define linklayer __reserved
#endif

uint32_t TcHandle(uint16_t maj, uint16_t min) {
    return TC_HANDLE(maj, min);
}

TError TNl::Error(int nl_err, const std::string &prefix) {
    std::string desc = prefix + ": " + std::string(nl_geterror(nl_err));

    switch (abs(nl_err)) {
        case NLE_OBJ_NOTFOUND:
            return TError(EError::Unknown, ENOENT, desc);
        case NLE_NODEV:
            return TError(EError::Unknown, ENODEV, desc);
        default:
            return TError(EError::Unknown, desc);
    }
}

void TNl::Dump(const std::string &prefix, void *obj) const {
    std::stringstream ss;
    struct nl_dump_params dp = {};

    dp.dp_type = Verbose ? NL_DUMP_STATS : NL_DUMP_LINE;
    dp.dp_data = &ss;
    dp.dp_cb = [](struct nl_dump_params *dp, char *buf) {
            auto ss = (std::stringstream *)dp->dp_data;
            *ss << std::string(buf);
    };
    nl_object_dump(OBJ_CAST(obj), &dp);
    L_NL("{} {}", prefix, StringReplaceAll(ss.str(), "\n", " "));
}

TError TNl::Connect() {
    int ret;
    TError error;

    Disconnect();

    Sock = nl_socket_alloc();
    if (!Sock)
        return TError("Cannot allocate netlink socket");

    /*
    nl_socket_modify_cb(Sock, NL_CB_MSG_IN, NL_CB_DEBUG, nullptr, nullptr);
    nl_socket_modify_cb(Sock, NL_CB_MSG_OUT, NL_CB_DEBUG, nullptr, nullptr);
    */

    ret = nl_connect(Sock, NETLINK_ROUTE);
    if (ret < 0) {
        nl_socket_free(Sock);
        return Error(ret, "Cannot connect netlink socket");
    }

    return OK;
}

void TNl::Disconnect() {
    if (Sock) {
        nl_close(Sock);
        nl_socket_free(Sock);
        Sock = nullptr;
    }
}

TError TNl::ProxyNeighbour(int ifindex, const TNlAddr &addr, bool add) {
    struct rtnl_neigh *neigh;
    int ret;

    neigh = rtnl_neigh_alloc();
    if (!neigh)
        return TError("Cannot allocate neighbour");

    ret = rtnl_neigh_set_dst(neigh, addr.Addr);
    if (ret) {
        rtnl_neigh_put(neigh);
        return Error(ret, "Cannot set neighbour dst");
    }

    rtnl_neigh_set_flags(neigh, NTF_PROXY);
    rtnl_neigh_set_state(neigh, NUD_PERMANENT);
    rtnl_neigh_set_ifindex(neigh, ifindex);

    if (add) {
        Dump("add", neigh);
        ret = rtnl_neigh_add(Sock, neigh, NLM_F_CREATE | NLM_F_REPLACE);
    } else {
        Dump("del", neigh);
        ret = rtnl_neigh_delete(Sock, neigh, 0);

        if (ret == -NLE_OBJ_NOTFOUND)
            ret = 0;
    }
    rtnl_neigh_put(neigh);
    if (ret)
        return Error(ret, "Cannot modify neighbour for l3 network");

    return OK;
}

TError TNl::PermanentNeighbour(int ifindex, const TNlAddr &addr,
                               const TNlAddr &lladdr, bool add) {
    struct rtnl_neigh *neigh;
    int ret;

    neigh = rtnl_neigh_alloc();
    if (!neigh)
        return TError("Cannot allocate neighbour");

    ret = rtnl_neigh_set_dst(neigh, addr.Addr);
    if (ret) {
        rtnl_neigh_put(neigh);
        return Error(ret, "Cannot set neighbour dst");
    }

    rtnl_neigh_set_lladdr(neigh, lladdr.Addr);
    rtnl_neigh_set_state(neigh, NUD_PERMANENT);
    rtnl_neigh_set_ifindex(neigh, ifindex);

    if (add) {
        Dump("add", neigh);
        ret = rtnl_neigh_add(Sock, neigh, NLM_F_CREATE | NLM_F_REPLACE);
    } else {
        Dump("del", neigh);
        ret = rtnl_neigh_delete(Sock, neigh, 0);

        if (ret == -NLE_OBJ_NOTFOUND)
            ret = 0;
    }
    rtnl_neigh_put(neigh);
    if (ret)
        return Error(ret, "Cannot modify neighbour entry");

    return OK;
}

TError TNl::AddrLabel(const TNlAddr &prefix, uint32_t label) {
    struct ifaddrlblmsg al;
    struct nl_msg *msg;
    TError error;
    int ret;

    L_NL("add addrlabel {} {}", prefix.Format(), label);

    memset(&al, 0, sizeof(al));
    al.ifal_family = prefix.Family();
    al.ifal_prefixlen = prefix.Prefix();

    msg = nlmsg_alloc_simple(RTM_NEWADDRLABEL, NLM_F_EXCL|NLM_F_CREATE);
    if (!msg)
        return TError("nlmsg_alloc_simple addrlabel");

    ret = nlmsg_append(msg, &al, sizeof(al), NLMSG_ALIGNTO);
    if (ret < 0) {
        error = Error(ret, "nlmsg_append addrlabel");
        goto free_msg;
    }

    ret = nla_put(msg, IFAL_ADDRESS, prefix.Length(), prefix.Binary());
    if (ret < 0) {
        error = Error(ret, "nla_put IFAL_ADDRESS");
        goto free_msg;
    }

    ret = nla_put(msg, IFAL_LABEL, sizeof(label), &label);
    if (ret < 0) {
        error = Error(ret, "nla_put IFAL_ADDRESS");
        goto free_msg;
    }

    ret = nl_send_sync(Sock, msg);
    if (ret)
        error = Error(ret, "nl_send_sync addrlabel");
    msg = nullptr;

free_msg:
    nlmsg_free(msg);
    return error;
}

int TNl::GetFd() {
    return nl_socket_get_fd(Sock);
}


TNlLink::TNlLink(std::shared_ptr<TNl> sock, const std::string &name, int index) {
    Nl = sock;
    Link = rtnl_link_alloc();
    PORTO_ASSERT(Link);
    rtnl_link_set_name(Link, name.c_str());
    if (index)
        rtnl_link_set_ifindex(Link, index);
}

TNlLink::TNlLink(std::shared_ptr<TNl> sock, struct rtnl_link *link) {
    Nl = sock;
    Link = link;
    nl_object_get(OBJ_CAST(Link));
}

TNlLink::~TNlLink() {
    if (Link)
        rtnl_link_put(Link);
}

TError TNlLink::Load() {
    struct rtnl_link *link;
    int ret;

    ret = rtnl_link_get_kernel(GetSock(), rtnl_link_get_ifindex(Link),
                                       rtnl_link_get_name(Link), &link);
    if (ret)
        return Error(ret, "Cannot load link");
    rtnl_link_put(Link);
    Link = link;
    return OK;
}

int TNlLink::GetIndex() const {
    return rtnl_link_get_ifindex(Link);
}

int TNlLink::GetGroup() const {
    return rtnl_link_get_group(Link);
}

TNlAddr TNlLink::GetAddr() const {
    return TNlAddr(rtnl_link_get_addr(Link));
}

std::string TNlLink::GetName() const {
    return std::string(rtnl_link_get_name(Link) ?: "???");
}

std::string TNlLink::GetType() const {
    return std::string(rtnl_link_get_type(Link) ?: "");
}

std::string TNlLink::GetDesc() const {
    return std::to_string(GetIndex()) + ":" + GetName();
}

bool TNlLink::IsLoopback() const {
    return rtnl_link_get_flags(Link) & IFF_LOOPBACK;
}

bool TNlLink::IsRunning() const {
    return rtnl_link_get_flags(Link) & IFF_RUNNING;
}

TError TNlLink::Error(int nl_err, const std::string &desc) const {
    return TNl::Error(nl_err, GetDesc() + " " + desc);
}

void TNlLink::Dump(const std::string &prefix, void *obj) const {
    if (obj)
        Nl->Dump(GetDesc() + " " + prefix, obj);
    else
        Nl->Dump(prefix, Link);
}

TError TNlLink::Up() {
    Dump("up");

    auto change = rtnl_link_alloc();
    if (!change)
        return Error(-NLE_NOMEM, "Cannot allocate link");
    rtnl_link_set_flags(change, IFF_UP);
    int ret = rtnl_link_change(GetSock(), Link, change, 0);
    rtnl_link_put(change);
    if (ret < 0)
        return Error(ret, "Cannot set up");
    return OK;
}

TError TNlLink::Remove() {
    Dump("remove");
    int ret = rtnl_link_delete(GetSock(), Link);
    if (ret)
        return Error(ret, "Cannot remove");
    return OK;
}

TError TNlLink::ChangeNs(const std::string &newName, int nsFd) {
    auto change = rtnl_link_alloc();
    if (!change)
        return Error(-NLE_NOMEM, "Cannot allocate link");
    rtnl_link_set_name(change, newName.c_str());
    rtnl_link_set_ns_fd(change, nsFd);
    Dump("change ns", change);
    int ret = rtnl_link_change(GetSock(), Link, change, 0);
    rtnl_link_put(change);
    if (ret < 0)
        return Error(ret, "Cannot change ns");
    return OK;
}

TError TNlLink::AddDirectRoute(const TNlAddr &addr, bool ecn) {
    struct rtnl_route *route;
    struct rtnl_nexthop *nh;
    int ret;

    route = rtnl_route_alloc();
    if (!route)
        return TError("Cannot allocate route");

    ret = rtnl_route_set_dst(route, addr.Addr);
    if (ret < 0) {
        rtnl_route_put(route);
        return Error(ret, "Cannot set route destination");
    }

    nh = rtnl_route_nh_alloc();
    if (!nh) {
        rtnl_route_put(route);
        return TError("Cannot allocate next hop");
    }

    rtnl_route_nh_set_ifindex(nh, GetIndex());
    rtnl_route_add_nexthop(route, nh);

    if (ecn) {
        ret = rtnl_route_set_metric(route, RTAX_FEATURES, RTAX_FEATURE_ECN);
        if (ret < 0)
            return Error(ret, "Cannot enable ECN");
    }

    Dump("add", route);
    ret = rtnl_route_add(GetSock(), route, NLM_F_CREATE | NLM_F_REPLACE);
    rtnl_route_put(route);
    if (ret < 0)
        return Error(ret, "Cannot add direct route");

    return OK;
}

TError TNlLink::SetDefaultGw(const TNlAddr &addr, bool ecn, int mtu) {
    struct rtnl_route *route;
    struct rtnl_nexthop *nh;
    TError error;
    TNlAddr all;
    int ret;

    error = all.Parse(addr.Family(), "default");
    if (error)
        return error;

    route = rtnl_route_alloc();
    if (!route)
        return TError("Unable to allocate route");

    ret = rtnl_route_set_dst(route, all.Addr);
    if (ret < 0) {
        rtnl_route_put(route);
        return Error(ret, "Cannot set route destination");
    }

    nh = rtnl_route_nh_alloc();
    if (!nh) {
        rtnl_route_put(route);
        return TError("Unable to allocate next hop");
    }

    rtnl_route_nh_set_gateway(nh, addr.Addr);
    rtnl_route_nh_set_ifindex(nh, GetIndex());
    rtnl_route_add_nexthop(route, nh);

    if (ecn) {
        ret = rtnl_route_set_metric(route, RTAX_FEATURES, RTAX_FEATURE_ECN);
        if (ret < 0)
            return Error(ret, "Cannot enable ECN");
    }

    if (mtu > 0) {
        ret = rtnl_route_set_metric(route, RTAX_MTU, mtu);
        if (ret < 0)
            return Error(ret, "Cannot set default gateway mtu");
    }

    Dump("add", route);
    ret = rtnl_route_add(GetSock(), route, NLM_F_MATCH);
    rtnl_route_put(route);
    if (ret < 0)
        return Error(ret, "Cannot set default gateway");

    return OK;
}

TError TNlLink::AddAddress(const TNlAddr &addr) {
    struct rtnl_addr *a = rtnl_addr_alloc();
    if (!a)
        return TError("Cannot allocate address");

    rtnl_addr_set_link(a, Link);
    rtnl_addr_set_family(a, nl_addr_get_family(addr.Addr));
    rtnl_addr_set_flags(a, IFA_F_NODAD);

    int ret = rtnl_addr_set_local(a, addr.Addr);
    if (ret < 0) {
        rtnl_addr_put(a);
        return Error(ret, "Cannot set local address");
    }

    ret = rtnl_addr_add(GetSock(), a, 0);
    if (ret < 0) {
        rtnl_addr_put(a);
        return Error(ret, "Cannot add address");
    }

    rtnl_addr_put(a);

    return OK;
}

TError TNlLink::WaitAddress(int timeout_s) {
    struct nl_cache *cache;
    int ret;

    L_NET("Wait for autoconf at {}", GetDesc());

    ret = rtnl_addr_alloc_cache(GetSock(), &cache);
    if (ret < 0)
        return Nl->Error(ret, "Cannot allocate addr cache");

    do {
        for (auto obj = nl_cache_get_first(cache); obj; obj = nl_cache_get_next(obj)) {
            auto addr = (struct rtnl_addr *)obj;

            if (!rtnl_addr_get_local(addr) ||
                    rtnl_addr_get_ifindex(addr) != GetIndex() ||
                    rtnl_addr_get_family(addr) != AF_INET6 ||
                    rtnl_addr_get_scope(addr) >= RT_SCOPE_LINK ||
                    (rtnl_addr_get_flags(addr) &
                     (IFA_F_TENTATIVE | IFA_F_DEPRECATED)))
                continue;

            L_NET("Got {} at {}", TNlAddr(rtnl_addr_get_local(addr)).Format(), GetDesc());

            nl_cache_free(cache);
            return OK;
        }

        usleep(1000000);
        ret = nl_cache_refill(GetSock(), cache);
        if (ret < 0)
            return Nl->Error(ret, "Cannot refill address cache");
    } while (--timeout_s > 0);

    nl_cache_free(cache);
    return TError("Network autoconf timeout");
}

#ifdef IFLA_IPVLAN_MAX
static const std::map<std::string, int> ipvlanMode = {
    { "l2", IPVLAN_MODE_L2 },
    { "l3", IPVLAN_MODE_L3 },
};
#endif

static const std::map<std::string, int> macvlanType = {
    { "private", MACVLAN_MODE_PRIVATE },
    { "vepa", MACVLAN_MODE_VEPA },
    { "bridge", MACVLAN_MODE_BRIDGE },
    { "passthru", MACVLAN_MODE_PASSTHRU },
};

TError TNlLink::AddXVlan(const std::string &vlantype,
                         const std::string &master,
                         uint32_t type,
                         const std::string &hw,
                         int mtu) {
    TError error = OK;
    int ret;
    uint32_t masterIdx;
    struct nl_msg *msg;
    struct nlattr *linkinfo, *infodata;
    struct ifinfomsg ifi = {};
    struct ether_addr ea;
    auto Name = GetName();

    if (hw.length() && !ether_aton_r(hw.c_str(), &ea))
        return TError("Invalid " + vlantype + " mac address " + hw);

    TNlLink masterLink(Nl, master);
    error = masterLink.Load();
    if (error)
        return error;
    masterIdx = masterLink.GetIndex();
    int masterGroup = masterLink.GetGroup();

    msg = nlmsg_alloc_simple(RTM_NEWLINK, NLM_F_CREATE);
    if (!msg)
        return TError("Unable to add " + vlantype + ": no memory");

    ret = nlmsg_append(msg, &ifi, sizeof(ifi), NLMSG_ALIGNTO);
    if (ret < 0) {
        error = TError("Unable to add " + vlantype + ": " + nl_geterror(ret));
        goto free_msg;
    }

    /* link configuration */
    ret = nla_put(msg, IFLA_LINK, sizeof(uint32_t), &masterIdx);
    if (ret < 0) {
        error = TError(EError::Unknown, std::string("Unable to put IFLA_LINK: ") + nl_geterror(ret));
        goto free_msg;
    }
    ret = nla_put(msg, IFLA_IFNAME, Name.length() + 1, Name.c_str());
    if (ret < 0) {
        error = TError(EError::Unknown, std::string("Unable to put IFLA_IFNAME: ") + nl_geterror(ret));
        goto free_msg;
    }

    if (mtu > 0) {
        ret = nla_put(msg, IFLA_MTU, sizeof(int), &mtu);
        if (ret < 0) {
            error = TError(EError::Unknown, std::string("Unable to put IFLA_MTU: ") + nl_geterror(ret));
            goto free_msg;
        }
    }

    ret = nla_put(msg, IFLA_GROUP, sizeof(int), &masterGroup);
    if (ret < 0) {
        error = TError(EError::Unknown, std::string("Unable to put IFLA_GROUP: ") + nl_geterror(ret));
        goto free_msg;
    }

    if (hw.length()) {
        struct nl_addr *addr = nl_addr_build(AF_LLC, &ea, ETH_ALEN);
        ret = nla_put(msg, IFLA_ADDRESS, nl_addr_get_len(addr), nl_addr_get_binary_addr(addr));
        if (ret < 0) {
            error = TError(EError::Unknown, std::string("Unable to put IFLA_ADDRESS: ") + nl_geterror(ret));
            goto free_msg;
        }
        nl_addr_put(addr);
    }

    /* link type */
    linkinfo = nla_nest_start(msg, IFLA_LINKINFO);
    if (!linkinfo) {
        error = TError("Unable to add " + vlantype + ": can't nest IFLA_LINKINFO");
        goto free_msg;
    }
    ret = nla_put(msg, IFLA_INFO_KIND, vlantype.length() + 1, vlantype.c_str());
    if (ret < 0) {
        error = TError(EError::Unknown, std::string("Unable to put IFLA_INFO_KIND: ") + nl_geterror(ret));
        goto free_msg;
    }

    /* xvlan specific */
    infodata = nla_nest_start(msg, IFLA_INFO_DATA);
    if (!infodata) {
        error = TError("Unable to add " + vlantype + ": can't nest IFLA_INFO_DATA");
        goto free_msg;
    }

    if (vlantype == "macvlan") {
        ret = nla_put(msg, IFLA_MACVLAN_MODE, sizeof(uint32_t), &type);
        if (ret < 0) {
            error = TError(EError::Unknown, std::string("Unable to put IFLA_MACVLAN_MODE: ") + nl_geterror(ret));
            goto free_msg;
        }
#ifdef IFLA_IPVLAN_MAX
    } else if (vlantype == "ipvlan") {
        uint16_t mode = type;
        ret = nla_put(msg, IFLA_IPVLAN_MODE, sizeof(uint16_t), &mode);
        if (ret < 0) {
            error = TError(EError::Unknown, std::string("Unable to put IFLA_IPVLAN_MODE: ") + nl_geterror(ret));
            goto free_msg;
        }
#endif
    }
    nla_nest_end(msg, infodata);
    nla_nest_end(msg, linkinfo);

    L_NL("add {} {} master {} type {} hw {} mtu {}", vlantype, Name,  master,
      type, hw, mtu);

    ret = nl_send_sync(GetSock(), msg);
    if (ret)
        return Error(ret, "Cannot add " + vlantype);

    return Load();

free_msg:
    nlmsg_free(msg);
    return error;

}

TError TNlLink::AddIpVlan(const std::string &master,
                          const std::string &mode, int mtu) {
#ifdef IFLA_IPVLAN_MAX
    return AddXVlan("ipvlan", master, ipvlanMode.at(mode), "", mtu);
#else
    return TError(EError::NotSupported, "Porto is not compiled with IP VLAN support");
#endif
}

TError TNlLink::AddMacVlan(const std::string &master,
                           const std::string &type, const std::string &hw,
                           int mtu) {
    return AddXVlan("macvlan", master, macvlanType.at(type), hw, mtu);
}

TError TNlLink::Enslave(const std::string &name) {
    struct rtnl_link *link;
    int ret;

    link = rtnl_link_alloc();
    rtnl_link_set_name(link, name.c_str());

    rtnl_link_set_master(link, GetIndex());
    rtnl_link_set_flags(link, IFF_UP);

    Dump("mod", link);
    ret = rtnl_link_change(GetSock(), link, link, 0);
    if (ret < 0) {
        Dump("del", link);
        (void)rtnl_link_delete(GetSock(), link);
        rtnl_link_put(link);
        return Error(ret, "Cannot enslave interface " + name);
    }

    rtnl_link_put(link);
    return OK;
}

TError TNlLink::AddVeth(const std::string &name,
                        const std::string &hw,
                        int mtu, int group, int nsFd) {
    struct rtnl_link *veth, *peer;
    int ret;

    peer = rtnl_link_veth_alloc();
    if (!peer)
        return TError("Unable to allocate veth");

    rtnl_link_set_name(peer, rtnl_link_get_name(Link));

    veth = rtnl_link_veth_get_peer(peer);
    rtnl_link_set_name(veth, name.c_str());

    if (nsFd >= 0)
        rtnl_link_set_ns_fd(veth, nsFd);

    if (mtu > 0) {
        rtnl_link_set_mtu(peer, mtu);
        rtnl_link_set_mtu(veth, mtu);
    }

    if (group)  {
        rtnl_link_set_group(peer, group);
        rtnl_link_set_group(veth, group);
    }

    if (!hw.empty()) {
        TNlAddr addr;
        TError error = addr.Parse(AF_LLC, hw.c_str());
        if (error)
            return error;
        rtnl_link_set_addr(veth, addr.Addr);
    }

    rtnl_link_set_flags(peer, IFF_UP);

    Dump("add", veth);
    rtnl_link_put(veth);

    Dump("add", peer);
    ret = rtnl_link_add(GetSock(), peer, NLM_F_CREATE | NLM_F_EXCL);
    if (ret < 0) {
        rtnl_link_put(peer);
        return Error(ret, "Cannot add veth");
    }

    rtnl_link_put(peer);

    return Load();
}

TError TNlLink::AddIp6Tnl(const std::string &name,
                          const TNlAddr &remote, const TNlAddr &local,
                          int type, int mtu, int encap_limit, int ttl,
                          int tx_queues) {

    rtnl_link_set_type(Link, "ip6tnl");

    rtnl_link_ip6_tnl_set_proto(Link, type);
    rtnl_link_ip6_tnl_set_remote(Link, (struct in6_addr *)remote.Binary());
    rtnl_link_ip6_tnl_set_local(Link, (struct in6_addr *)local.Binary());
    rtnl_link_ip6_tnl_set_encaplimit(Link, encap_limit);
    rtnl_link_ip6_tnl_set_ttl(Link, ttl);
    rtnl_link_set_mtu(Link, mtu);

    if (tx_queues)
        rtnl_link_set_num_tx_queues(Link, tx_queues);

    Dump("add", Link);

    int ret = rtnl_link_add(GetSock(), Link, NLM_F_CREATE | NLM_F_EXCL);

    if (ret)
        return Error(ret, "Cannot add ip6tnl " + name);

    this->Load();

    return SetMtu(mtu);
}

int TNlLink::GetMtu() {
    return rtnl_link_get_mtu(Link);
}

TError TNlLink::SetMtu(int mtu) {
    auto link_mtu = rtnl_link_alloc();
    rtnl_link_set_name(link_mtu, rtnl_link_get_name(Link));
    rtnl_link_set_mtu(link_mtu, mtu);

    int ret = rtnl_link_change(GetSock(), Link, link_mtu, 0);

    rtnl_link_put(link_mtu);

    if (ret)
        return Error(ret, "Cannot set mtu for " + GetName());

    return OK;
}

TError TNlLink::SetGroup(int group) {
    auto link = rtnl_link_alloc();
    rtnl_link_set_name(link, rtnl_link_get_name(Link));
    rtnl_link_set_group(link, group);

    int ret = rtnl_link_change(GetSock(), Link, link, 0);

    rtnl_link_put(link);

    if (ret)
        return Error(ret, "Cannot set group for " + GetName());

    return OK;
}

TError TNlLink::SetMacAddr(const std::string &mac) {
    TNlAddr addr;
    TError error = addr.Parse(AF_LLC, mac);
    if (error)
        return error;

    auto link = rtnl_link_alloc();
    rtnl_link_set_name(link, rtnl_link_get_name(Link));
    rtnl_link_set_addr(link, addr.Addr);

    int ret = rtnl_link_change(GetSock(), Link, link, 0);
    rtnl_link_put(link);

    if (ret)
        return Error(ret, "Cannot set mac for " + GetName());

    return OK;
}

bool TNlLink::ValidIpVlanMode(const std::string &mode) {
#ifdef IFLA_IPVLAN_MAX
    return ipvlanMode.find(mode) != ipvlanMode.end();
#else
    return false;
#endif
}

bool TNlLink::ValidMacVlanType(const std::string &type) {
    return macvlanType.find(type) != macvlanType.end();
}

bool TNlLink::ValidMacAddr(const std::string &hw) {
    struct ether_addr ea;
    return ether_aton_r(hw.c_str(), &ea) != nullptr;
}

/*void TNl::DumpCache(struct nl_cache *cache) const {

    if (!Verbose)
        return;

    static std::function<void(struct nl_dump_params *, char *)> handler;

    struct nl_dump_params dp = {};
    dp.dp_cb = [](struct nl_dump_params *params, char *buf) { handler(params, buf); };
    dp.dp_type = NL_DUMP_DETAILS;

    auto &str = L_NL();
    handler = [&](struct nl_dump_params *params, char *buf) { str << buf; };

    str << "netlink cache: ";
    nl_cache_dump(cache, &dp);
}*/

TError TNlClass::Load(const TNl &nl) {
    struct nl_cache *cache;
    struct rtnl_class *tclass;

    int ret = rtnl_class_alloc_cache(nl.GetSock(), Index, &cache);
    if (ret < 0)
            return nl.Error(ret, "Cannot allocate class cache");

    tclass = rtnl_class_get(cache, Index, Handle);
    if (!tclass) {
        nl_cache_free(cache);
        return TError("Can't find tc class");
    }

    Kind = rtnl_tc_get_kind(TC_CAST(tclass));

    if (Kind == "htb") {
        Rate = rtnl_htb_get_rate(tclass);
        Ceil = rtnl_htb_get_ceil(tclass);
    }

    if (Kind == "hfsc") {
        struct tc_service_curve sc;
        if (!rtnl_class_hfsc_get_rsc(tclass, &sc))
            Rate = sc.m2;
        if (!rtnl_class_hfsc_get_usc(tclass, &sc))
            Ceil = sc.m2;
    }

    rtnl_class_put(tclass);
    nl_cache_free(cache);

    return OK;
}

bool TNlClass::Exists(const TNl &nl) {
    return !Load(nl);
}

TError TNlQdisc::CreateCodel(const TNl &nl) {
    struct tcmsg tchdr;
    struct nl_msg *msg;
    struct nlattr *u32;
    TError error;
    int ret;

    L_NL("add qdisc codel dev {} id {:x} parent {:x} limit {} target {} interval {} ecn {} ce_threshold {}",
         Index, Handle, Parent, Limit,
         config().network().codel_target() ?: 5000,
         config().network().codel_interval() ?: 100000,
         config().network().codel_ecn(),
         config().network().codel_ce_threshold());

    msg = nlmsg_alloc_simple(RTM_NEWQDISC, NLM_F_CREATE | NLM_F_REPLACE);
    if (!msg)
        return TError(Kind);

    tchdr.tcm_family = AF_UNSPEC;
    tchdr.tcm_ifindex = Index;
    tchdr.tcm_handle = Handle;
    tchdr.tcm_parent = Parent;

    ret = nlmsg_append(msg, &tchdr, sizeof(tchdr), NLMSG_ALIGNTO);
    if (ret < 0)
        goto free_msg;

    ret = nla_put(msg, TCA_KIND, Kind.size() + 1, Kind.c_str());
    if (ret < 0)
        goto free_msg;

    u32 = nla_nest_start(msg, TCA_OPTIONS);
    if (!u32) {
        error = TError(EError::Unknown, "TCA_OPTIONS");
        goto free_msg;
    }

    if (Limit) {
        ret = nla_put_u32(msg, TCA_CODEL_LIMIT, Limit);
        if (ret < 0)
            goto free_msg;
    }

    if (config().network().has_codel_target()) {
        ret = nla_put_u32(msg, TCA_CODEL_TARGET, config().network().codel_target());
        if (ret < 0)
            goto free_msg;
    }

    if (config().network().has_codel_interval()) {
        ret = nla_put_u32(msg, TCA_CODEL_INTERVAL, config().network().codel_interval());
        if (ret < 0)
            goto free_msg;
    }

    if (config().network().has_codel_ecn()) {
        ret = nla_put_u32(msg, TCA_CODEL_ECN, config().network().codel_ecn());
        if (ret < 0)
            goto free_msg;
    }

    if (config().network().has_codel_ce_threshold()) {
        ret = nla_put_u32(msg, TCA_CODEL_CE_THRESHOLD, config().network().codel_ce_threshold());
        if (ret < 0)
            goto free_msg;
    }

    nla_nest_end(msg, u32);

    ret = nl_send_sync(nl.GetSock(), msg);
    if (ret)
        error = nl.Error(ret, Kind);

    return error;

free_msg:
    if (ret)
        error = nl.Error(ret, Kind);
    nlmsg_free(msg);

    return error;
}

TError TNlQdisc::Create(const TNl &nl) {
    TError error = OK;
    int ret;
    struct rtnl_qdisc *qdisc;

    if (Kind == "")
        return Delete(nl);

    if (Kind == "codel")
        return CreateCodel(nl);

    qdisc = rtnl_qdisc_alloc();
    if (!qdisc)
        return TError(EError::Unknown, std::string("Unable to allocate qdisc object"));

    rtnl_tc_set_ifindex(TC_CAST(qdisc), Index);
    rtnl_tc_set_parent(TC_CAST(qdisc), Parent);
    rtnl_tc_set_handle(TC_CAST(qdisc), Handle);

    ret = rtnl_tc_set_kind(TC_CAST(qdisc), Kind.c_str());
    if (ret < 0) {
        error = nl.Error(ret, "Cannot to set qdisc type: " + Kind);
        goto free_qdisc;
    }

    if (Kind == "bfifo" || Kind == "pfifo") {
        if (Limit)
            rtnl_qdisc_fifo_set_limit(qdisc, Limit);
    }

    if (Kind == "htb") {
        if (Default)
            rtnl_htb_set_defcls(qdisc, TC_H_MIN(Default));
        if (Quantum)
            rtnl_htb_set_rate2quantum(qdisc, Quantum);
    }

    if (Kind == "hfsc") {
        if (Default)
            rtnl_qdisc_hfsc_set_defcls(qdisc, TC_H_MIN(Default));
    }

    if (Kind == "sfq") {
        if (Limit)
            rtnl_sfq_set_limit(qdisc, Limit);
        if (Quantum)
            rtnl_sfq_set_quantum(qdisc, Quantum);
    }

    if (Kind == "fq_codel") {
        if (Limit)
            rtnl_qdisc_fq_codel_set_limit(qdisc, Limit);
        if (Quantum)
            rtnl_qdisc_fq_codel_set_quantum(qdisc, Quantum);
        if (config().network().has_codel_target())
            rtnl_qdisc_fq_codel_set_target(qdisc, config().network().codel_target());
        if (config().network().has_codel_interval())
            rtnl_qdisc_fq_codel_set_interval(qdisc, config().network().codel_interval());
        if (config().network().has_codel_ecn())
            rtnl_qdisc_fq_codel_set_ecn(qdisc, config().network().codel_ecn());
    }

    nl.Dump("create", qdisc);

    ret = rtnl_qdisc_add(nl.GetSock(), qdisc, NLM_F_CREATE  | NLM_F_REPLACE);
    if (ret < 0)
        error = nl.Error(ret, "Cannot create qdisc");

free_qdisc:
    rtnl_qdisc_put(qdisc);

    return error;
}

TError TNlQdisc::Delete(const TNl &nl) {
    struct rtnl_qdisc *qdisc;
    int ret;

    qdisc = rtnl_qdisc_alloc();
    if (!qdisc)
        return TError(EError::Unknown, std::string("Unable to allocate qdisc object"));

    rtnl_tc_set_ifindex(TC_CAST(qdisc), Index);
    rtnl_tc_set_parent(TC_CAST(qdisc), Parent);

    nl.Dump("remove", qdisc);
    ret = rtnl_qdisc_delete(nl.GetSock(), qdisc);
    rtnl_qdisc_put(qdisc);
    if (ret < 0)
        return nl.Error(ret, "Cannot remove qdisc");

    return OK;
}

bool TNlQdisc::Check(const TNl &nl) {
    struct nl_cache *qdiscCache;
    bool result = false;
    int ret;

    ret = rtnl_qdisc_alloc_cache(nl.GetSock(), &qdiscCache);
    if (ret < 0) {
        L_ERR("{}",  nl.Error(ret, "cannot alloc qdisc cache"));
        return false;
    }

    struct rtnl_qdisc *qdisc = rtnl_qdisc_get(qdiscCache, Index, Handle);
    if (!qdisc) {
        result = Kind == "";
        goto out;
    }

    nl.Dump("found", qdisc);

    if (rtnl_tc_get_ifindex(TC_CAST(qdisc)) != Index)
        goto out;

    if (rtnl_tc_get_parent(TC_CAST(qdisc)) != Parent)
        goto out;

    if (rtnl_tc_get_handle(TC_CAST(qdisc)) != Handle)
        goto out;

    if (rtnl_tc_get_kind(TC_CAST(qdisc)) != Kind)
        goto out;

    if (Kind == "htb") {
        if (rtnl_htb_get_defcls(qdisc) != TC_H_MIN(Default))
            goto out;
    }

    if (Kind == "hfsc") {
        if (rtnl_qdisc_hfsc_get_defcls(qdisc) != TC_H_MIN(Default))
            goto out;
    }

    result = true;

out:
    rtnl_qdisc_put(qdisc);
    nl_cache_free(qdiscCache);
    return result;
}

static uint32_t htb_burst_to_buffer(uint64_t burst, uint64_t speed) {
    uint32_t tick_per_ns = 64; // /proc/net/psched
    return std::min((double)burst * NSEC_PER_SEC / tick_per_ns / speed,
                    (double)UINT32_MAX);
}

TError TNlClass::CreateHTB(const TNl &nl, bool safe) {
    struct tcmsg tchdr;
    struct nl_msg *msg;
    struct nlattr *u32;
    TError error;
    int ret;

    L_NL("add class htb dev {} id {:x} parent {:x} rate {} ceil {} burst {} cburst {} quantum {} prio {}",
         Index, Handle, Parent, Rate ?: defRate, Ceil, RateBurst, CeilBurst, Quantum, Prio);

    if (safe && Handle != TC_HANDLE(ROOT_TC_MAJOR, 1)) {
        TNlClass cls(Index, TC_H_UNSPEC, Parent);

        if (!cls.Exists(nl))
            return TError(EError::Unknown, "parent class does not exists");
    }

    msg = nlmsg_alloc_simple(RTM_NEWTCLASS, NLM_F_CREATE | NLM_F_REPLACE);
    if (!msg)
        return TError(Kind);

    tchdr.tcm_family = AF_UNSPEC;
    tchdr.tcm_ifindex = Index;
    tchdr.tcm_handle = Handle;
    tchdr.tcm_parent = Parent;

    ret = nlmsg_append(msg, &tchdr, sizeof(tchdr), NLMSG_ALIGNTO);
    if (ret < 0)
        goto free_msg;

    ret = nla_put(msg, TCA_KIND, Kind.size() + 1, Kind.c_str());
    if (ret < 0)
        goto free_msg;

    struct tc_htb_opt opts;

    memset(&opts, 0, sizeof(opts));

    opts.prio = Prio;
    opts.quantum = Quantum;

    if (Rate) {
        opts.rate.rate = std::min(Rate, (uint64_t)UINT32_MAX);
        opts.buffer = htb_burst_to_buffer(RateBurst, Rate);
    } else {
        opts.rate.rate = defRate;
        opts.buffer = htb_burst_to_buffer(RateBurst, defRate);
    }
    opts.rate.linklayer = TC_LINKLAYER_ETHERNET;

    if (Ceil) {
        opts.ceil.rate = std::min(Ceil, (uint64_t)UINT32_MAX);
        opts.cbuffer = htb_burst_to_buffer(CeilBurst, Ceil);
    } else {
        opts.ceil.rate = UINT32_MAX;
        opts.cbuffer = htb_burst_to_buffer(1, 1); /* 1s */
    }
    opts.ceil.linklayer = TC_LINKLAYER_ETHERNET;

    u32 = nla_nest_start(msg, TCA_OPTIONS);
    if (!u32) {
        error = TError(EError::Unknown, "TCA_OPTIONS");
        goto free_msg;
    }

    ret = nla_put(msg, TCA_HTB_PARMS, sizeof(opts), &opts);
    if (ret < 0)
        goto free_msg;

    ret = nla_put_u64(msg, TCA_HTB_RATE64, Rate ?: defRate);
    if (ret < 0)
        goto free_msg;

    ret = nla_put_u64(msg, TCA_HTB_CEIL64, Ceil ?: NET_MAX_RATE);
    if (ret < 0)
        goto free_msg;

    nla_nest_end(msg, u32);

    ret = nl_send_sync(nl.GetSock(), msg);
    if (ret)
        error = nl.Error(ret, Kind);

    return error;

free_msg:
    if (ret)
        error = nl.Error(ret, Kind);
    nlmsg_free(msg);

    return error;
}

TError TNlClass::Create(const TNl &nl, bool safe) {
    struct rtnl_class *cls;
    TError error;
    int ret;

    if (Kind == "htb")
        return CreateHTB(nl, safe);

    cls = rtnl_class_alloc();
    if (!cls)
        return TError("Cannot allocate rtnl_class object");

    rtnl_tc_set_ifindex(TC_CAST(cls), Index);
    rtnl_tc_set_parent(TC_CAST(cls), Parent);
    rtnl_tc_set_handle(TC_CAST(cls), Handle);

    ret = rtnl_tc_set_kind(TC_CAST(cls), Kind.c_str());
    if (ret < 0) {
        error = nl.Error(ret, "Cannot set class kind");
        goto free_class;
    }

    if (Kind == "hfsc") {
        uint64_t maxRate = UINT32_MAX;
        struct tc_service_curve rsc, fsc, usc;

        if (Rate) {
            rsc.m1 = std::min(Rate * 2, maxRate);
            rsc.m2 = std::min(Rate, maxRate);
            rsc.d = rsc.m1 ? std::ceil(Quantum * 1000000. / rsc.m1) : 0;

            ret = rtnl_class_hfsc_set_rsc(cls, &rsc);
            if (error) {
                error = nl.Error(ret, "Cannot set class rsc");
                goto free_class;
            }
        }

        fsc.m1 = std::min(std::max(Rate, defRate) * 2, maxRate);
        fsc.m2 = std::min(std::max(Rate, defRate), maxRate);
        fsc.d = fsc.m1 ? std::ceil(RateBurst * 1000000. / fsc.m1) : 0;

        ret = rtnl_class_hfsc_set_fsc(cls, &fsc);
        if (error) {
            error = nl.Error(ret, "Cannot set class fsc");
            goto free_class;
        }

        if (Ceil) {
            usc.m1 = std::min(Ceil * 2, maxRate);
            usc.m2 = std::min(Ceil, maxRate);
            usc.d = usc.m1 ? std::ceil(CeilBurst * 1000000. / usc.m1) : 0;

            ret = rtnl_class_hfsc_set_usc(cls, &usc);
            if (error) {
                error = nl.Error(ret, "Cannot set class usc");
                goto free_class;
            }
        }
    }

    nl.Dump("add", cls);
    ret = rtnl_class_add(nl.GetSock(), cls, NLM_F_CREATE | NLM_F_REPLACE);
    if (ret < 0) {
        (void)Delete(nl);
        nl.Dump("add", cls);
        ret = rtnl_class_add(nl.GetSock(), cls, NLM_F_CREATE | NLM_F_REPLACE);
        if (ret < 0)
            error = nl.Error(ret, "Cannot add traffic class");
    }

free_class:
    rtnl_class_put(cls);
    return error;
}

TError TNlClass::Delete(const TNl &nl) {
    struct rtnl_class *cls;
    TError error;
    int ret;

    cls = rtnl_class_alloc();
    if (!cls)
        return TError("Cannot allocate rtnl_class object");

    rtnl_tc_set_ifindex(TC_CAST(cls), Index);
    rtnl_tc_set_handle(TC_CAST(cls), Handle);

    nl.Dump("del", cls);
    ret = rtnl_class_delete(nl.GetSock(), cls);

    /* If busy -> remove recursively */
    if (ret == -NLE_BUSY) {
        std::vector<uint32_t> handles({Handle});
        struct nl_cache *cache;

        ret = rtnl_class_alloc_cache(nl.GetSock(), Index, &cache);
        if (ret < 0) {
            error = nl.Error(ret, "Cannot allocate class cache");
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
            nl.Dump("del", cls);
            ret = rtnl_class_delete(nl.GetSock(), cls);
            if (ret < 0 && ret != -NLE_OBJ_NOTFOUND)
                break;
        }
    }

    if (ret < 0 && ret != -NLE_OBJ_NOTFOUND)
        error = nl.Error(ret, "Cannot remove traffic class");
out:
    rtnl_class_put(cls);
    return error;
}


TError TNlPoliceFilter::Create(const TNl &nl) {
    uint32_t table[256];
    uint32_t result = TC_ACT_OK;
    TError error = OK;
    struct nlattr *u32, *u32_police;
    struct tc_u32_sel sel = {};
    struct tc_police parm = {};
    struct nl_msg *msg;
    struct tcmsg tchdr;
    int ret;

    memset(table, 0, sizeof(*table));

    tchdr.tcm_family = AF_UNSPEC;
    tchdr.tcm_ifindex = Index;
    tchdr.tcm_handle = 0;
    tchdr.tcm_parent = Parent;

    /* FIXME: may be we should avoid drop ARP and ICMP(4/6) */

    tchdr.tcm_info = TC_H_MAKE(FilterPrio << 16, htons(ETH_P_ALL));

    msg = nlmsg_alloc_simple(RTM_NEWTFILTER, NLM_F_EXCL|NLM_F_CREATE);
    if (!msg)
        return TError("Unable to add u32 filter: no memory");

    ret = nlmsg_append(msg, &tchdr, sizeof(tchdr), NLMSG_ALIGNTO);
    if (ret < 0) {
        error = TError(EError::Unknown, std::string("Unable to add u32: ") +
                                        nl_geterror(ret));
        goto free_msg;
    }

    ret = nla_put(msg, TCA_KIND, strlen(FilterType) + 1, FilterType);
    if (ret < 0) {
        error = TError(EError::Unknown, std::string("Unable to add u32: ") +
                                        nl_geterror(ret));
        goto free_msg;
    }

    u32 = nla_nest_start(msg, TCA_OPTIONS);

    sel.flags = TC_U32_TERMINAL;
    ret = nla_put(msg, TCA_U32_SEL, sizeof(sel), &sel);
    if (ret < 0) {
        error = TError(EError::Unknown, std::string("Unable to add u32 sel: ") +
                                        nl_geterror(ret));
        goto free_msg;
    }


    u32_police = nla_nest_start(msg, TCA_U32_POLICE);

    if (!u32_police) {
        error = TError(EError::Unknown, std::string("cannot create nested attrs: ") +
                                        nl_geterror(ret));
        goto free_msg;

    }

    parm.action = TC_ACT_SHOT;

    parm.rate.mpu = 0;
    parm.rate.overhead = 0;
    parm.rate.cell_align = -1;
    parm.rate.cell_log = 1;
    parm.rate.linklayer = TC_LINKLAYER_ETHERNET;
    parm.rate.rate = Rate;

    if (PeakRate) {
        parm.peakrate.mpu = 0;
        parm.peakrate.overhead = 0;
        parm.peakrate.cell_align = -1;
        parm.peakrate.cell_log = 1;
        parm.peakrate.linklayer = TC_LINKLAYER_ETHERNET;
        parm.peakrate.rate = PeakRate;
    }

    parm.burst = (1000000000 >> 6) * (double)Burst / Rate; /* psched ticks */

    parm.mtu = Mtu;

    ret = nla_put(msg, TCA_POLICE_TBF, sizeof(parm), &parm);
    if (ret < 0) {
        error = TError(EError::Unknown,
                       std::string("Unable to add policer: nla_put(TCA_POLICE_AVRATE): ") +
                       nl_geterror(ret));
        goto free_msg;
    }

    ret = nla_put(msg, TCA_POLICE_RATE, sizeof(table), table);
    if (ret < 0) {
        error = TError(EError::Unknown,
                       std::string("Unable to add policer: nl_put(TCA_POLICE_RATE): ") +
                       nl_geterror(ret));
        goto free_msg;
    }

    if (PeakRate) {
        ret = nla_put(msg, TCA_POLICE_PEAKRATE, sizeof(table), table);
        if (ret < 0) {
            error = TError(EError::Unknown,
                           std::string("Unable to add policer: nl_put(TCA_POLICE_RATE): ") +
                           nl_geterror(ret));
            goto free_msg;
        }
    }

    ret = nla_put(msg, TCA_POLICE_RESULT, sizeof(result), &result);

    nla_nest_end(msg, u32_police);
    nla_nest_end(msg, u32);

    L_NL("police {}: add u32 parent 0x{:x}", Index, Parent);

    ret = nl_send_sync(nl.GetSock(), msg);
    if (ret)
        error = TError(EError::Unknown,
                       std::string("Unable to add filter: nl_send_sync(): ") +
                       nl_geterror(ret));

    return error;

free_msg:
    nlmsg_free(msg);

    return error;
}

TError TNlPoliceFilter::Delete(const TNl &nl) {
    TError error = OK;
    struct tcmsg tchdr;
    struct nl_msg *msg;
    int ret;

    tchdr.tcm_family = AF_UNSPEC;
    tchdr.tcm_ifindex = Index;
    tchdr.tcm_handle = 0;
    tchdr.tcm_parent = Parent;
    tchdr.tcm_info = TC_H_MAKE(FilterPrio << 16, htons(ETH_P_IPV6));

    msg = nlmsg_alloc_simple(RTM_DELTFILTER, 0);
    if (!msg)
        return TError("Unable to del policer: no memory");

    ret = nlmsg_append(msg, &tchdr, sizeof(tchdr), NLMSG_ALIGNTO);
    if (ret < 0) {
        error = TError(EError::Unknown, std::string("Unable to del policer: ") +
                                        nl_geterror(ret));
        goto free_msg;
    }

    ret = nla_put(msg, TCA_OPTIONS, 0, NULL);
    if (ret < 0) {
        error = TError(EError::Unknown, std::string("Unable to del policer: ") +
                                        nl_geterror(ret));
        goto free_msg;
    }

    ret = nl_send_sync(nl.GetSock(), msg);
    if (ret)
        error = TError(EError::Unknown, std::string("Unable to del policer: ") +
                                        nl_geterror(ret));
    return error;

free_msg:
    nlmsg_free(msg);
    return error;
}

TError TNlCgFilter::Create(const TNl &nl) {
    TError error = OK;
    struct nl_msg *msg;
    int ret;
    struct tcmsg tchdr;

    tchdr.tcm_family = AF_UNSPEC;
    tchdr.tcm_ifindex = Index;
    tchdr.tcm_handle = Handle;
    tchdr.tcm_parent = Parent;
    tchdr.tcm_info = TC_H_MAKE(FilterPrio << 16, htons(ETH_P_ALL));

    msg = nlmsg_alloc_simple(RTM_NEWTFILTER, NLM_F_EXCL|NLM_F_CREATE);
    if (!msg)
        return TError("Unable to add filter: no memory");

    ret = nlmsg_append(msg, &tchdr, sizeof(tchdr), NLMSG_ALIGNTO);
    if (ret < 0) {
        error = TError(EError::Unknown, std::string("Unable to add filter: ") + nl_geterror(ret));
        goto free_msg;
    }

    ret = nla_put(msg, TCA_KIND, strlen(FilterType) + 1, FilterType);
    if (ret < 0) {
        error = TError(EError::Unknown, std::string("Unable to add filter: ") + nl_geterror(ret));
        goto free_msg;
    }

    ret = nla_put(msg, TCA_OPTIONS, 0, NULL);
    if (ret < 0) {
        error = TError(EError::Unknown, std::string("Unable to add filter: ") + nl_geterror(ret));
        goto free_msg;
    }

    L_NL("cg {}: add tfilter id 0x{:x} parent 0x{:x}", Index, Handle, Parent);

    ret = nl_send_sync(nl.GetSock(), msg);
    if (ret)
        error = TError(EError::Unknown, std::string("Unable to add filter: ") + nl_geterror(ret));

    if (!error && !Exists(nl))
        error = TError("BUG: created filter doesn't exist");

    return error;

free_msg:
    nlmsg_free(msg);

    return error;
}

bool TNlCgFilter::Exists(const TNl &nl) {
    int ret;
    struct nl_cache *clsCache;

    ret = rtnl_cls_alloc_cache(nl.GetSock(), Index, Parent, &clsCache);
    if (ret < 0) {
        L_ERR("Can't allocate filter cache: {}", nl_geterror(ret));
        return false;
    }

//    nl.DumpCache(clsCache);

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

TError TNlCgFilter::Delete(const TNl &nl) {
    TError error = OK;
    struct rtnl_cls *cls;
    int ret;

    cls = rtnl_cls_alloc();
    if (!cls)
        return TError(EError::Unknown, std::string("Unable to allocate filter object"));

    rtnl_tc_set_ifindex(TC_CAST(cls), Index);

    ret = rtnl_tc_set_kind(TC_CAST(cls), FilterType);
    if (ret < 0) {
        error = TError(EError::Unknown, std::string("Unable to set filter type: ") + nl_geterror(ret));
        goto free_cls;
    }

    rtnl_cls_set_prio(cls, FilterPrio);
    rtnl_cls_set_protocol(cls, 0);
    rtnl_tc_set_parent(TC_CAST(cls), Parent);

    nl.Dump("remove", cls);

    ret = rtnl_cls_delete(nl.GetSock(), cls, 0);
    if (ret < 0)
        error = nl.Error(ret, "Cannot to remove filter");

free_cls:
    rtnl_cls_put(cls);

    return error;
}

TNlAddr::TNlAddr(struct nl_addr *addr) {
    if (addr)
        Addr = nl_addr_get(addr);
    else
        Addr = nullptr;
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
    Forget();
}

void TNlAddr::Forget() {
    nl_addr_put(Addr);
    Addr = nullptr;
}

bool TNlAddr::IsEmpty() const {
    return !Addr || nl_addr_iszero(Addr);
}

int TNlAddr::Family() const {
    return Addr ? nl_addr_get_family(Addr) : AF_UNSPEC;
}

bool TNlAddr::IsHost() const {
    return Addr && nl_addr_get_prefixlen(Addr) == nl_addr_get_len(Addr) * 8;
}

unsigned int TNlAddr::Length() const {
    return Addr ? nl_addr_get_len(Addr) : 0;
}

const void *TNlAddr::Binary() const {
    return Addr ? nl_addr_get_binary_addr(Addr) : nullptr;
}

unsigned int TNlAddr::Prefix() const {
    return Addr ? nl_addr_get_prefixlen(Addr) : 0;
}

TError TNlAddr::Parse(int family, const std::string &string) {
    Forget();

    int ret = nl_addr_parse(string.c_str(), family, &Addr);
    if (ret)
        return TNl::Error(ret, "Cannot parse address " + string);

    return OK;
}

std::string TNlAddr::Format() const {
    char buf[128];
    return std::string(nl_addr2str(Addr, buf, sizeof(buf)));
}

void TNlAddr::AddOffset(uint64_t offset) {
    uint8_t *byte = (uint8_t *)nl_addr_get_binary_addr(Addr);

    for (int i = nl_addr_get_len(Addr) - 1; offset && i >= 0; i--) {
        offset += byte[i];
        byte[i] = offset & 0xFF;
        offset >>= 8;
    }
}

uint64_t TNlAddr::GetOffset(const TNlAddr &base) const {
    uint8_t *byte = (uint8_t *)nl_addr_get_binary_addr(Addr);
    uint8_t *base_byte = (uint8_t *)nl_addr_get_binary_addr(base.Addr);
    int len = nl_addr_get_len(Addr);
    uint64_t offset = 0;

    for (int i = 0; i < len; i++) {
        offset <<= 8;
        offset += byte[i] - base_byte[i];
    }

    return offset;
}

void TNlAddr::GetRange(TNlAddr &base, uint64_t &count) const {
    int len = nl_addr_get_len(Addr);
    int bits = len * 8 - nl_addr_get_prefixlen(Addr);
    count = bits < 64 ? (1ull << bits) : 0;

    base = *this;
    uint8_t *byte = (uint8_t *)nl_addr_get_binary_addr(base.Addr);

    byte += len - 1 - bits / 8;
    if (bits % 8)
        *byte &= 0xff << (bits % 8);
    memset(byte + 1, 0, bits / 8);
}

TError TNlAddr::GetRange(std::vector<TNlAddr> &addrs, int max) const {
    TNlAddr base;
    uint64_t count;

    GetRange(base, count);
    if (count > max)
        return TError(EError::ResourceNotAvailable, "Too many ip in subnet {}, max {}", Format(), max);

    while (count--) {
        addrs.push_back(base);
        base.AddOffset(1);
    }

    return OK;
}

bool TNlAddr::IsMatch(const TNlAddr &addr) const {
    return Addr && addr.Addr &&
        nl_addr_get_prefixlen(Addr) <= nl_addr_get_prefixlen(addr.Addr) &&
        nl_addr_cmp_prefix(Addr, addr.Addr) == 0;
}

bool TNlAddr::IsEqual(const TNlAddr &addr) const {
    return Addr && addr.Addr && nl_addr_cmp(Addr, addr.Addr) == 0;
}
