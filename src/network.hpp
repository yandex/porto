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
#include "util/hgram.hpp"

class TContainer;
class TNetwork;
struct TNetDeviceConfig;
struct TTaskEnv;

struct TNetStat {
    uint64_t TxBytes = 0lu;
    uint64_t TxPackets = 0lu;
    uint64_t TxDrops = 0lu;
    uint64_t TxOverruns = 0lu;

    uint64_t RxBytes = 0lu;
    uint64_t RxPackets = 0lu;
    uint64_t RxDrops = 0lu;
    uint64_t RxOverruns = 0lu;

    uint64_t UpdateTs = 0lu;

    void operator+=(const TNetStat &a) {
        TxBytes += a.TxBytes;
        TxPackets += a.TxPackets;
        TxDrops += a.TxDrops;
        TxOverruns += a.TxOverruns;
        RxBytes += a.RxBytes;
        RxPackets += a.RxPackets;
        RxDrops += a.RxDrops;
        RxOverruns += a.RxOverruns;
    }

    void Reset() {
        TxBytes = 0;
        TxPackets = 0;
        TxDrops = 0;
        TxOverruns = 0;
        RxBytes = 0;
        RxPackets = 0;
        RxDrops = 0;
        RxOverruns = 0;
    }
};

struct TNetClass {
    int Registered = 0;
    int Owner = 0;

    uint32_t BaseHandle;
    uint32_t MetaHandle;
    uint32_t LeafHandle;

    int DefaultTos = 0;
    TUintMap TxRate;
    TUintMap TxLimit;
    TUintMap RxLimit;

    std::map<std::string, TNetStat> ClassStat;
    std::shared_ptr<TNetwork> OriginNet;

    TNetClass *Fold;
    TNetClass *Parent;

    static bool IsDisabled();
};

class TNetDevice {
public:
    std::string Name;
    std::string Type;
    std::string Qdisc;
    int Owner = 0;
    int Index;
    int Link;
    int MTU;
    int TxQueues;
    int Group;
    std::string GroupName;
    uint64_t Rate, Ceil;
    bool Managed;
    bool Uplink;
    bool Prepared;
    bool Missing;

    TNetStat DeviceStat;

    struct nl_cache *ClassCache = nullptr;

    TNetDevice(struct rtnl_link *);

    uint64_t GetConfig(const TUintMap &cfg, uint64_t def = 0, int cs = -1) const;
    std::string GetConfig(const TStringMap &cfg, std::string def = "", int cs = -1) const;
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

    static std::mutex NetStateMutex;

    /* Copy-on-write list of networks */
    static std::shared_ptr<const std::list<std::shared_ptr<TNetwork>>> NetworksList;

    static std::unordered_map<ino_t, std::shared_ptr<TNetwork>> NetworksIndex;
    static void Register(std::shared_ptr<TNetwork> &net, ino_t inode);
    static TError GetSockets(const std::vector<pid_t> &pids, std::unordered_set<ino_t> &sockets);
    void Unregister();

    static inline std::shared_ptr<const std::list<std::shared_ptr<TNetwork>>> Networks() {
        /* FIXME not in gcc-4.7 return std::atomic_load(&NetworksList); */
        auto lock = LockNetworks();
        return NetworksList;
    }

    const bool NetIsHost;

    /* Protects netink socket operaions and external state */
    std::mutex NetMutex;

    /* Containers, parent before childs. Protected with NetStateMutex. */
    std::list<TNetClass *> NetClasses;

    TNetClass DefaultClass;
    TNetClass *RootClass = nullptr;

    void InitStat(TNetClass &cls);
    void RegisterClass(TNetClass &cls);
    void UnregisterClass(TNetClass &cls);

    std::list<TContainer *> NetUsers;

    ino_t NetInode = 0;

    std::shared_ptr<TNl> Nl;

    unsigned IfaceSeq = 0;

    std::condition_variable NetCv;

    static std::atomic<int> GlobalStatGen;
    std::atomic<int> StatGen;
    std::atomic<uint64_t> StatTime;

    TError TrySetupClasses(TNetClass &cls, bool safe = false);

    void SyncStatLocked();
    TError Reconnect();
    TError RepairLocked();

    /* Something went wrong, handled by Repair */
    TError NetError;

public:
    TNetwork(bool host = false);
    ~TNetwork();

