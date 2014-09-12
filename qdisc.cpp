#include "qdisc.hpp"
#include "util/log.hpp"

// HTB shaping details:
// http://luxik.cdi.cz/~devik/qos/htb/manual/userg.htm

extern "C" {
#include <netlink/route/classifier.h>
#include <netlink/route/qdisc.h>
#include <netlink/route/qdisc/htb.h>
}

using namespace std;

uint32_t TcHandle(uint16_t maj, uint16_t min) {
    return TC_HANDLE(maj, min);
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

    ret = rtnl_link_alloc_cache(sock, AF_UNSPEC, &link_cache);
    if (ret < 0) {
        error = TError(EError::Unknown, string("Unable to allocate link cache: ") + nl_geterror(ret));
        goto close_socket;
    }
    LogCache(link_cache);

    nl_cache_mngt_provide(link_cache);

    link = rtnl_link_get_by_name(link_cache, device.c_str());
    if (!link) {
        error = TError(EError::Unknown, string("Invalid device ") + device);
        goto free_cache;
    }

    return TError::Success();

free_cache:
    nl_cache_free(link_cache);
close_socket:
    nl_close(sock);
free_socket:
    nl_socket_free(sock);

    return error;
}

void TNetlink::Close() {
    if (link_cache)
        nl_cache_free(link_cache);
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

    str << "netlink: ";
    nl_object_dump(OBJ_CAST(obj), &dp);
    str << endl;
}

void TNetlink::LogCache(struct nl_cache *cache) {
    static function<void(struct nl_dump_params *, char *)> handler;

    struct nl_dump_params dp = {};
    dp.dp_cb = [](struct nl_dump_params *params, char *buf) { handler(params, buf); };

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
        return TError(EError::Unknown, string("tc class rate is not specified"));

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

TError TNetlink::GetStat(uint32_t handle, int stat, uint64_t &val) {
    int ret;
    struct nl_cache *class_cache;

    ret = rtnl_class_alloc_cache(sock, rtnl_link_get_ifindex(link), &class_cache);
    if (ret < 0)
        return TError(EError::Unknown, string("Unable to allocate class cache: ") + nl_geterror(ret));

    if (0)
        LogCache(class_cache);

    struct rtnl_class *tclass = rtnl_class_get(class_cache, rtnl_link_get_ifindex(link), handle);
    if (!tclass) {
        nl_cache_free(class_cache);
        return TError(EError::Unknown, "Can't get class statistics");
    }

    val = rtnl_tc_get_stat(TC_CAST(tclass), (rtnl_tc_stat)stat);
    nl_cache_free(class_cache);

    return TError::Success();
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
    if (ret < 0) {
        error = TError(EError::Unknown, string("Unable to add qdisc: ") + nl_geterror(ret));
        goto free_qdisc;
    }

free_qdisc:
    rtnl_qdisc_put(qdisc);

    return error;
}

const std::string &TTclass::GetDevice() {
    if (ParentTclass)
        return ParentTclass->GetDevice();
    else
        return ParentQdisc->GetDevice();
}

TError TTclass::GetStat(ETclassStat stat, uint64_t &val) {
    TNetlink nl;

    TError error = nl.Open(GetDevice());
    if (error)
        return error;

    switch (stat) {
    case ETclassStat::Packets:
        return nl.GetStat(Handle, RTNL_TC_PACKETS, val);
    case ETclassStat::Bytes:
        return nl.GetStat(Handle, RTNL_TC_BYTES, val);
    case ETclassStat::Drops:
        return nl.GetStat(Handle, RTNL_TC_DROPS, val);
    case ETclassStat::Overlimits:
        return nl.GetStat(Handle, RTNL_TC_OVERLIMITS, val);
    default:
        return TError(EError::Unknown, "Invalid class statistics");
    };
}

uint32_t TTclass::GetParent() {
    if (ParentQdisc)
        return ParentQdisc->GetHandle();
    else
        return ParentTclass->Handle;
}

TError TTclass::Create(uint32_t prio, uint32_t rate, uint32_t ceil) {
    TNetlink nl;

    TError error = nl.Open(GetDevice());
    if (error)
        return error;

    error = nl.AddClass(GetParent(), Handle, prio, rate, ceil);
    if (error)
        return error;

    return TError::Success();
}

TError TTclass::Remove() {
    TNetlink nl;

    TError error = nl.Open(GetDevice());
    if (error)
        return error;

    error = nl.RemoveClass(GetParent(), Handle);
    if (error)
        return error;

    return TError::Success();
}

TError TQdisc::Create() {
    TNetlink nl;

    TError error = nl.Open(Device);
    if (error)
        return error;

    error = nl.AddHTB(TC_H_ROOT, Handle, DefClass);
    if (error)
        return error;

    return TError::Success();
}

TError TQdisc::Remove() {
    TNetlink nl;

    TError error = nl.Open(Device);
    if (error)
        return error;

    error = nl.RemoveHTB(TC_H_ROOT);
    if (error)
        return error;

    return TError::Success();
}

const std::string &TQdisc::GetDevice() {
    return Device;
}
