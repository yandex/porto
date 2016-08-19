#pragma once

#include <memory>
#include <string>
#include <mutex>

#include "common.hpp"
#include "util/netlink.hpp"
#include "util/locks.hpp"
#include "util/namespace.hpp"
#include "util/cred.hpp"
#include "util/idmap.hpp"

class TContainer;

enum class ENetStat {
    Packets,
    Bytes,
    Drops,
    Overlimits,

    RxPackets,
    RxBytes,
    RxDrops,

    TxPackets,
    TxBytes,
    TxDrops,
};

class TNetworkDevice {
public:
    std::string Name;
    std::string Type;
    int Index;
    int Link;
    int Group;
    int MTU;
    uint64_t Rate, Ceil;
    bool Managed;
    bool Prepared;
    bool Missing;

    TNetworkDevice(struct rtnl_link *);

    std::string GetDesc(void) const;
    uint64_t GetConfig(const TUintMap &cfg, uint64_t def = 0) const;
    std::string GetConfig(const TStringMap &cfg, std::string def = "") const;
};

class TNetwork : public std::enable_shared_from_this<TNetwork>,
                 public TNonCopyable,
                 public TLockable {
    std::shared_ptr<TNl> Nl;
    struct nl_sock *GetSock() const { return Nl->GetSock(); }

    unsigned IfaceName = 0;

public:
    std::vector<TNetworkDevice> Devices;

    TError RefreshDevices();
    TError RefreshClasses(bool force);

    bool ManagedNamespace = false;
    bool NewManagedDevices = false;

    int DeviceIndex(const std::string &name) {
        for (auto dev: Devices)
            if (dev.Name == name)
                return dev.Index;
        return 0;
    }

    TNlAddr NatBaseV4;
    TNlAddr NatBaseV6;
    TIdMap NatBitmap;

    TNetwork();
    ~TNetwork();
    TError Connect();
    TError ConnectNetns(TNamespaceFd &netns);
    TError ConnectNew(TNamespaceFd &netns);
    std::shared_ptr<TNl> GetNl() { return Nl; };

    TError Destroy();

    void GetDeviceSpeed(TNetworkDevice &dev) const;
    TError SetupQueue(TNetworkDevice &dev);

    TError DelTC(const TNetworkDevice &dev, uint32_t handle) const;

    TError CreateTC(uint32_t handle, uint32_t parent, bool leaf,
                    TUintMap &prio, TUintMap &rate, TUintMap &ceil);
    TError DestroyTC(uint32_t handle);

    TError GetDeviceStat(ENetStat kind, TUintMap &stat);
    TError GetTrafficStat(uint32_t handle, ENetStat kind, TUintMap &stat);

    TError GetGateAddress(std::vector<TNlAddr> addrs,
                          TNlAddr &gate4, TNlAddr &gate6, int &mtu);
    TError AddAnnounce(const TNlAddr &addr, std::string master);
    TError DelAnnounce(const TNlAddr &addr);

    TError GetNatAddress(std::vector <TNlAddr> &addrs);
    TError PutNatAddress(const std::vector <TNlAddr> &addrs);

    std::string NewDeviceName(const std::string &prefix);
    std::string MatchDevice(const std::string &pattern);

    static void AddNetwork(ino_t inode, std::shared_ptr<TNetwork> &net);
    static std::shared_ptr<TNetwork> GetNetwork(ino_t inode);

    static void InitializeConfig();

    static void RefreshNetworks();
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

struct TL3NetCfg {
    std::string Name;
    std::string Master;
    int Mtu;
    std::vector<TNlAddr> Addrs;
    bool Nat;
};

class TContainerHolder;
class TContainer;

struct TNetCfg {
    std::shared_ptr<TContainerHolder> Holder;
    std::shared_ptr<TContainer> Parent;
    std::shared_ptr<TNetwork> ParentNet;
    std::shared_ptr<TNetwork> Net;
    unsigned Id;
    TCred OwnerCred;
    bool NewNetNs;
    bool Inherited;
    bool NetUp;
    bool SaveIp;
    std::string Hostname;
    std::vector<std::string> Steal;
    std::vector<TMacVlanNetCfg> MacVlan;
    std::vector<TIpVlanNetCfg> IpVlan;
    std::vector<TVethNetCfg> Veth;
    std::vector<TL3NetCfg> L3lan;
    std::string NetNsName;
    std::string NetCtName;
    std::vector<TGwVec> GwVec;
    std::vector<TIpVec> IpVec;
    std::vector<std::string> Autoconf;

    TNamespaceFd NetNs;

    void Reset();
    TError ParseNet(std::vector<std::string> lines);
    TError ParseIp(std::vector<std::string> lines);
    TError FormatIp(std::vector<std::string> &lines);
    TError ParseGw(std::vector<std::string> lines);
    std::string GenerateHw(const std::string &name);
    TError ConfigureVeth(TVethNetCfg &veth);
    TError ConfigureL3(TL3NetCfg &l3);
    TError ConfigureInterfaces();
    TError PrepareNetwork();
    TError DestroyNetwork();
};

extern std::shared_ptr<TNetwork> HostNetwork;
