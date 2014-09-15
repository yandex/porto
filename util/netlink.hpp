#ifndef __NETLINK_H__
#define __NETLINK_H__

#include <string>

#include "error.hpp"

struct nl_sock;
struct rtnl_link;
struct nl_cache;

enum class ETclassStat {
    Packets,
    Bytes,
    Drops,
    Overlimits
};

class TNetlink {
    const int FilterPrio = 10;
    const char *FilterType = "cgroup";

    struct nl_sock *sock;
    struct rtnl_link *link;
    struct nl_cache *linkCache;

public:
    TError Open(const std::string &device);
    void Close();
    void LogObj(const std::string &prefix, void *obj);
    void LogCache(struct nl_cache *cache);
    TError AddClass(uint32_t parent, uint32_t handle, uint32_t prio, uint32_t rate, uint32_t ceil);
    TError GetStat(uint32_t handle, ETclassStat stat, uint64_t &val);
    bool ClassExists(uint32_t handle);
    TError RemoveClass(uint32_t parent, uint32_t handle);
    TError AddHTB(uint32_t parent, uint32_t handle, uint32_t defaultClass);
    TError RemoveHTB(uint32_t parent);
    TError AddCgroupFilter(uint32_t parent, uint32_t handle);
    TError RemoveCgroupFilter(uint32_t parent, uint32_t handle);
    ~TNetlink() { Close(); }
};

uint32_t TcHandle(uint16_t maj, uint16_t min);
uint32_t TcRootHandle();
uint16_t TcMajor(uint32_t handle);

#endif