    bool IsHost() const { return NetIsHost; }

    std::shared_ptr<TNl> GetNl() { return Nl; }
    struct nl_sock *GetSock() const { return Nl->GetSock(); }

    std::string NetName;

    static TError New(TNamespaceFd &netns, std::shared_ptr<TNetwork> &net, pid_t netnsPid = 0);
    static TError Open(const TPath &path, TNamespaceFd &netns,
                       std::shared_ptr<TNetwork> &net,
                       bool host = false);
    void Destroy();

    std::unique_lock<std::mutex> LockNet() {
        return std::unique_lock<std::mutex>(NetMutex);
    }

    static inline std::unique_lock<std::mutex> LockNetState() {
        return std::unique_lock<std::mutex>(NetStateMutex);
    }

    /* Created and managed by us */
    bool ManagedNamespace = false;

    std::vector<TNetDevice> Devices;

    std::map<std::string, TNetStat> DeviceStat;
    std::shared_ptr<THistogram> RxSpeedHgram;
    std::shared_ptr<THistogram> TxSpeedHgram;
    uint64_t RxMaxSpeed = 0;
    uint64_t TxMaxSpeed = 0;

    std::list<TNetProxyNeighbour> Neighbours;

    std::map<std::string, int> DeviceOwners;

    bool EnabledRxLimit = false;

    TError SyncDevices();
    std::string NewDeviceName(const std::string &prefix);
    std::string MatchDevice(const std::string &pattern);
    int DeviceIndex(const std::string &name);
    std::string GetDeviceQdisc(const TNetDevice &dev);
    void GetDeviceSpeed(TNetDevice &dev) const;
    void SetDeviceOwner(const std::string &name, int owner);

    void StartRepair();
    TError WaitRepair();

    TError SetupQueue(TNetDevice &dev, bool force);

    static void InitClass(TContainer &ct);

    TError SetupClass(TNetDevice &dev, TNetClass &cls, int cs, bool safe = false);
    TError DeleteClass(TNetDevice &dev, TNetClass &cls, int cs);
    TError SetupClasses(TNetClass &cls, bool safe = false);
    TError SetupPolice(TNetDevice &dev);
    TError SetupRxLimit(TNetDevice &dev, std::unique_lock<std::mutex> &statLock);

    void InitClasslessQdisc(TNetDevice &dev, TNlQdisc &qdisc);
    TError SetupMQ(TNetDevice &dev);

    void SyncStat();
    static void SyncAllStat();

    /* Network stats from /proc/net/netstat */
    TUintMap NetStat;
    TUintMap NetSnmp;

    TError GetL3Gate(TNetDeviceConfig &dev);

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
    static std::string DeviceGroupName(int group);

    static int DefaultTos;
    static TError ParseTos(const std::string &str, int &tos);
    static std::string FormatTos(int tos);

    static bool NetworkSysctl(const std::string &key);
    static bool NamespaceSysctl(const std::string &key);

    static TError StartNetwork(TContainer &ct, TTaskEnv &task);
    static void StopNetwork(TContainer &ct);
    static TError RestoreNetwork(TContainer &ct);

    static void NetWatchdog();
    static void L3StatWatchdog();

    static TError SyncResolvConf();
    static void UpdateSockDiag();
    static void RepairSockDiag();

    static void UpdateProcNetStats(const std::string &basename);
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
    bool EnableECN = false;
    bool EnableExtraRoutes = false;

    std::vector<TNlAddr> Ip;
    TNlAddr Gate4;
    TNlAddr Gate6;
    int GateMtu4 = -1;
    int GateMtu6 = -1;

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
    bool NetNone = false;
    bool L3Only = true;

    bool NetUp = false;
    bool SaveIp = false;
    bool EnableECN = false;

    std::string Hostname;
    std::string NetNsName;
    std::string NetCtName;
    TMultiTuple IpRoute;
    TStringMap NetSysctl;

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
    TError ApplySysctl();
    TError AddRoute(const std::string &dsc, int index, struct nl_addr *via, int mtu, int advmss);

    TError CreateTap(TNetDeviceConfig &dev);
    TError DestroyTap(TNetDeviceConfig &dev);

    TError Open(TContainer &ct);
    TError OpenNetwork(TContainer &ct);
};
