#include "porto.hpp"
#include "netlink.hpp"
#include "util/log.hpp"

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

TError TNetlink::FindDev(std::string &device) {
    static string dev;

    if (dev.length()) {
        device = dev;
        return TError::Success();
    }

    struct FindDevIter { string name; string running; } data;
    nl_cache_foreach(linkCache, [](struct nl_object *obj, void *data) {
                     FindDevIter *p = (FindDevIter *)data;
                     struct rtnl_link *l = (struct rtnl_link *)obj;
                     const vector<string> prefixes = { "eth", "em", "wlp2s" };

                     if (p->name.length())
                        return;

                     for (auto &pref : prefixes)
                        if (strncmp(rtnl_link_get_name(l), pref.c_str(),
                                    pref.length()) == 0 &&
                            rtnl_link_get_flags(l) & IFF_RUNNING)
                            p->name = rtnl_link_get_name(l);

                     if (p->running.length())
                        return;

                     if ((rtnl_link_get_flags(l) & IFF_RUNNING) &&
                        !(rtnl_link_get_flags(l) & IFF_LOOPBACK))
                        p->running = rtnl_link_get_name(l);
                     }, &data);

    if (data.name.length()) {
        dev = device = data.name;
    } else {
        if (data.running.length()) {
            dev = device = data.running;

            TLogger::Log() << "Can't find predefined link, using " << dev << std::endl;
        } else {
            return TError(EError::Unknown, "Can't find appropriate link");
        }
    }

    return TError::Success();
}

TError TNetlink::Open(std::string device) {
    int ret;
    TError error;

    sock = nl_socket_alloc();
    if (!sock)
        return TError(EError::Unknown, string("Unable to allocate netlink socket"));

    ret = nl_connect(sock, NETLINK_ROUTE);
    if (ret < 0) {
        error = TError(EError::Unknown, string("Unable to connect netlink socket: ") + nl_geterror(ret));
        goto free_socket;
    }

    ret = rtnl_link_alloc_cache(sock, AF_UNSPEC, &linkCache);
    if (ret < 0) {
        error = TError(EError::Unknown, string("Unable to allocate link cache: ") + nl_geterror(ret));
        goto close_socket;
    }

    LogCache(linkCache);

    nl_cache_mngt_provide(linkCache);

    if (!device.length()) {
        error = FindDev(device);
        if (error)
            return error;
    }

    link = rtnl_link_get_by_name(linkCache, device.c_str());
    if (!link) {
        error = TError(EError::Unknown, string("Invalid device ") + device);
        goto free_cache;
    }

    return TError::Success();

free_cache:
    nl_cache_free(linkCache);
close_socket:
    nl_close(sock);
free_socket:
    nl_socket_free(sock);

    return error;
}

void TNetlink::Close() {
    if (link)
        rtnl_link_put(link);
    if (linkCache)
        nl_cache_mngt_unprovide(linkCache);
        nl_cache_free(linkCache);
    if (sock) {
        nl_close(sock);
        nl_socket_free(sock);
    }
}

void TNetlink::LogObj(const std::string &prefix, void *obj) {
    static std::function<void(struct nl_dump_params *, char *)> handler;

    struct nl_dump_params dp = {};
    dp.dp_cb = [](struct nl_dump_params *params, char *buf) { handler(params, buf); };

    auto &str = TLogger::Log();
    handler = [&](struct nl_dump_params *params, char *buf) { str << buf; };

    str << "netlink " << rtnl_link_get_name(link) << ": " << prefix << " ";
    nl_object_dump(OBJ_CAST(obj), &dp);
    str << std::endl;
}

void TNetlink::LogCache(struct nl_cache *cache) {
    if (!debug)
        return;

    static std::function<void(struct nl_dump_params *, char *)> handler;

    struct nl_dump_params dp = {};
    dp.dp_cb = [](struct nl_dump_params *params, char *buf) { handler(params, buf); };
    dp.dp_type = NL_DUMP_DETAILS;

    auto &str = TLogger::Log();
    handler = [&](struct nl_dump_params *params, char *buf) { str << buf; };

    str << "netlink " << rtnl_link_get_name(link) << " cache: ";
    nl_cache_dump(cache, &dp);
    str << std::endl;
}

TError TNetlink::AddClass(uint32_t parent, uint32_t handle, uint32_t prio, uint32_t rate, uint32_t ceil) {
    TError error = TError::Success();
    int ret;
    struct rtnl_class *tclass;

    if (!rate)
        return TError(EError::Unknown, string("tc classifier rate is not specified"));

    tclass = rtnl_class_alloc();
    if (!tclass)
        return TError(EError::Unknown, string("Unable to allocate tclass object"));

    rtnl_tc_set_link(TC_CAST(tclass), link);
    rtnl_tc_set_parent(TC_CAST(tclass), parent);
    rtnl_tc_set_handle(TC_CAST(tclass), handle);

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

    LogObj("add", tclass);

    ret = rtnl_class_add(sock, tclass, NLM_F_CREATE);
    if (ret < 0)
        error = TError(EError::Unknown, string("Unable to add tclass: ") + nl_geterror(ret));

free_class:
    rtnl_class_put(tclass);

    return error;
}

