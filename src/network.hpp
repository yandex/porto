#pragma once

#include <memory>
#include <string>
#include <mutex>
#include <thread>
#include <condition_variable>

#include "common.hpp"
#include "util/netlink.hpp"
#include "util/locks.hpp"
#include "util/namespace.hpp"
#include "util/cred.hpp"
#include "util/idmap.hpp"

extern "C" {
#include <netlink/route/link.h>
#include <netlink/route/tc.h>
}

class TContainer;

struct TNetStats {
    uint64_t Packets = 0lu;
    uint64_t Bytes = 0lu;
    uint64_t Drops = 0lu;
    uint64_t Overlimits = 0lu;

    uint64_t RxPackets = 0lu;
    uint64_t RxBytes = 0lu;
    uint64_t RxDrops = 0lu;

    uint64_t TxPackets = 0lu;
    uint64_t TxBytes = 0lu;
    uint64_t TxDrops = 0lu;
};

class TNetworkDevice {
public:
    std::string Name;
    std::string Type;
    int Index;
    int Link;
    int Group;
    std::string GroupName;
    int MTU;
    uint64_t Rate, Ceil;
    bool Managed;
    bool Prepared;
    bool Missing;

    TNetStats Stats;

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

    TError RefreshDevices(bool force = false);
    uint64_t SumChildrenStat(struct nl_cache *cache, uint32_t handle,
                             rtnl_tc_stat kind);
    void RefreshStats(std::list<std::shared_ptr<TContainer>> &subtree);
    TError RefreshClasses(std::list<std::shared_ptr<TContainer>> &subtree);

    bool ManagedNamespace = false;
    bool NewManagedDevices = false;
    bool NeedRefresh = false;

    int DeviceIndex(const std::string &name) {
        for (auto dev: Devices)
            if (dev.Name == name)
                return dev.Index;
        return 0;
    }

    TNlAddr NatBaseV4;
    TNlAddr NatBaseV6;
    TIdMap NatBitmap;
    unsigned Owners = 1;

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

    TError CreateTC(uint32_t handle, uint32_t parent, uint32_t leaf,
                    TUintMap &prio, TUintMap &rate, TUintMap &ceil);
    TError DestroyTC(uint32_t handle, uint32_t leaf);

    TError GetGateAddress(std::vector<TNlAddr> addrs,
                          TNlAddr &gate4, TNlAddr &gate6, int &mtu, int &group);
    TError AddAnnounce(const TNlAddr &addr, std::string master);
    TError DelAnnounce(const TNlAddr &addr);

    TError GetNatAddress(std::vector <TNlAddr> &addrs);
    TError PutNatAddress(const std::vector <TNlAddr> &addrs);

    std::string NewDeviceName(const std::string &prefix);
    std::string MatchDevice(const std::string &pattern);

    TError CreateIngressQdisc(TUintMap &rate);

    static void AddNetwork(ino_t inode, std::shared_ptr<TNetwork> &net);
    static std::shared_ptr<TNetwork> GetNetwork(ino_t inode);

    static void InitializeConfig();

    static bool NamespaceSysctl(const std::string &key);
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
    int Group = 0;
};

struct TIpIp6NetCfg {
    std::string Name;
    TNlAddr Local;
    TNlAddr Remote;
    int EncapLimit;
    int Ttl;
    int Mtu;
    bool DefaultRoute;
};

class TContainer;

struct TNetCfg {
    std::shared_ptr<TContainer> Parent;
    std::shared_ptr<TNetwork> ParentNet;
    std::shared_ptr<TNetwork> Net;
    unsigned Id = 0;
    TCred OwnerCred;
    bool NewNetNs = false;
    bool Inherited = false;
    bool NetUp = false;
    bool SaveIp = false;
    bool L3Only = true;
    std::string Hostname;
    std::vector<std::string> Steal;
    std::vector<TMacVlanNetCfg> MacVlan;
    std::vector<TIpVlanNetCfg> IpVlan;
    std::vector<TVethNetCfg> Veth;
    std::vector<TL3NetCfg> L3lan;
    std::vector<TIpIp6NetCfg> IpIp6;
    std::string NetNsName;
    std::string NetCtName;
    std::vector<TGwVec> GwVec;
    std::vector<TIpVec> IpVec;
    std::vector<std::string> Autoconf;

    TNamespaceFd NetNs;

    void Reset();
    TError ParseNet(TMultiTuple &net_settings);
    TError ParseIp(TMultiTuple &ip_settings);
    void FormatIp(TMultiTuple &ip_settings);
    TError ParseGw(TMultiTuple &gw_settings);
    std::string GenerateHw(const std::string &name);
    TError ConfigureVeth(TVethNetCfg &veth);
    TError ConfigureL3(TL3NetCfg &l3);
    TError ConfigureInterfaces();
    TError PrepareNetwork();
    TError DestroyNetwork();
};

extern std::shared_ptr<TNetwork> HostNetwork;
constexpr const char PORTOD_NETWORKER_NAME[] = "portod-network";

class TNetWorker {
    bool WorkPending = false;
    bool Shutdown = true;
    std::thread Thread;
    std::condition_variable Cv;
    bool StatsNeeded = false;

public:
    void Start();
    void Stop();
    void Wake();
    void Loop();

    TError RefreshNetwork(std::shared_ptr<TContainer> ct);
    void RefreshStats(std::shared_ptr<TContainer> ct, bool force);
};

extern TNetWorker NetWorker;
