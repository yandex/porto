#ifndef __NETLINK_H__
#define __NETLINK_H__

#include <string>
#include <functional>

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
    NO_COPY_CONSTRUCT(TNetlink);
    const int FilterPrio = 10;
    const char *FilterType = "cgroup";

    struct nl_sock *sock = nullptr;
    struct rtnl_link *link = nullptr;
    struct nl_cache *linkCache = nullptr;

public:
    TError FindDev(std::string &device);
    std::vector<std::string> FindDev(int flags);

    TNetlink() {}
    TError Open(const std::string device, bool getLink = true);
    void Close();
    void LogObj(const std::string &prefix, void *obj);
    void LogCache(struct nl_cache *cache);
    TError AddClass(uint32_t parent, uint32_t handle, uint32_t prio, uint32_t rate, uint32_t ceil);
    TError GetStat(uint32_t handle, ETclassStat stat, uint64_t &val);
    TError GetClassProperties(uint32_t handle, uint32_t &prio, uint32_t &rate, uint32_t &ceil);
    bool ClassExists(uint32_t handle);
    TError RemoveClass(uint32_t parent, uint32_t handle);
    TError AddHTB(uint32_t parent, uint32_t handle, uint32_t defaultClass);
    bool QdiscExists(uint32_t handle);
    TError RemoveHTB(uint32_t parent);
    TError AddCgroupFilter(uint32_t parent, uint32_t handle);
    bool CgroupFilterExists(uint32_t parent, uint32_t handle);
    TError RemoveCgroupFilter(uint32_t parent, uint32_t handle);
    ~TNetlink() { Close(); }
    int GetLinkIndex(const std::string &device);
    TError AddMacVlan(const std::string &name, const std::string &master,
                      const std::string &type, const std::string &hw);
    TError RemoveLink(const std::string &name);
    TError LinkUp(const std::string &name);
    TError ChangeLinkNs(const std::string &name, const std::string &newName,
                        int pid);
    TError AddMacVlan(const std::string &name, const std::string &master,
                      const std::string &type, const std::string &hw,
                      int nsPid);
    bool ValidLink(const std::string &name);

    static bool ValidMacVlanType(const std::string &type);
    static bool ValidMacAddr(const std::string &hw);

    static void EnableDebug(bool enable);
    static TError Exec(std::string device, std::function<TError(TNetlink &nl)> f, bool getLink = true);
};

uint32_t TcHandle(uint16_t maj, uint16_t min);
uint32_t TcRootHandle();
uint16_t TcMajor(uint32_t handle);

static inline bool ValidLink(const std::string &name) {
    TError error = TNetlink::Exec(config().network().device(),
        [&](TNetlink &nl) {
            if (nl.ValidLink(name))
                return TError::Success();
            else
                return TError(EError::Unknown, "");
        });

    return error == TError::Success();
}

#endif