TError TNetlink::GetStat(uint32_t handle, ETclassStat stat, uint64_t &val) {
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
    default:
        return TError(EError::Unknown, "Unsupported netlink statistics");
    }

    ret = rtnl_class_alloc_cache(sock, rtnl_link_get_ifindex(link), &classCache);
    if (ret < 0)
        return TError(EError::Unknown, string("Unable to allocate class cache: ") + nl_geterror(ret));

    LogCache(classCache);

    struct rtnl_class *tclass = rtnl_class_get(classCache, rtnl_link_get_ifindex(link), handle);
    if (!tclass) {
        nl_cache_free(classCache);
        return TError(EError::Unknown, "Can't get class statistics");
    }

    val = rtnl_tc_get_stat(TC_CAST(tclass), (rtnl_tc_stat)rtnlStat);
    rtnl_class_put(tclass);
    nl_cache_free(classCache);

    return TError::Success();
}

TError TNetlink::GetClassProperties(uint32_t handle, uint32_t &prio, uint32_t &rate, uint32_t &ceil) {
    int ret;
    struct nl_cache *classCache;

    ret = rtnl_class_alloc_cache(sock, rtnl_link_get_ifindex(link), &classCache);
    if (ret < 0)
        return TError(EError::Unknown, string("Unable to allocate class cache: ") + nl_geterror(ret));

    LogCache(classCache);

    struct rtnl_class *tclass = rtnl_class_get(classCache, rtnl_link_get_ifindex(link), handle);

    prio = rtnl_htb_get_prio(tclass);
    rate = rtnl_htb_get_rate(tclass);
    ceil = rtnl_htb_get_ceil(tclass);

    rtnl_class_put(tclass);
    nl_cache_free(classCache);

    return TError::Success();
}

bool TNetlink::ClassExists(uint32_t handle) {
    int ret;
    struct nl_cache *classCache;

    ret = rtnl_class_alloc_cache(sock, rtnl_link_get_ifindex(link), &classCache);
    if (ret < 0)
        return false;

    LogCache(classCache);

    struct rtnl_class *tclass = rtnl_class_get(classCache, rtnl_link_get_ifindex(link), handle);
    rtnl_class_put(tclass);
    nl_cache_free(classCache);

    return tclass != nullptr;
}

TError TNetlink::RemoveClass(uint32_t parent, uint32_t handle) {
    TError error = TError::Success();
    int ret;
    struct rtnl_class *tclass;

    tclass = rtnl_class_alloc();
    if (!tclass)
        return TError(EError::Unknown, string("Unable to allocate tclass object"));

    rtnl_tc_set_link(TC_CAST(tclass), link);
    rtnl_tc_set_parent(TC_CAST(tclass), parent);
    rtnl_tc_set_handle(TC_CAST(tclass), handle);

    ret = rtnl_class_delete(sock, tclass);
    if (ret < 0)
        error = TError(EError::Unknown, string("Unable to remove tclass: ") + nl_geterror(ret));

    LogObj("remove", tclass);

    rtnl_class_put(tclass);

    return error;
}

TError TNetlink::RemoveHTB(uint32_t parent) {
    struct rtnl_qdisc *qdisc;

    qdisc = rtnl_qdisc_alloc();
    if (!qdisc)
        return TError(EError::Unknown, string("Unable to allocate qdisc object"));

    rtnl_tc_set_link(TC_CAST(qdisc), link);
    rtnl_tc_set_parent(TC_CAST(qdisc), parent);

    LogObj("remove", qdisc);

    rtnl_qdisc_delete(sock, qdisc);
    rtnl_qdisc_put(qdisc);

    return TError::Success();
}

TError TNetlink::AddHTB(uint32_t parent, uint32_t handle, uint32_t defaultClass) {
    TError error = TError::Success();
    int ret;
    struct rtnl_qdisc *qdisc;

    qdisc = rtnl_qdisc_alloc();
    if (!qdisc)
        return TError(EError::Unknown, string("Unable to allocate qdisc object"));

    rtnl_tc_set_link(TC_CAST(qdisc), link);
    rtnl_tc_set_parent(TC_CAST(qdisc), parent);

    rtnl_tc_set_handle(TC_CAST(qdisc), handle);

    ret = rtnl_tc_set_kind(TC_CAST(qdisc), "htb");
    if (ret < 0) {
        error = TError(EError::Unknown, string("Unable to set qdisc type: ") + nl_geterror(ret));
        goto free_qdisc;
    }

    rtnl_htb_set_defcls(qdisc, TcHandle(1, defaultClass));
    rtnl_htb_set_rate2quantum(qdisc, 10);

    LogObj("add", qdisc);

    ret = rtnl_qdisc_add(sock, qdisc, NLM_F_CREATE);
    if (ret < 0)
        error = TError(EError::Unknown, string("Unable to add qdisc: ") + nl_geterror(ret));

free_qdisc:
    rtnl_qdisc_put(qdisc);

    return error;
}

