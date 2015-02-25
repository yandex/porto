#pragma once

#include <string>
#include <functional>
#include <memory>

#include "common.hpp"
extern "C" {
#include <arpa/inet.h>
}

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
    bool IsEmpty();
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
};

class TNlLink : public TNonCopyable {
    std::shared_ptr<TNl> Nl;
    std::string Name;
    std::string Alias;

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
    TError AddMacVlan(const std::string &master,
                      const std::string &type, const std::string &hw,
                      int mtu);
    TError Enslave(const std::string &name);
    TError AddVeth(const std::string &name, const std::string &peerName, const std::string &hw, int mtu, int nsPid);
    const std::string &GetAlias();
    void SetAlias(const std::string &alias) { Alias = alias; }

    static bool ValidMacVlanType(const std::string &type);
    static bool ValidMacAddr(const std::string &hw);

    TError SetDefaultGw(const TNlAddr &addr);
    TError SetIpAddr(const TNlAddr &addr, const int prefix);
    bool HasQueue();

    int GetIndex();
    struct rtnl_link *GetLink() { return Link; }
    struct nl_sock *GetSock() { return Nl->GetSock(); }
    std::shared_ptr<TNl> GetNl() { return Nl; };

    void LogObj(const std::string &prefix, void *obj);
    void LogCache(struct nl_cache *cache);
};

class TNlClass : public TNonCopyable {
    std::shared_ptr<TNlLink> Link;
    const uint32_t Parent, Handle;

public:
    TNlClass(std::shared_ptr<TNlLink> link, uint32_t parent, uint32_t handle) : Link(link), Parent(parent), Handle(handle) {}

    TError Create(uint32_t prio, uint32_t rate, uint32_t ceil);
    bool Valid(uint32_t prio, uint32_t rate, uint32_t ceil);
    TError Remove();
    TError GetStat(ETclassStat stat, uint64_t &val);
    TError GetProperties(uint32_t &prio, uint32_t &rate, uint32_t &ceil);
    bool Exists();
};

class TNlHtb : public TNonCopyable {
    std::shared_ptr<TNlLink> Link;
    const uint32_t Parent, Handle;

public:
    TNlHtb(std::shared_ptr<TNlLink> link, uint32_t parent, uint32_t handle) : Link(link), Parent(parent), Handle(handle) {}
    TError Create(uint32_t defaultClass);
    TError Remove();
    bool Exists();
    bool Valid(uint32_t defaultClass);
};

class TNlCgFilter : public TNonCopyable {
    const int FilterPrio = 10;
    const char *FilterType = "cgroup";

    std::shared_ptr<TNlLink> Link;
    const uint32_t Parent, Handle;

public:
    TNlCgFilter(std::shared_ptr<TNlLink> link, uint32_t parent, uint32_t handle) : Link(link), Parent(parent), Handle(handle) {}
    TError Create();
    bool Exists();
    TError Remove();
};

TError ParseIpPrefix(const std::string &s, TNlAddr &addr, int &prefix);
