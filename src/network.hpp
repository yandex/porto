#pragma once

#include <memory>
#include <atomic>
#include <string>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <condition_variable>

#include "common.hpp"
#include "util/netlink.hpp"
#include "util/namespace.hpp"
#include "util/cred.hpp"
#include "util/idmap.hpp"

class TContainer;
struct TTaskEnv;

struct TNetStat {
    /* Class TX */
    uint64_t Packets = 0lu;
    uint64_t Bytes = 0lu;
    uint64_t Drops = 0lu;
    uint64_t Overlimits = 0lu;

    /* Device RX */
    uint64_t RxPackets = 0lu;
    uint64_t RxBytes = 0lu;
    uint64_t RxDrops = 0lu;

    /* Device TX */
    uint64_t TxPackets = 0lu;
    uint64_t TxBytes = 0lu;
    uint64_t TxDrops = 0lu;

    void operator+=(const TNetStat &a) {
        Packets += a.Packets;
        Bytes += a.Bytes;
        Drops += a.Drops;
        Overlimits += a.Overlimits;

        RxPackets += a.RxPackets;
        RxBytes += a.RxBytes;
        RxDrops += a.RxDrops;

        TxPackets += a.TxPackets;
        TxBytes += a.TxBytes;
        TxDrops += a.TxDrops;
    }
};

struct TNetClass {
    bool HostClass;
    int Registered = 0;

    uint32_t HostParentHandle;
    uint32_t ParentHandle;
    uint32_t Handle;
    uint32_t Leaf;
    TUintMap Prio;
    TUintMap Rate;
    TUintMap Limit;
    TUintMap RxLimit;

    std::map<std::string, TNetStat> Stat;

    TNetClass *Fold;
    TNetClass *Parent;
};

class TNetDevice {
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

    TNetStat Stat;
    struct nl_cache *ClassCache = nullptr;

    TNetDevice(struct rtnl_link *);

    std::string GetDesc(void) const;
    uint64_t GetConfig(const TUintMap &cfg, uint64_t def = 0) const;
    std::string GetConfig(const TStringMap &cfg, std::string def = "") const;
};

struct TNetProxyNeighbour {
    TNlAddr Ip;
    std::string Master;
};

class TNetwork : public TNonCopyable {
    friend struct TNetEnv;

    static std::mutex NetworksMutex;

    static inline std::unique_lock<std::mutex> LockNetworks() {
        return std::unique_lock<std::mutex>(NetworksMutex);
    }

    /* Copy-on-write list of networks */
    static std::shared_ptr<const std::list<std::shared_ptr<TNetwork>>> NetworksList;

    static std::unordered_map<ino_t, std::shared_ptr<TNetwork>> NetworksIndex;
    static void Register(std::shared_ptr<TNetwork> &net, ino_t inode);
    void Unregister();

    static inline std::shared_ptr<const std::list<std::shared_ptr<TNetwork>>> Networks() {
        /* FIXME not in gcc-4.7 return std::atomic_load(&NetworksList); */
        auto lock = LockNetworks();
        return NetworksList;
    }

    /* Protects netink socket operaions and external state */
    std::mutex NetMutex;

    /* Protects internal state */
    std::mutex NetStateMutex;

    /* Containers, parent before childs. Protected with NetStateMutex. */
    std::list<TNetClass *> NetClasses;

    void InitStat(TNetClass &cls);
    void RegisterClass(TNetClass &cls);
    void UnregisterClass(TNetClass &cls);

    unsigned NetUsers = 0;

    ino_t NetInode = 0;

    std::shared_ptr<TNl> Nl;
    struct nl_sock *GetSock() const { return Nl->GetSock(); }

    unsigned IfaceSeq = 0;

    std::condition_variable NetCv;

    static std::atomic<int> GlobalStatGen;
    std::atomic<int> StatGen;
    std::atomic<uint64_t> StatTime;

    TError TrySetupClasses(TNetClass &cls);

    void SyncStatLocked();
    TError RepairLocked();

    /* Something went wrong, handled by Repair */
    TError NetError;

public:
    TNetwork();
    ~TNetwork();

    std::shared_ptr<TNl> GetNl() { return Nl; };

    std::string NetName;