bool TNetlink::QdiscExists(uint32_t handle) {
    int ret;
    struct nl_cache *qdiscCache;
    bool exists;

    ret = rtnl_qdisc_alloc_cache(sock, &qdiscCache);
    if (ret < 0)
        return false;

    LogCache(qdiscCache);

    struct rtnl_qdisc *qdisc = rtnl_qdisc_get(qdiscCache, rtnl_link_get_ifindex(link), handle);
    exists = qdisc != nullptr;
    rtnl_qdisc_put(qdisc);
    nl_cache_free(qdiscCache);

    return exists;
}

TError TNetlink::AddCgroupFilter(uint32_t parent, uint32_t handle) {
    TError error = TError::Success();
    struct nl_msg *msg;
    int ret;
	struct tcmsg tchdr;

    tchdr.tcm_family = AF_UNSPEC;
    tchdr.tcm_ifindex = rtnl_link_get_ifindex(link);
    tchdr.tcm_handle = handle;
    tchdr.tcm_parent = parent;
	tchdr.tcm_info = TC_H_MAKE(FilterPrio << 16, htons(ETH_P_IP));

	msg = nlmsg_alloc_simple(RTM_NEWTFILTER, NLM_F_CREATE);
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

    TLogger::Log() << "netlink " << rtnl_link_get_name(link) << ": create tfilter id 0x" << std::hex << handle << " parent 0x" << parent << std::dec  << std::endl;

    ret = nl_send_sync(sock, msg);
    if (ret) {
        error = TError(EError::Unknown, string("Unable to add filter: ") + nl_geterror(ret));
        goto free_msg;
    }

    if (!CgroupFilterExists(parent, handle))
        error = TError(EError::Unknown, "BUG: created filter doesn't exist");

    return error;

free_msg:
	nlmsg_free(msg);

    return error;
}

bool TNetlink::CgroupFilterExists(uint32_t parent, uint32_t handle) {
    int ret;
    struct nl_cache *clsCache;

    ret = rtnl_cls_alloc_cache(sock, rtnl_link_get_ifindex(link), parent, &clsCache);
    if (ret < 0) {
        TLogger::Log() << "Can't allocate filter cache: " << nl_geterror(ret) << std::endl;
        return false;
    }

    LogCache(clsCache);

    struct CgFilterIter {
        uint32_t parent;
        uint32_t handle;
        bool exists;
    } data = { parent, handle, false };
    nl_cache_foreach(clsCache, [](struct nl_object *obj, void *data) {
                     CgFilterIter *p = (CgFilterIter *)data;
                     if (rtnl_tc_get_handle(TC_CAST(obj)) == p->handle &&
                         rtnl_tc_get_parent(TC_CAST(obj)) == p->parent)
                         p->exists = true;
                     }, &data);

    nl_cache_free(clsCache);
    return data.exists;
}

TError TNetlink::RemoveCgroupFilter(uint32_t parent, uint32_t handle) {
    TError error = TError::Success();
    struct rtnl_cls *cls;
    int ret;

    cls = rtnl_cls_alloc();
    if (!cls)
        return TError(EError::Unknown, string("Unable to allocate filter object"));

    rtnl_tc_set_link(TC_CAST(cls), link);
    rtnl_tc_set_handle(TC_CAST(cls), handle);

    ret = rtnl_tc_set_kind(TC_CAST(cls), FilterType);
    if (ret < 0) {
        error = TError(EError::Unknown, string("Unable to set filter type: ") + nl_geterror(ret));
        goto free_cls;
    }

    rtnl_cls_set_prio(cls, FilterPrio);
    rtnl_cls_set_protocol(cls, ETH_P_IP);
    rtnl_tc_set_parent(TC_CAST(cls), parent);

    LogObj("remove", cls);

    ret = rtnl_cls_delete(sock, cls, 0);
    if (ret < 0)
        error = TError(EError::Unknown, string("Unable to remove filter: ") + nl_geterror(ret));

free_cls:
    rtnl_cls_put(cls);

    return error;

}

