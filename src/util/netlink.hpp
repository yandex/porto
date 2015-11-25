#pragma once

#include <string>
#include <functional>
#include <memory>

#include "common.hpp"
extern "C" {
#include <arpa/inet.h>
#include <linux/netlink.h>
}

const uint64_t NET_MAX_LIMIT = 0xFFFFFFFF;
const uint64_t NET_MAX_GUARANTEE = 0xFFFFFFFF;
const uint64_t NET_MAP_WHITEOUT = 0xFFFFFFFFFFFFFFFF;

struct nl_sock;
struct rtnl_link;
struct nl_cache;
struct nl_addr;

class TNlAddr {
    struct nl_addr *Addr = nullptr;
public:
    TNlAddr() {}
    TNlAddr(const TNlAddr &other);
    TNlAddr &operator=(const TNlAddr &other);
    ~TNlAddr();
    TError Parse(const std::string &s);
    struct nl_addr *GetAddr() const { return Addr; }
    bool IsEmpty() const;
};

enum class ETclassStat {
    Packets,
    Bytes,
    Drops,
    Overlimits,
    BPS,
    PPS,
};

uint32_t TcHandle(uint16_t maj, uint16_t min);
uint32_t TcRootHandle();
uint16_t TcMajor(uint32_t handle);

class TNl : public TNonCopyable {
    struct nl_sock *Sock = nullptr;
    struct nl_cache *LinkCache = nullptr;

public:

    TNl() {}
    ~TNl() { Disconnect(); }

    TError Connect();
    void Disconnect();
    std::vector<std::string> FindLink(int flags);
    bool ValidLink(const std::string &name);

    static void EnableDebug(bool enable);

    struct nl_sock *GetSock() { return Sock; }
    struct nl_cache *GetCache() { return LinkCache; }

    TError GetDefaultLink(std::vector<std::string> &links);
    int GetFd();
    TError SubscribeToLinkUpdates();
    void FlushEvents();
    TError RefillCache();
};

class TNlLink : public TNonCopyable {
    std::shared_ptr<TNl> Nl;
    std::string Name;
    struct nl_cache *ClassCache = nullptr;

    TError AddXVlan(const std::string &vlantype,
                    const std::string &master,
                    uint32_t type,
                    const std::string &hw,
                    int mtu);

public:
    struct rtnl_link *Link = nullptr;

    TNlLink(std::shared_ptr<TNl> nl, const std::string &name) : Nl(nl), Name(name) {}
    ~TNlLink();
    TError Load();

    TError Remove();
    TError Up();
    TError ChangeNs(const std::string &newName, int pid);
    bool Valid();
    int FindIndex(const std::string &device);
    TError RefillCache();
    TError AddIpVlan(const std::string &master,
                     const std::string &mode, int mtu);
    TError AddMacVlan(const std::string &master,
                      const std::string &type, const std::string &hw,
                      int mtu);
    TError Enslave(const std::string &name);
    TError AddVeth(const std::string &name, const std::string &peerName, const std::string &hw, int mtu, int nsPid);
    const std::string &GetAlias() const;

    static bool ValidIpVlanMode(const std::string &mode);
    static bool ValidMacVlanType(const std::string &type);
    static bool ValidMacAddr(const std::string &hw);

    TError SetDefaultGw(const TNlAddr &addr);
    TError SetIpAddr(const TNlAddr &addr, const int prefix);
    bool HasQueue();
    bool IsLoopback();

    int GetIndex() const;
    struct rtnl_link *GetLink() const { return Link; }
    struct nl_sock *GetSock() const { return Nl->GetSock(); }
    std::shared_ptr<TNl> GetNl() { return Nl; };

    void LogObj(const std::string &prefix, void *obj) const;
    void LogCache(struct nl_cache *cache) const;
    TError RefillClassCache();
    struct nl_cache *GetClassCache() const { return ClassCache; }
};

class TNlClass : public TNonCopyable {
    const uint32_t Parent, Handle;

public:
    TNlClass(uint32_t parent, uint32_t handle) : Parent(parent), Handle(handle) {}

    TError Create(TNlLink &link, uint32_t prio, uint32_t rate, uint32_t ceil);
    bool Valid(const TNlLink &link, uint32_t prio, uint32_t rate, uint32_t ceil);
    TError Remove(TNlLink &link);
    TError GetStat(TNlLink &link, ETclassStat stat, uint64_t &val);
    TError GetProperties(const TNlLink &link, uint32_t &prio, uint32_t &rate, uint32_t &ceil);
    bool Exists(const TNlLink &link);
};

class TNlHtb : public TNonCopyable {
    const uint32_t Parent, Handle;

public:
    TNlHtb(uint32_t parent, uint32_t handle) : Parent(parent), Handle(handle) {}
    TError Create(const TNlLink &link, uint32_t defaultClass);
    TError Remove(const TNlLink &link);
    bool Exists(const TNlLink &link);
    bool Valid(const TNlLink &link, uint32_t defaultClass);
};

class TNlCgFilter : public TNonCopyable {
    const int FilterPrio = 10;
    const char *FilterType = "cgroup";
    const uint32_t Parent, Handle;

public:
    TNlCgFilter(uint32_t parent, uint32_t handle) : Parent(parent), Handle(handle) {}
    TError Create(const TNlLink &link);
    bool Exists(const TNlLink &link);
    TError Remove(const TNlLink &link);
};

TError ParseIpPrefix(const std::string &s, TNlAddr &addr, int &prefix);
