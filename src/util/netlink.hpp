#pragma once

#include <string>
#include <functional>
#include <memory>

#include "common.hpp"
extern "C" {
#include <arpa/inet.h>
#include <linux/netlink.h>
#include <linux/pkt_cls.h>
}

struct nl_sock;
struct rtnl_link;
struct nl_cache;
struct nl_addr;
class TNlLink;

class TNlAddr {
public:
    struct nl_addr *Addr = nullptr;

    TNlAddr() {}
    TNlAddr(struct nl_addr *addr);
    TNlAddr(const TNlAddr &other);
    TNlAddr &operator=(const TNlAddr &other);
    ~TNlAddr();

    void Forget();

    TError Parse(int family, const TString &string);
    TString Format() const;

    int Family() const;
    bool IsEmpty() const;
    bool IsHost() const;
    bool IsEqual(const TNlAddr &addr) const;
    bool IsMatch(const TNlAddr &addr) const;

    unsigned int Length() const;
    const void * Binary() const;
    unsigned int Prefix() const;

    void AddOffset(uint64_t offset);
    uint64_t GetOffset(const TNlAddr &base) const;
    void GetRange(TNlAddr &base, uint64_t &count) const;
    TError GetRange(std::vector<TNlAddr> &addrs, int max) const;
};

uint32_t TcHandle(uint16_t maj, uint16_t min);

class TNl : public std::enable_shared_from_this<TNl>,
            public TPortoNonCopyable {
    struct nl_sock *Sock = nullptr;

public:

    TNl() {}
    ~TNl() { Disconnect(); }

    TError Connect();
    void Disconnect();

    struct nl_sock *GetSock() const { return Sock; }

    int GetFd();

    static TError Error(int nl_err, const TString &desc);
    void Dump(const TString &prefix, void *obj) const;
    void DumpCache(struct nl_cache *cache) const;

    TError ProxyNeighbour(int ifindex, const TNlAddr &addr, bool add);
    TError PermanentNeighbour(int ifindex, const TNlAddr &addr,
                              const TNlAddr &lladdr, bool add);
    TError AddrLabel(const TNlAddr &prefix, uint32_t label);
};

class TNlLink : public TPortoNonCopyable {
    std::shared_ptr<TNl> Nl;
    struct rtnl_link *Link = nullptr;

    TError AddXVlan(const TString &vlantype,
                    const TString &master,
                    uint32_t type,
                    const TString &hw,
                    int mtu);

public:

    TNlLink(std::shared_ptr<TNl> sock, const TString &name, int index = 0);
    TNlLink(std::shared_ptr<TNl> sock, struct rtnl_link *link);
    ~TNlLink();
    TError Load();

    int GetIndex() const;
    int GetGroup() const;
    TNlAddr GetAddr() const;
    TString GetName() const;
    TString GetType() const;
    TString GetDesc() const;
    bool IsLoopback() const;
    bool IsRunning() const;
    TError Error(int nl_err, const TString &desc) const;
    void Dump(const TString &prefix, void *obj = nullptr) const;

    TError Remove();
    TError Up();
    TError Enslave(const TString &name);
    TError ChangeNs(const TString &newName, int nsFd);
    TError AddIpVlan(const TString &master,
                     const TString &mode, int mtu);
    TError AddMacVlan(const TString &master,
                      const TString &type, const TString &hw,
                      int mtu);
    TError AddVeth(const TString &name, const TString &hw, int mtu,
                   int group, int nsFd);
    TError AddIp6Tnl(const TString &name,
                     const TNlAddr &remote, const TNlAddr &local,
                     int type, int mtu, int encap_limit, int ttl);

    static bool ValidIpVlanMode(const TString &mode);
    static bool ValidMacVlanType(const TString &type);
    static bool ValidMacAddr(const TString &hw);

    TError AddDirectRoute(const TNlAddr &addr, bool ecn = false);
    TError SetDefaultGw(const TNlAddr &addr, bool ecn = false, int mtu = -1);
    TError AddAddress(const TNlAddr &addr);
    TError WaitAddress(int timeout_s);
    int GetMtu();
    TError SetMtu(int mtu);
    TError SetGroup(int group);
    TError SetMacAddr(const TString &mac);

    struct nl_sock *GetSock() const { return Nl->GetSock(); }
    std::shared_ptr<TNl> GetNl() { return Nl; };
};

class TNlQdisc {
public:
    int Index;
    uint32_t Parent, Handle;
    TString Kind;
    uint32_t Default = 0;
    uint32_t Limit = 0;
    uint32_t Quantum = 0;
    TNlQdisc(int index, uint32_t parent, uint32_t handle) :
        Index(index), Parent(parent), Handle(handle) {}

    TError Create(const TNl &nl);
    TError Delete(const TNl &nl);
    bool Check(const TNl &nl);
};

class TNlClass {
public:
    int Index = 0;
    uint32_t Parent = -1;
    uint32_t Handle = -1;
    TString Kind;
    uint64_t Rate = 0;
    uint64_t defRate = 0;
    uint64_t Ceil = 0;
    uint64_t RateBurst = 0;
    uint64_t CeilBurst = 0;
    uint64_t Quantum = 0;
    uint64_t MTU = 0;

    TNlClass() {}
    TNlClass(int index, uint32_t parent, uint32_t handle) :
        Index(index), Parent(parent), Handle(handle) {}

    TError Create(const TNl &nl);
    TError Delete(const TNl &nl);
    TError Load(const TNl &nl);
    bool Exists(const TNl &nl);
};

class TNlCgFilter : public TPortoNonCopyable {
    const int Index;
    const int FilterPrio = 10;
    const char *FilterType = "cgroup";
    const uint32_t Parent, Handle;

public:
    TNlCgFilter(int index, uint32_t parent, uint32_t handle) :
        Index(index), Parent(parent), Handle(handle) {}
    TError Create(const TNl &nl);
    bool Exists(const TNl &nl);
    TError Delete(const TNl &nl);
};

class TNlPoliceFilter {
public:
    const char *FilterType = "u32";
    int Index = 0;
    int FilterPrio = 10;
    uint32_t Parent = -1;
    uint32_t Rate = 0;
    uint32_t PeakRate = 0;
    uint32_t Mtu = 65536;
    uint32_t Burst = 65536;
    uint32_t Action = TC_ACT_SHOT;

    TNlPoliceFilter(int index, uint32_t parent) : Index(index), Parent(parent) {}
    TError Create(const TNl &nl);
    TError Delete(const TNl &nl);
};