int TNetlink::GetLinkIndex(const std::string &device) {
    struct Iter { string name; int idx; } data = { device, -1 };
    nl_cache_foreach(linkCache, [](struct nl_object *obj, void *data) {
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

TError TNetlink::AddMacVlan(const std::string &name, const std::string &master,
                            const std::string &type, const std::string &hw) {
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
    int masterIdx = GetLinkIndex(master);

    rtnl_link_set_link(hostLink, masterIdx);
    rtnl_link_set_name(hostLink, name.c_str());

    if (ea) {
        struct nl_addr *addr = nl_addr_build(AF_LLC, ea, ETH_ALEN);
        rtnl_link_set_addr(hostLink, addr);
        nl_addr_put(addr);
    }

    rtnl_link_macvlan_set_mode(hostLink, mode);

    LogObj("add", hostLink);

    ret = rtnl_link_add(sock, hostLink, NLM_F_CREATE);
    if (ret < 0)
        error = TError(EError::Unknown, string("Unable to add macvlan: ") + nl_geterror(ret));

    rtnl_link_put(hostLink);

    /* our link cache is invalid now, refill */
    ret = nl_cache_refill(sock, linkCache);
    if (ret < 0) {
        error = TError(EError::Unknown, string("Unable to add macvlan: ") + nl_geterror(ret));
        TLogger::LogError(error, "Can't refill cache");
    } else {
        LogCache(linkCache);
    }

    return error;
}

TError TNetlink::RemoveLink(const std::string &name) {
    TError error = TError::Success();
    struct rtnl_link *hostLink = rtnl_link_alloc();
    rtnl_link_set_name(hostLink, name.c_str());
    LogObj("remove", hostLink);
    int ret = rtnl_link_delete(sock, hostLink);
    if (ret < 0)
        error = TError(EError::Unknown, string("Unable to remove macvlan: ") + nl_geterror(ret));
    rtnl_link_put(hostLink);

    return error;
}

TError TNetlink::LinkUp(const std::string &name) {
    int ret;
    struct rtnl_link *oldLink = rtnl_link_alloc();
    if (!oldLink)
        return TError(EError::Unknown, "Unable to allocate link");
    struct rtnl_link *newLink = rtnl_link_alloc();
    if (!newLink) {
        rtnl_link_put(oldLink);
        return TError(EError::Unknown, "Unable to allocate link");
    }

    rtnl_link_set_name(oldLink, name.c_str());
    rtnl_link_set_flags(newLink, IFF_UP);

    LogObj("up old", oldLink);
    LogObj("up new", oldLink);

    ret = rtnl_link_change(sock, oldLink, newLink, 0);
    if (ret < 0) {
        rtnl_link_put(oldLink);
        rtnl_link_put(newLink);
        return TError(EError::Unknown, string("Unable to change link namespace: ") + nl_geterror(ret));
    }

    rtnl_link_put(newLink);
    rtnl_link_put(oldLink);

    return TError::Success();
}

TError TNetlink::ChangeLinkNs(const std::string &name,
                              const std::string &newName, int pid) {
    int ret;
    struct rtnl_link *newLink;
    struct rtnl_link *oldLink = rtnl_link_get_by_name(linkCache, name.c_str());
    if (!oldLink)
        return TError(EError::Unknown, "Invalid link " + name);

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

    ret = rtnl_link_change(sock, oldLink, newLink, 0);
    if (ret < 0) {
        rtnl_link_put(oldLink);
        rtnl_link_put(newLink);
        return TError(EError::Unknown, string("Unable to change link namespace: ") + nl_geterror(ret));
    }

    rtnl_link_put(newLink);
    rtnl_link_put(oldLink);

    return TError::Success();
}

TError TNetlink::AddMacVlan(const std::string &name, const std::string &master,
                            const std::string &type, const std::string &hw,
                            int nsPid) {
    string tmpName = "portotmp0";

    /* remove any leftover temporary device */

    (void)RemoveLink(tmpName);

    /* create device with fixed name in current namespace */

    TError error = AddMacVlan(tmpName, master, type, hw);
    if (error)
        return error;

    /* move device into target namespace and change name */

    error = ChangeLinkNs(tmpName, name, nsPid);
    if (error) {
        (void)RemoveLink(tmpName);
        return error;
    }

    return TError::Success();
}

bool TNetlink::ValidMacVlanType(const std::string &type) {
    return rtnl_link_macvlan_str2mode(type.c_str()) >= 0;
}

bool TNetlink::ValidMacAddr(const std::string &hw) {
    return ether_aton(hw.c_str()) != nullptr;
}

void TNetlink::EnableDebug(bool enable) {
    debug = enable;
}

TError TNetlink::Exec(std::string device, std::function<TError(TNetlink &nl)> f) {
    TNetlink nl;

    TError error = nl.Open(device);
    if (error)
        return error;

    return f(nl);
}
