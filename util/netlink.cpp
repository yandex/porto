#include "porto.hpp"
#include "netlink.hpp"
#include "util/log.hpp"

// HTB shaping details:
// http://luxik.cdi.cz/~devik/qos/htb/manual/userg.htm

extern "C" {
#include <linux/if_ether.h>
#include <netlink/route/classifier.h>
#include <netlink/route/cls/cgroup.h>
#include <netlink/route/qdisc.h>
#include <netlink/route/qdisc/htb.h>

#include <netlink/route/rtnl.h>
#include <netlink/route/link.h>

}

using namespace std;

uint32_t TcHandle(uint16_t maj, uint16_t min) {
    return TC_HANDLE(maj, min);
}

uint32_t TcRootHandle() {
    return TC_H_ROOT;
}

uint16_t TcMajor(uint32_t handle) {
    return (uint16_t)(handle >> 16);
}

TError TNetlink::Open(const std::string &device) {
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

    if (DEBUG_NETLINK)
        LogCache(linkCache);

    nl_cache_mngt_provide(linkCache);

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
        nl_cache_free(linkCache);
    if (sock) {
        nl_close(sock);
        nl_socket_free(sock);
    }
}

void TNetlink::LogObj(const std::string &prefix, void *obj) {
    static function<void(struct nl_dump_params *, char *)> handler;

    struct nl_dump_params dp = {};
    dp.dp_cb = [](struct nl_dump_params *params, char *buf) { handler(params, buf); };

    auto &str = TLogger::Log();
    handler = [&](struct nl_dump_params *params, char *buf) { str << buf; };

    str << "netlink: " << prefix << " ";
    nl_object_dump(OBJ_CAST(obj), &dp);
    str << endl;
}

void TNetlink::LogCache(struct nl_cache *cache) {
    static function<void(struct nl_dump_params *, char *)> handler;

    struct nl_dump_params dp = {};
    dp.dp_cb = [](struct nl_dump_params *params, char *buf) { handler(params, buf); };
    dp.dp_type = NL_DUMP_DETAILS;

    auto &str = TLogger::Log();
    handler = [&](struct nl_dump_params *params, char *buf) { str << buf; };

    str << "netlink cache: ";
    nl_cache_dump(cache, &dp);
    str << endl;
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

    rtnl_class_delete(sock, tclass);

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

    if (DEBUG_NETLINK)
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

bool TNetlink::ClassExists(uint32_t handle) {
    int ret;
    struct nl_cache *classCache;
    bool exists;

    ret = rtnl_class_alloc_cache(sock, rtnl_link_get_ifindex(link), &classCache);
    if (ret < 0)
        return TError(EError::Unknown, string("Unable to allocate class cache: ") + nl_geterror(ret));

    if (DEBUG_NETLINK)
        LogCache(classCache);

    struct rtnl_class *tclass = rtnl_class_get(classCache, rtnl_link_get_ifindex(link), handle);
    exists = tclass != nullptr;
    rtnl_class_put(tclass);
    nl_cache_free(classCache);

    return exists;
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

    // delete current qdisc
    rtnl_qdisc_delete(sock, qdisc);

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

TError TNetlink::AddCgroupFilter(uint32_t parent, uint32_t handle) {
    TError error = TError::Success();
    const int prio = 10;
    const int proto = ETH_P_IP;
    const char *ftype = "cgroup";
    struct nl_msg *msg;
    int ret;
	struct tcmsg tchdr;

    tchdr.tcm_family = AF_UNSPEC;
    tchdr.tcm_ifindex = rtnl_link_get_ifindex(link);
    tchdr.tcm_handle = handle;
    tchdr.tcm_parent = parent;
	tchdr.tcm_info = TC_H_MAKE(prio << 16, htons(proto));

    (void)RemoveCgroupFilter(parent, handle);

	msg = nlmsg_alloc_simple(RTM_NEWTFILTER, NLM_F_CREATE);
	if (!msg)
        return TError(EError::Unknown, "Unable to add filter: no memory");

    ret = nlmsg_append(msg, &tchdr, sizeof(tchdr), NLMSG_ALIGNTO);
    if (ret < 0) {
        error = TError(EError::Unknown, string("Unable to add filter: ") + nl_geterror(ret));
		goto free_msg;
    }

    ret = nla_put(msg, TCA_KIND, strlen(ftype) + 1, ftype);
    if (ret < 0) {
        error = TError(EError::Unknown, string("Unable to add filter: ") + nl_geterror(ret));
		goto free_msg;
    }

    ret = nla_put(msg, TCA_OPTIONS, 0, NULL);
    if (ret < 0) {
        error = TError(EError::Unknown, string("Unable to add filter: ") + nl_geterror(ret));
		goto free_msg;
    }

    ret = nl_send_sync(sock, msg);
    if (ret) {
        error = TError(EError::Unknown, string("Unable to add filter: ") + nl_geterror(ret));
        goto free_msg;
    }

free_msg:
	nlmsg_free(msg);

    return error;
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

    ret = rtnl_tc_set_kind(TC_CAST(cls), "cgroup");
    if (ret < 0) {
        error = TError(EError::Unknown, string("Unable to set filter type: ") + nl_geterror(ret));
        goto free_cls;
    }

    rtnl_cls_set_prio(cls, 10);
    rtnl_cls_set_protocol(cls, ETH_P_IP);
    rtnl_tc_set_parent(TC_CAST(cls), parent);

    LogObj("remove", cls);

    (void)rtnl_cls_delete(sock, cls, 0);

free_cls:
    rtnl_cls_put(cls);

    return error;

}
