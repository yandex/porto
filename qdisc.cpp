#include "qdisc.hpp"

extern "C" {
#include <net/if.h>

#include <libmnl/libmnl.h>
#include <linux/if_link.h>
#include <linux/rtnetlink.h>
}

TError TTclass::Create() {
	//RTM_NEWTCLASS

    return TError::Success();
}

TError TTclass::Remove() {
	//RTM_DELTCLASS

    return TError::Success();
}

TError TQdisc::Create() {
#if 0
    auto iface = if_nametoindex(Device.c_str());
    if (iface < 0)
        return TError(EError::Unknown, errno, "Can't convert device name to index");


	char buf[MNL_SOCKET_BUFFER_SIZE];



	struct nlmsghdr *nlh = mnl_nlmsg_put_header(buf);
	nlh->nlmsg_type	= RTM_NEWQDISC,;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE; // | NLM_F_ACK;


    struct tcmsg *tcm = mnl_nlmsg_put_extra_header(nlh, sizeof(*tcm));
    tcm->tcm_ifindex = iface;
    tcm->tcm_family = AF_UNSPEC;
    tcm->tcm_parent = TC_H_ROOT; // TODO: get parent





	struct rtmsg *rtm = mnl_nlmsg_put_extra_header(nlh, sizeof(struct rtmsg));
	rtm->rtm_family = family;
	rtm->rtm_dst_len = prefix;
	rtm->rtm_src_len = 0;
	rtm->rtm_tos = 0;
	rtm->rtm_protocol = RTPROT_STATIC;
	rtm->rtm_table = RT_TABLE_MAIN;
	rtm->rtm_type = RTN_UNICAST;
	/* is there any gateway? */
	rtm->rtm_scope = (argc == 4) ? RT_SCOPE_LINK : RT_SCOPE_UNIVERSE;
	rtm->rtm_flags = 0;

	if (family == AF_INET)
		mnl_attr_put_u32(nlh, RTA_DST, dst.ip);
	else
		mnl_attr_put(nlh, RTA_DST, sizeof(struct in6_addr), &dst);

	mnl_attr_put_u32(nlh, RTA_OIF, iface);


	nl = mnl_socket_open(NETLINK_ROUTE);
	if (nl == NULL) {
		perror("mnl_socket_open");
		exit(EXIT_FAILURE);
	}

	if (mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
		perror("mnl_socket_bind");
		exit(EXIT_FAILURE);
	}
	portid = mnl_socket_get_portid(nl);

	if (mnl_socket_sendto(nl, nlh, nlh->nlmsg_len) < 0) {
		perror("mnl_socket_sendto");
		exit(EXIT_FAILURE);
	}

	ret = mnl_socket_recvfrom(nl, buf, sizeof(buf));
	if (ret < 0) {
		perror("mnl_socket_recvfrom");
		exit(EXIT_FAILURE);
	}

    // TODO

	mnl_socket_close(nl);
#endif

    return TError::Success();
}

TError TQdisc::Remove() {
    // RTM_DELQDISC

    return TError::Success();
}