    static TError New(TNamespaceFd &netns, std::shared_ptr<TNetwork> &net);
    static TError Open(const TPath &path, TNamespaceFd &netns,
                       std::shared_ptr<TNetwork> &net);
    void Destroy();

    std::unique_lock<std::mutex> LockNet() {
        return std::unique_lock<std::mutex>(NetMutex);
    }

    std::unique_lock<std::mutex> LockNetState() {
        return std::unique_lock<std::mutex>(NetStateMutex);
    }

    /* Created and managed by us */
    bool ManagedNamespace = false;

    std::vector<TNetDevice> Devices;

    std::list<TNetProxyNeighbour> Neighbours;

    TError SyncDevices(bool force = false);
    std::string NewDeviceName(const std::string &prefix);
    std::string MatchDevice(const std::string &pattern);
    int DeviceIndex(const std::string &name);
    void GetDeviceSpeed(TNetDevice &dev) const;

    void FatalError(TError &error);
    void StartRepair();
    TError WaitRepair();

    TError SetupQueue(TNetDevice &dev);

    static void InitClass(TContainer &ct);

    TError SetupClass(TNetDevice &dev, TNetClass &cls);
    TError DeleteClass(TNetDevice &dev, TNetClass &cls);
    TError SetupIngress(TNetDevice &dev, TNetClass &cls);

    TError SetupClasses(TNetClass &cls);

    void SyncStat();
    static void SyncAllStat();

    TError GetGateAddress(std::vector<TNlAddr> addrs,
                          TNlAddr &gate4, TNlAddr &gate6, int &mtu, int &group);

    TError SetupProxyNeighbour(const std::vector <TNlAddr> &ip,
                               const std::string &master);

    TError AddProxyNeightbour(const std::vector<TNlAddr> &ip,
                              const std::string &master);
    void DelProxyNeightbour(const std::vector<TNlAddr> &ip);
    void RepairProxyNeightbour();

    TNlAddr NatBaseV4;
    TNlAddr NatBaseV6;
    TIdMap NatBitmap;

    TError GetNatAddress(std::vector <TNlAddr> &addrs);
    TError PutNatAddress(const std::vector <TNlAddr> &addrs);

    TError SetupAddrLabel();

    static void InitializeConfig();

    static bool NamespaceSysctl(const std::string &key);

    static TError StartNetwork(TContainer &ct, TTaskEnv &task);
    static void StopNetwork(TContainer &ct);
    static TError RestoreNetwork(TContainer &ct);

    static void NetWatchdog();
};

struct TNetDeviceConfig {
    std::string Name;
    std::string Type;
    std::string Mode;
    std::string Mac;
    std::string Master;
    int Mtu = -1;
    int Group = 0;
    bool Autoconf = false;

    std::vector<TNlAddr> Ip;
    TNlAddr Gw;

    struct {
        TNlAddr Local;
        TNlAddr Remote;
    } IpIp6;

    struct {
        uid_t Uid = NoUser;
        gid_t Gid = NoGroup;
    } Tap;
};

struct TNetEnv {
    unsigned Id;
    std::string Name;
    TCred TaskCred;

    std::shared_ptr<TContainer> Parent;
    std::shared_ptr<TNetwork> ParentNet;

    std::shared_ptr<TNetwork> Net;
    TNamespaceFd NetNs;

    bool NetInherit = false;
    bool NetIsolate = false;
    bool L3Only = true;

    bool NetUp = false;
    bool SaveIp = false;

    std::string Hostname;
    std::string NetNsName;
    std::string NetCtName;
    TMultiTuple IpRoute;

    std::vector<TNetDeviceConfig> Devices;

    TError Parse(TContainer &ct);
    TError ParseNet(TMultiTuple &net_settings);
    TError ParseIp(TMultiTuple &ip_settings);
    void FormatIp(TMultiTuple &ip_settings);
    TError ParseGw(TMultiTuple &gw_settings);

    TError CheckIpLimit();

    std::string GenerateHw(const std::string &name);
    TError ConfigureL3(TNetDeviceConfig &dev);
    TError SetupInterfaces();
    TError ApplyIpRoute();

    TError CreateTap(TNetDeviceConfig &dev);
    TError DestroyTap(TNetDeviceConfig &dev);

    TError Open(TContainer &ct);
    TError OpenNetwork();
};
