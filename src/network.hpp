#pragma once

#include <memory>
#include <string>
#include <mutex>

#include "common.hpp"
#include "util/netlink.hpp"
#include "util/locks.hpp"

class TNetwork : public std::enable_shared_from_this<TNetwork>,
                 public TNonCopyable,
                 public TLockable {
    std::shared_ptr<TNl> Nl;
    struct nl_sock *GetSock() const { return Nl->GetSock(); }

    std::vector<std::pair<std::string, int>> ifaces;

    TError PrepareLink(int index, std::string name);

public:
    TError UpdateInterfaces();

    TNetwork();
    ~TNetwork();
    TError Connect();
    TError Prepare();
    TError Destroy();

    TError GetTrafficCounters(int minor, ETclassStat stat,
                              std::map<std::string, uint64_t> &result);

    TError UpdateTrafficClasses(int parent, int minor,
            std::map<std::string, uint64_t> &Prio,
            std::map<std::string, uint64_t> &Rate,
            std::map<std::string, uint64_t> &Ceil);
    TError RemoveTrafficClasses(int minor);

    TError AddTrafficClass(int ifIndex, uint32_t parent, uint32_t handle,
                           uint64_t prio, uint64_t rate, uint64_t ceil);
    TError DelTrafficClass(int ifIndex, uint32_t handle);
};


struct THostNetCfg {
    std::string Dev;
};

struct TMacVlanNetCfg {
    std::string Master;
    std::string Name;
    std::string Type;
    std::string Hw;
    int Mtu;
};

struct TIpVlanNetCfg {
    std::string Master;
    std::string Name;
    std::string Mode;
    int Mtu;
};

struct TIpVec {
    std::string Iface;
    TNlAddr Addr;
    int Prefix;
};

struct TGwVec {
    std::string Iface;
    TNlAddr Addr;
};

struct TVethNetCfg {
    std::string Bridge;
    std::string Name;
    std::string Hw;
    std::string Peer;
    int Mtu;
};

struct TNetCfg {
    unsigned Id;
    bool NewNetNs;
    bool Inherited;
    bool Host;
    std::vector<THostNetCfg> HostIface;
    std::vector<TMacVlanNetCfg> MacVlan;
    std::vector<TIpVlanNetCfg> IpVlan;
    std::vector<TVethNetCfg> Veth;
    std::string NetNsName;
    std::string NetCtName;
    std::vector<TGwVec> GwVec;
    std::vector<TIpVec> IpVec;

    void Reset();
    TError ParseNet(std::vector<std::string> lines);
    TError ParseIp(std::vector<std::string> lines);
    TError ParseGw(std::vector<std::string> lines);
};
