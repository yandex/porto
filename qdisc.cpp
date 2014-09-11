#include "qdisc.hpp"
#include "util/log.hpp"

// HTB shaping details:
// http://luxik.cdi.cz/~devik/qos/htb/manual/userg.htm

extern "C" {
#include <netlink/route/qdisc.h>
#include <netlink/route/qdisc/htb.h>
}

using namespace std;

uint32_t TcHandle(uint16_t maj, uint16_t min) {
    return TC_HANDLE(maj, min);
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

    ret = rtnl_link_alloc_cache(sock, AF_UNSPEC, &cache);
    if (ret < 0) {
        error = TError(EError::Unknown, string("Unable to allocate link cache: ") + nl_geterror(ret));
        goto close_socket;
    }

    nl_cache_mngt_provide(cache);

    link = rtnl_link_get_by_name(cache, device.c_str());
    if (!link) {
        error = TError(EError::Unknown, string("Invalid device ") + device);
        goto free_cache;
    }

    return TError::Success();

free_cache:
    nl_cache_free(cache);
close_socket:
    nl_close(sock);
free_socket:
    nl_socket_free(sock);

    return error;
}

void TNetlink::Close() {
    if (cache)
        nl_cache_free(cache);
    if (sock) {
        nl_close(sock);
        nl_socket_free(sock);
    }
}

void TNetlink::Log(const std::string &prefix, void *obj) {
    char buf[1024];
    nl_object_dump_buf(OBJ_CAST(obj), buf, sizeof(buf));
    TLogger::Log() << "netlink: " << prefix << " " << buf << endl;
}

TError TNetlink::AddClass(uint32_t parent, uint32_t handle, uint32_t prio, uint32_t rate, uint32_t ceil) {
    int ret;
    struct rtnl_class *tclass;
    TError error = TError::Success();

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

    Log("add", tclass);

    ret = rtnl_class_add(sock, tclass, NLM_F_CREATE);
    if (ret < 0)
        error = TError(EError::Unknown, string("Unable to add tclass: ") + nl_geterror(ret));

free_class:
    rtnl_class_put(tclass);

    return error;
}

TError TNetlink::RemoveClass(uint32_t parent, uint32_t handle) {
    int ret;
    struct rtnl_class *tclass;
    TError error = TError::Success();

    tclass = rtnl_class_alloc();
    if (!tclass)
        return TError(EError::Unknown, string("Unable to allocate tclass object"));

    rtnl_tc_set_link(TC_CAST(tclass), link);
    rtnl_tc_set_parent(TC_CAST(tclass), parent);
    rtnl_tc_set_handle(TC_CAST(tclass), handle);

    ret = rtnl_class_delete(sock, tclass);
    if (ret < 0)
        error = TError(EError::Unknown, string("Unable to remove tclass: ") + nl_geterror(ret));

    Log("remove", tclass);

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

    Log("remove", qdisc);

    rtnl_qdisc_delete(sock, qdisc);

    return TError::Success();
}

TError TNetlink::AddHTB(uint32_t parent, uint32_t handle, uint32_t defaultClass) {
    int ret;
    TError error = TError::Success();
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

    Log("add", qdisc);

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

uint16_t TTclass::GetMajor() {
    return (uint16_t)(Handle >> 16);
}

TError TTclass::Create(uint32_t prio, uint32_t rate, uint32_t ceil) {
    uint32_t parent;
    if (ParentQdisc)
        parent = ParentQdisc->GetHandle();
    else
        parent = ParentTclass->Handle;

    TNetlink nl;

    TError error = nl.Open(GetDevice());
    if (error)
        return error;

    error = nl.AddClass(parent, Handle, 10, 100 * 1024 * 1024, 0);
    if (error)
        return error;

    return TError::Success();
}

TError TTclass::Remove() {
    uint32_t parent;
    if (ParentQdisc)
        parent = ParentQdisc->GetHandle();
    else
        parent = ParentTclass->Handle;

    TNetlink nl;

    TError error = nl.Open(GetDevice());
    if (error)
        return error;

    error = nl.RemoveClass(parent, Handle);
    if (error)
        return error;

    return TError::Success();
}

uint32_t TQdisc::GetHandle() {
    return Handle;
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
