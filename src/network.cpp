#include <algorithm>
#include <unordered_map>
#include <sstream>
#include <fstream>

#include "network.hpp"
#include "container.hpp"
#include "holder.hpp"
#include "config.hpp"
#include "client.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/crc32.hpp"

extern "C" {
#include <linux/if.h>
#include <netlink/route/addr.h>
#include <netlink/route/link.h>
#include <netlink/route/tc.h>
#include <netlink/route/class.h>
#include <netlink/route/qdisc/htb.h>
}

std::shared_ptr<TNetwork> HostNetwork;
static std::unordered_map<ino_t, std::weak_ptr<TNetwork>> Networks;
static std::mutex NetworksMutex;

static std::vector<std::string> UnmanagedDevices;
static std::vector<int> UnmanagedGroups;

static TStringMap DeviceQdisc;
static TUintMap DeviceRate;
static TUintMap DeviceCeil;
static TUintMap DeviceRateBurst;
static TUintMap DeviceCeilBurst;
static TUintMap DeviceQuantum;

static TUintMap PortoRate;

static TUintMap DefaultRate;
static TStringMap DefaultQdisc;
static TUintMap DefaultQdiscLimit;
static TUintMap DefaultQdiscQuantum;

static TUintMap ContainerRate;
static TStringMap ContainerQdisc;
static TUintMap ContainerQdiscLimit;

static inline std::unique_lock<std::mutex> LockNetworks() {
    return std::unique_lock<std::mutex>(NetworksMutex);
}

TNetworkDevice::TNetworkDevice(struct rtnl_link *link) {
    Name = rtnl_link_get_name(link);
    Type = rtnl_link_get_type(link) ?: "";
    Index = rtnl_link_get_ifindex(link);
    Link = rtnl_link_get_link(link);
    Group = rtnl_link_get_group(link);
    MTU = rtnl_link_get_mtu(link);

    Rate = NET_MAX_RATE;
    Ceil = NET_MAX_RATE;

    Managed = true;
    Prepared = false;
    Missing = false;

    for (auto &pattern: UnmanagedDevices)
        if (StringMatch(Name, pattern))
            Managed = false;

    if (std::find(UnmanagedGroups.begin(), UnmanagedGroups.end(), Group) !=
            UnmanagedGroups.end())
        Managed = false;
}

std::string TNetworkDevice::GetDesc(void) const {
    return std::to_string(Index) + ":" + Name + " (" + Type + ")";
}

uint64_t TNetworkDevice::GetConfig(const TUintMap &cfg, uint64_t def) const {
    for (auto &it: cfg) {
        if (StringMatch(Name, it.first))
            return it.second;
    }
    auto it = cfg.find("default");
    if (it != cfg.end())
        return it->second;
    return def;
}

std::string TNetworkDevice::GetConfig(const TStringMap &cfg, std::string def) const {
    for (auto &it: cfg) {
        if (StringMatch(Name, it.first))
            return it.second;
    }
    auto it = cfg.find("default");
    if (it != cfg.end())
        return it->second;
    return def;
}

void TNetwork::AddNetwork(ino_t inode, std::shared_ptr<TNetwork> &net) {
    auto lock = LockNetworks();
    Networks[inode] = net;

    for (auto it = Networks.begin(); it != Networks.end(); ) {
        if (it->second.expired())
            it = Networks.erase(it);
        else
            it++;
    }
}

std::shared_ptr<TNetwork> TNetwork::GetNetwork(ino_t inode) {
    auto lock = LockNetworks();
    auto it = Networks.find(inode);
    if (it != Networks.end())
        return it->second.lock();
    return nullptr;
}

void TNetwork::RefreshNetworks() {
    auto lock = LockNetworks();
    for (auto &it: Networks) {
        auto net = it.second.lock();
        if (net)
            net->RefreshClasses(false);
    }
}

void TNetwork::InitializeConfig() {
    std::ifstream groupCfg("/etc/iproute2/group");
    int id;
    std::string name;
    std::map<std::string, int> groupMap;

    while (groupCfg >> std::ws) {
        if (groupCfg.peek() != '#' && (groupCfg >> id >> name)) {
            L_SYS() << "Network device group: " << id << ":" << name << std::endl;
            groupMap[name] = id;
        }
        groupCfg.ignore(1 << 16, '\n');
    }

    UnmanagedDevices.clear();
    UnmanagedGroups.clear();

    for (auto device: config().network().unmanaged_device()) {
        L_SYS() << "Unmanaged network device: " << device << std::endl;
        UnmanagedDevices.push_back(device);
    }

    for (auto group: config().network().unmanaged_group()) {
        int id;

        if (groupMap.count(group)) {
            id = groupMap[group];
        } else if (StringToInt(group, id)) {
            L_SYS() << "Unknown network device group: " << group << std::endl;
            continue;
        }

        L_SYS() << "Unmanaged network device group: " << id << ":" << group << std::endl;
        UnmanagedGroups.push_back(id);
    }

    if (config().network().has_device_qdisc())
        StringToStringMap(config().network().device_qdisc(), DeviceQdisc);
    if (config().network().has_device_rate())
        StringToUintMap(config().network().device_rate(), DeviceRate);
    if (config().network().has_device_ceil())
        StringToUintMap(config().network().device_rate(), DeviceCeil);
    if (config().network().has_default_rate())
        StringToUintMap(config().network().default_rate(), DefaultRate);
    if (config().network().has_porto_rate())
        StringToUintMap(config().network().porto_rate(), PortoRate);
    if (config().network().has_container_rate())
        StringToUintMap(config().network().container_rate(), ContainerRate);
    if (config().network().has_device_quantum())
        StringToUintMap(config().network().device_quantum(), DeviceQuantum);
    if (config().network().has_device_rate_burst())
        StringToUintMap(config().network().device_rate_burst(), DeviceRateBurst);
    if (config().network().has_device_ceil_burst())
        StringToUintMap(config().network().device_ceil_burst(), DeviceCeilBurst);

    if (config().network().has_default_qdisc())
        StringToStringMap(config().network().default_qdisc(), DefaultQdisc);
    if (config().network().has_default_qdisc_limit())
        StringToUintMap(config().network().default_qdisc_limit(), DefaultQdiscLimit);
    if (config().network().has_default_qdisc_quantum())
        StringToUintMap(config().network().default_qdisc_quantum(), DefaultQdiscQuantum);

    if (config().network().has_container_qdisc())
        StringToStringMap(config().network().container_qdisc(), ContainerQdisc);
    if (config().network().has_container_qdisc_limit())
        StringToUintMap(config().network().container_qdisc_limit(), ContainerQdiscLimit);
}

TError TNetwork::Destroy() {
    auto lock = ScopedLock();
    TError error;

    L_ACT() << "Removing network..." << std::endl;

    for (auto &dev: Devices) {
        if (!dev.Managed)
            continue;

        TNlQdisc qdisc(dev.Index, TC_H_ROOT, TC_HANDLE(ROOT_TC_MAJOR, ROOT_TC_MINOR));
        error = qdisc.Delete(*Nl);
        if (error)
            L_ERR() << "Cannot remove htb: " << error << std::endl;
    }

    return TError::Success();
}

void TNetwork::GetDeviceSpeed(TNetworkDevice &dev) const {
    TPath knob("/sys/class/net/" + dev.Name + "/speed");
    uint64_t speed, rate, ceil;
    std::string text;

    if (ManagedNamespace) {
        dev.Ceil = NET_MAX_RATE;
        dev.Rate = NET_MAX_RATE;
        return;
    }

    if (knob.ReadAll(text) || StringToUint64(text, speed) || speed < 100) {
        ceil = NET_MAX_RATE;
        rate = NET_MAX_RATE;
    } else {
        ceil = speed * 125000; /* Mbit -> Bps */
        rate = speed * 112500; /* 90% */
    }

    dev.Ceil = dev.GetConfig(DeviceCeil, ceil);
    dev.Rate = dev.GetConfig(DeviceRate, rate);
}

TError TNetwork::SetupQueue(TNetworkDevice &dev) {
    TError error;

    //
    // 1:0 qdisc
    //  |
    // 1:1 / class
    //  |
    //  +- 1:2 default class
    //  |
    //  +- 1:3 /porto class
    //      |
    //      +- 1:4 container a
    //      |   |
    //      |   +- 1:5 container a/b
    //      |
    //      +- 1:6 container b
    //

    L() << "Setup queue for network device " << dev.GetDesc() << std::endl;

    TNlQdisc qdisc(dev.Index, TC_H_ROOT, TC_HANDLE(ROOT_TC_MAJOR, ROOT_TC_MINOR));
    qdisc.Kind = dev.GetConfig(DeviceQdisc);
    qdisc.Default = TC_HANDLE(ROOT_TC_MAJOR, DEFAULT_TC_MINOR);
    qdisc.Quantum = 10;

    if (!qdisc.Check(*Nl)) {
        (void)qdisc.Delete(*Nl);
        error = qdisc.Create(*Nl);
        if (error) {
            L_ERR() << "Cannot create root qdisc: " << error << std::endl;
            return error;
        }
    }

    TNlCgFilter filter(dev.Index, TC_HANDLE(ROOT_TC_MAJOR, ROOT_TC_MINOR), 1);
    (void)filter.Delete(*Nl);

    error = filter.Create(*Nl);
    if (error) {
        L_ERR() << "Can't create tc filter: " << error << std::endl;
        return error;
    }

    GetDeviceSpeed(dev);

    TNlClass cls;

    cls.Kind = dev.GetConfig(DeviceQdisc);
    cls.Index = dev.Index;
    cls.Parent = TC_HANDLE(ROOT_TC_MAJOR, ROOT_TC_MINOR);
    cls.Handle = TC_HANDLE(ROOT_TC_MAJOR, ROOT_CONTAINER_ID);
    cls.Prio = NET_DEFAULT_PRIO;
    cls.Rate = dev.Ceil;
    cls.Ceil = dev.Ceil;

    error = cls.Create(*Nl);
    if (error) {
        L_ERR() << "Can't create root tclass: " << error << std::endl;
        return error;
    }

    cls.Parent = TC_HANDLE(ROOT_TC_MAJOR, ROOT_CONTAINER_ID);
    cls.Handle = TC_HANDLE(ROOT_TC_MAJOR, DEFAULT_TC_MINOR);
    cls.Rate = dev.GetConfig(DefaultRate);

    error = cls.Create(*Nl);
    if (error) {
        L_ERR() << "Can't create default tclass: " << error << std::endl;
        return error;
    }

    if (!ManagedNamespace) {
        TNlQdisc defq(dev.Index, TC_HANDLE(ROOT_TC_MAJOR, DEFAULT_TC_MINOR),
                      TC_HANDLE(DEFAULT_TC_MAJOR, ROOT_TC_MINOR));
        defq.Kind = dev.GetConfig(DefaultQdisc);
        defq.Limit = dev.GetConfig(DefaultQdiscLimit);
        defq.Quantum = dev.GetConfig(DefaultQdiscQuantum, dev.MTU * 2);
        if (!defq.Check(*Nl)) {
            error = defq.Create(*Nl);
            if (error)
                return error;
        }
    }

    cls.Parent = TC_HANDLE(ROOT_TC_MAJOR, ROOT_CONTAINER_ID);
    cls.Handle = TC_HANDLE(ROOT_TC_MAJOR, PORTO_ROOT_CONTAINER_ID);
    cls.Rate = dev.GetConfig(PortoRate);

    error = cls.Create(*Nl);
    if (error) {
        L_ERR() << "Can't create porto tclass: " << error << std::endl;
        return error;
    }

    dev.Prepared = true;

    return TError::Success();
}

TNetwork::TNetwork() : NatBitmap(0, 0) {
    Nl = std::make_shared<TNl>();
    PORTO_ASSERT(Nl != nullptr);
}

TNetwork::~TNetwork() {
}

TError TNetwork::Connect() {
    return Nl->Connect();
}

TError TNetwork::ConnectNetns(TNamespaceFd &netns) {
    TNamespaceFd my_netns;
    TError error;

    error = my_netns.Open(GetTid(), "ns/net");
    if (error)
        return error;

    error = netns.SetNs(CLONE_NEWNET);
    if (error)
        return error;

    error = Connect();

    TError error2 = my_netns.SetNs(CLONE_NEWNET);
    PORTO_ASSERT(!error2);

    return error;
}

TError TNetwork::ConnectNew(TNamespaceFd &netns) {
    TNamespaceFd my_netns;
    TError error;

    error = my_netns.Open(GetTid(), "ns/net");
    if (error)
        return error;

    if (unshare(CLONE_NEWNET))
        return TError(EError::Unknown, errno, "unshare(CLONE_NEWNET)");

    error = netns.Open(GetTid(), "ns/net");
    if (!error) {
        error = Connect();
        if (error)
            netns.Close();
    }

    if (!error)
        error = SetSysctl("net.ipv6.conf.all.accept_dad", "0");
    if (!error)
        error = SetSysctl("net.ipv6.conf.default.accept_dad", "0");

    TError error2 = my_netns.SetNs(CLONE_NEWNET);
    PORTO_ASSERT(!error2);

    return error;
}

TError TNetwork::RefreshDevices() {
    struct nl_cache *cache;
    TError error;
    int ret;

    ret = rtnl_link_alloc_cache(GetSock(), AF_UNSPEC, &cache);
    if (ret < 0)
        return Nl->Error(ret, "Cannot allocate link cache");

    for (auto &dev: Devices)
        dev.Missing = true;

    for (auto obj = nl_cache_get_first(cache); obj; obj = nl_cache_get_next(obj)) {
        auto link = (struct rtnl_link *)obj;
        int flags = rtnl_link_get_flags(link);

        if (flags & IFF_LOOPBACK)
            continue;

        /* Do not setup queue on down links in host namespace */
        if (!ManagedNamespace && !(flags & IFF_RUNNING))
            continue;

        TNetworkDevice dev(link);

        /* Ignore our veth pairs */
        if (dev.Type == "veth" &&
            (StringStartsWith(dev.Name, "portove-") ||
             StringStartsWith(dev.Name, "L3-")))
            continue;

        /* In managed namespace we control all devices */
        if (ManagedNamespace)
            dev.Managed = true;

        bool found = false;
        for (auto &d: Devices) {
            if (d.Name != dev.Name || d.Index != dev.Index)
                continue;
            d = dev;
            if (d.Managed && std::string(rtnl_link_get_qdisc(link) ?: "") !=
                    dev.GetConfig(DeviceQdisc))
                Nl->Dump("Detected missing qdisc", link);
            else
                d.Prepared = true;
            found = true;
            break;
        }
        if (!found) {
            Nl->Dump("New network device", link);
            if (!dev.Managed)
                L() << "Unmanaged device " << dev.GetDesc() << std::endl;
            Devices.push_back(dev);
        }
    }

    nl_cache_free(cache);

    for (auto dev = Devices.begin(); dev != Devices.end(); ) {
        if (dev->Missing) {
            L() << "Delete network device " << dev->GetDesc() << std::endl;
            dev = Devices.erase(dev);
        } else
            dev++;
    }

    for (auto &dev: Devices) {
        if (!dev.Managed || dev.Prepared)
            continue;
        error = SetupQueue(dev);
        if (error)
            return error;
        NewManagedDevices = true;
    }

    return TError::Success();
}

TError TNetwork::RefreshClasses(bool force) {
    auto netLock = ScopedLock();
    TError error = RefreshDevices();
    if (error || (!force && !NewManagedDevices))
        return error;
    NewManagedDevices = false;
    netLock.unlock();

    auto ctLock = LockContainers();
    for (auto &it: Containers) {
        auto ct = it.second.get();
        if (ct->Net.get() == this &&
            (ct->GetState() == EContainerState::Running ||
             ct->GetState() == EContainerState::Meta)) {
            error = ct->UpdateTrafficClasses();
            if (error)
                L_ERR() << "Cannot refresh tc for " << ct->GetName()
                        << " : " << error << std::endl;
        }
    }
    L() << "done" << std::endl;

    return TError::Success();
}

TError TNetwork::GetGateAddress(std::vector<TNlAddr> addrs,
                                TNlAddr &gate4, TNlAddr &gate6, int &mtu) {
    struct nl_cache *cache, *lcache;
    int ret;

    ret = rtnl_addr_alloc_cache(GetSock(), &cache);
    if (ret < 0)
        return Nl->Error(ret, "Cannot allocate addr cache");

    ret = rtnl_link_alloc_cache(GetSock(), AF_UNSPEC, &lcache);
    if (ret < 0) {
        nl_cache_free(cache);
        return Nl->Error(ret, "Cannot allocate link cache");
    }

    for (auto obj = nl_cache_get_first(cache); obj; obj = nl_cache_get_next(obj)) {
         auto addr = (struct rtnl_addr *)obj;
         auto local = rtnl_addr_get_local(addr);

         if (!local || rtnl_addr_get_scope(addr) == RT_SCOPE_HOST)
             continue;

         for (auto &a: addrs) {

             if (nl_addr_get_family(a.Addr) == nl_addr_get_family(local)) {

                 /* get any gate of required family */
                 if (nl_addr_get_family(local) == AF_INET && !gate4.Addr)
                     gate4 = TNlAddr(local);

                 if (nl_addr_get_family(local) == AF_INET6 && !gate6.Addr)
                     gate6 = TNlAddr(local);
             }

             if (nl_addr_cmp_prefix(local, a.Addr) == 0) {

                 /* choose best matching gate address */
                 if (nl_addr_get_family(local) == AF_INET &&
                         nl_addr_cmp_prefix(gate4.Addr, a.Addr) != 0)
                     gate4 = TNlAddr(local);

                 if (nl_addr_get_family(local) == AF_INET6 &&
                         nl_addr_cmp_prefix(gate6.Addr, a.Addr) != 0)
                     gate6 = TNlAddr(local);

                 auto link = rtnl_link_get(lcache, rtnl_addr_get_ifindex(addr));
                 if (link) {
                     int link_mtu = rtnl_link_get_mtu(link);

                     if (mtu < 0 || link_mtu < mtu)
                         mtu = link_mtu;

                     rtnl_link_put(link);
                 }
             }
         }
    }

    nl_cache_free(lcache);
    nl_cache_free(cache);

    if (gate4.Addr)
        nl_addr_set_prefixlen(gate4.Addr, 32);

    if (gate6.Addr)
        nl_addr_set_prefixlen(gate6.Addr, 128);

    return TError::Success();
}

TError TNetwork::AddAnnounce(const TNlAddr &addr, std::string master) {
    struct nl_cache *cache;
    TError error;
    int ret;

    if (master != "") {
        int index = DeviceIndex(master);
        if (index)
            return Nl->ProxyNeighbour(index, addr, true);
        return TError(EError::InvalidValue, "Master link not found: " + master);
    }

    ret = rtnl_addr_alloc_cache(GetSock(), &cache);
    if (ret < 0)
        return Nl->Error(ret, "Cannot allocate addr cache");

    for (auto &dev : Devices) {
        bool reachable = false;

        for (auto obj = nl_cache_get_first(cache); obj;
                obj = nl_cache_get_next(obj)) {
            auto raddr = (struct rtnl_addr *)obj;
            auto local = rtnl_addr_get_local(raddr);

            if (rtnl_addr_get_ifindex(raddr) == dev.Index &&
                    local && nl_addr_cmp_prefix(local, addr.Addr) == 0) {
                reachable = true;
                break;
            }
        }

        /* Add proxy entry only if address is directly reachable */
        if (reachable) {
            error = Nl->ProxyNeighbour(dev.Index, addr, true);
            if (error)
                break;
        }
    }

    nl_cache_free(cache);

    return error;
}

TError TNetwork::DelAnnounce(const TNlAddr &addr) {
    TError error;

    for (auto &dev: Devices)
        error = Nl->ProxyNeighbour(dev.Index, addr, false);

    return error;
}

TError TNetwork::GetNatAddress(std::vector<TNlAddr> &addrs) {
    TError error;
    int offset;

    error = NatBitmap.Get(offset);
    if (error)
        return TError(error, "Cannot allocate NAT address");

    if (!NatBaseV4.IsEmpty()) {
        TNlAddr addr = NatBaseV4;
        addr.AddOffset(offset);
        addrs.push_back(addr);
    }

    if (!NatBaseV6.IsEmpty()) {
        TNlAddr addr = NatBaseV6;
        addr.AddOffset(offset);
        addrs.push_back(addr);
    }

    return TError::Success();
}

TError TNetwork::PutNatAddress(const std::vector<TNlAddr> &addrs) {

    for (auto &addr: addrs) {
        if (addr.Family() == AF_INET && !NatBaseV4.IsEmpty()) {
            uint64_t offset =  addr.GetOffset(NatBaseV4);
            return NatBitmap.Put(offset);
        }
        if (addr.Family() == AF_INET6 && !NatBaseV6.IsEmpty()) {
            uint64_t offset =  addr.GetOffset(NatBaseV6);
            return NatBitmap.Put(offset);
        }
    }

    return TError::Success();
}

std::string TNetwork::NewDeviceName(const std::string &prefix) {
    for (int retry = 0; retry < 100; retry++) {
        std::string name = prefix + std::to_string(IfaceName++);
        TNlLink link(Nl, name);
        if (link.Load())
            return name;
    }
    return prefix + "0";
}

std::string TNetwork::MatchDevice(const std::string &pattern) {
    for (auto &dev: Devices) {
        if (StringMatch(dev.Name, pattern))
            return dev.Name;
    }
    return pattern;
}

TError TNetwork::GetDeviceStat(ENetStat kind, TUintMap &stat) {
    struct nl_cache *cache;
    rtnl_link_stat_id_t id;

    switch (kind) {
        case ENetStat::RxBytes:
            id = RTNL_LINK_RX_BYTES;
            break;
        case ENetStat::RxPackets:
            id = RTNL_LINK_RX_PACKETS;
            break;
        case ENetStat::RxDrops:
            id = RTNL_LINK_RX_DROPPED;
            break;
        case ENetStat::TxBytes:
            id = RTNL_LINK_TX_BYTES;
            break;
        case ENetStat::TxPackets:
            id = RTNL_LINK_TX_PACKETS;
            break;
        case ENetStat::TxDrops:
            id = RTNL_LINK_TX_DROPPED;
            break;
        default:
            return TError(EError::Unknown, "Unsupported netlink statistics");
    }

    int ret = rtnl_link_alloc_cache(GetSock(), AF_UNSPEC, &cache);
    if (ret < 0)
        return Nl->Error(ret, "Cannot allocate link cache");

    for (auto &dev: Devices) {
        auto link = rtnl_link_get(cache, dev.Index);
        if (link)
            stat[dev.Name] = rtnl_link_get_stat(link, id);
        else
            L_WRN() << "Cannot find device " << dev.GetDesc() << std::endl;
        rtnl_link_put(link);
    }

    nl_cache_free(cache);
    return TError::Success();
}

TError TNetwork::GetTrafficStat(uint32_t handle, ENetStat kind, TUintMap &stat) {
    rtnl_tc_stat rtnlStat;

    switch (kind) {
    case ENetStat::Packets:
        rtnlStat = RTNL_TC_PACKETS;
        break;
    case ENetStat::Bytes:
        rtnlStat = RTNL_TC_BYTES;
        break;
    case ENetStat::Drops:
        rtnlStat = RTNL_TC_DROPS;
        break;
    case ENetStat::Overlimits:
        rtnlStat = RTNL_TC_OVERLIMITS;
        break;
    default:
        return GetDeviceStat(kind, stat);
    }

    for (auto &dev: Devices) {
        struct nl_cache *cache;
        struct rtnl_class *cls;

        if (!dev.Managed || !dev.Prepared)
            continue;

        /* TODO optimize this stuff */
        int ret = rtnl_class_alloc_cache(GetSock(), dev.Index, &cache);
        if (ret < 0)
            return Nl->Error(ret, "Cannot allocate class cache");

        cls = rtnl_class_get(cache, dev.Index, handle);
        if (cls) {
            stat[dev.Name] = rtnl_tc_get_stat(TC_CAST(cls), rtnlStat);
            rtnl_class_put(cls);
        } else
            L_WRN() << "Cannot find tc class " << handle << " at " << dev.GetDesc() << std::endl;

        /* HFSC statistics isn't hierarchical */
        if (!strcmp(rtnl_tc_get_kind(TC_CAST(cls)), "hfsc")) {
            std::vector<uint32_t> handles({handle});
            for (int i = 0; i < (int)handles.size(); i++) {
                for (auto obj = nl_cache_get_first(cache); obj;
                          obj = nl_cache_get_next(obj)) {
                    if (rtnl_tc_get_parent(TC_CAST(obj)) == handles[i]) {
                        stat[dev.Name] += rtnl_tc_get_stat(TC_CAST(obj), rtnlStat);
                        handles.push_back(rtnl_tc_get_handle(TC_CAST(obj)));
                    }
                }
            }
        }

        nl_cache_free(cache);
    }

    return TError::Success();
}

TError TNetwork::CreateTC(uint32_t handle, uint32_t parent, bool leaf,
                          TUintMap &prio, TUintMap &rate, TUintMap &ceil) {
    TError error, result;
    TNlClass cls;

    cls.Parent = parent;
    cls.Handle = handle;

    for (auto &dev: Devices) {
        if (!dev.Managed)
            continue;

        uint64_t defRate;
        if (handle == TC_HANDLE(ROOT_TC_MAJOR, ROOT_CONTAINER_ID))
            defRate = dev.Rate;
        else if (handle == TC_HANDLE(ROOT_TC_MAJOR, PORTO_ROOT_CONTAINER_ID))
            defRate = dev.GetConfig(PortoRate);
        else
            defRate = dev.GetConfig(ContainerRate);

        cls.Index = dev.Index;
        cls.Kind = dev.GetConfig(DeviceQdisc);
        cls.Prio = dev.GetConfig(prio);
        cls.Rate = dev.GetConfig(rate, defRate);
        cls.Ceil = dev.GetConfig(ceil);
        cls.Quantum = dev.GetConfig(DeviceQuantum, dev.MTU * 2);
        cls.RateBurst = dev.GetConfig(DeviceRateBurst, dev.MTU * 10);
        cls.CeilBurst = dev.GetConfig(DeviceCeilBurst, dev.MTU * 10);

        error = cls.Create(*Nl);
        if (error) {
            L_WRN() << "Cannot add tc class: " << error << std::endl;
            if (!result)
                result = error;
        }

        if (!error && leaf) {
            TNlQdisc ctq(dev.Index, handle,
                         TC_HANDLE(TC_H_MIN(handle), CONTAINER_TC_MINOR));
            ctq.Kind = dev.GetConfig(ContainerQdisc);
            ctq.Limit = dev.GetConfig(ContainerQdiscLimit, dev.MTU * 20);
            error = ctq.Create(*Nl);
            if (error) {
                L_WRN() << "Cannot add container tc qdisc: " << error << std::endl;
                if (!result)
                    result = error;
            }
        }
    }

    return result;
}

TError TNetwork::DestroyTC(uint32_t handle) {
    TError error, result;

    for (auto &dev: Devices) {
        if (!dev.Managed)
            continue;

        TNlQdisc ctq(dev.Index, handle,
                     TC_HANDLE(TC_H_MIN(handle), CONTAINER_TC_MINOR));
        (void)ctq.Delete(*Nl);

        TNlClass cls(dev.Index, TC_H_UNSPEC, handle);
        error = cls.Delete(*Nl);
        if (error) {
            L_WRN() << "Cannot del tc class: " << error << std::endl;
            if (!result)
                result = error;
        }
    }

    return result;
}

void TNetCfg::Reset() {
    /* default - create new empty netns */
    NewNetNs = true;
    Inherited = false;
    Steal.clear();
    MacVlan.clear();
    IpVlan.clear();
    Veth.clear();
    L3lan.clear();
    NetNsName = "";
    NetCtName = "";
}

TError TNetCfg::ParseNet(std::vector<std::string> lines) {
    bool none = false;
    int idx = 0;

    Reset();

    if (lines.size() == 0)
        return TError(EError::InvalidValue, "Configuration is not specified");

    for (auto &line : lines) {
        std::vector<std::string> settings;
        TError error;

        SplitEscapedString(line, settings, ' ');
        if (settings.size() == 0)
            return TError(EError::InvalidValue, "Invalid net in: " + line);

        std::string type = StringTrim(settings[0]);

        if (type == "host" && settings.size() == 1)
            type = "inherited";

        if (type == "none") {
            none = true;
        } else if (type == "inherited") {
            NewNetNs = false;
            Inherited = true;
        } else if (type == "steal" || type == "host" /* legacy */) {
            if (settings.size() != 2)
                return TError(EError::InvalidValue, "Invalid net in: " + line);
            Steal.push_back(StringTrim(settings[1]));
        } else if (type == "container") {
            if (settings.size() != 2)
                return TError(EError::InvalidValue, "Invalid net in: " + line);
            NewNetNs = false;
            NetCtName = StringTrim(settings[1]);
        } else if (type == "macvlan") {
            if (settings.size() < 3)
                return TError(EError::InvalidValue, "Invalid macvlan in: " + line);

            std::string master = StringTrim(settings[1]);
            std::string name = StringTrim(settings[2]);
            std::string type = "bridge";
            std::string hw = "";
            int mtu = -1;

            if (settings.size() > 3) {
                type = StringTrim(settings[3]);
                if (!TNlLink::ValidMacVlanType(type))
                    return TError(EError::InvalidValue,
                            "Invalid macvlan type " + type);
            }

            if (settings.size() > 4) {
                TError error = StringToInt(settings[4], mtu);
                if (error)
                    return TError(EError::InvalidValue,
                            "Invalid macvlan mtu " + settings[4]);
            }

            if (settings.size() > 5) {
                hw = StringTrim(settings[5]);
                if (!TNlLink::ValidMacAddr(hw))
                    return TError(EError::InvalidValue,
                            "Invalid macvlan address " + hw);
            }

            TMacVlanNetCfg mvlan;
            mvlan.Master = master;
            mvlan.Name = name;
            mvlan.Type = type;
            mvlan.Hw = hw;
            mvlan.Mtu = mtu;

            MacVlan.push_back(mvlan);
        } else if (type == "ipvlan") {
            if (settings.size() < 3)
                return TError(EError::InvalidValue, "Invalid ipvlan in: " + line);

            std::string master = StringTrim(settings[1]);
            std::string name = StringTrim(settings[2]);
            std::string mode = "l2";
            int mtu = -1;

            if (settings.size() > 3) {
                mode = StringTrim(settings[3]);
                if (!TNlLink::ValidIpVlanMode(mode))
                    return TError(EError::InvalidValue,
                            "Invalid ipvlan mode " + mode);
            }

            if (settings.size() > 4) {
                TError error = StringToInt(settings[4], mtu);
                if (error)
                    return TError(EError::InvalidValue,
                            "Invalid ipvlan mtu " + settings[4]);
            }

            TIpVlanNetCfg ipvlan;
            ipvlan.Master = master;
            ipvlan.Name = name;
            ipvlan.Mode = mode;
            ipvlan.Mtu = mtu;

            IpVlan.push_back(ipvlan);
        } else if (type == "veth") {
            if (settings.size() < 3)
                return TError(EError::InvalidValue, "Invalid veth in: " + line);
            std::string name = StringTrim(settings[1]);
            std::string bridge = StringTrim(settings[2]);
            std::string hw = "";
            int mtu = -1;

            if (settings.size() > 3) {
                TError error = StringToInt(settings[3], mtu);
                if (error)
                    return TError(EError::InvalidValue,
                            "Invalid veth mtu " + settings[3]);
            }

            if (settings.size() > 4) {
                hw = StringTrim(settings[4]);
                if (!TNlLink::ValidMacAddr(hw))
                    return TError(EError::InvalidValue,
                            "Invalid veth address " + hw);
            }

            TVethNetCfg veth;
            veth.Bridge = bridge;
            veth.Name = name;
            veth.Hw = hw;
            veth.Mtu = mtu;
            veth.Peer = "portove-" + std::to_string(Id) + "-" + std::to_string(idx++);

            Veth.push_back(veth);

        } else if (type == "L3") {
            TL3NetCfg l3;

            l3.Name = "eth0";
            l3.Nat = false;
            if (settings.size() > 1)
                l3.Name = StringTrim(settings[1]);

            l3.Mtu = -1;
            if (settings.size() > 2)
                l3.Master = StringTrim(settings[2]);

            L3lan.push_back(l3);

        } else if (type == "NAT") {
            TL3NetCfg nat;

            nat.Nat = true;
            nat.Name = "eth0";
            nat.Mtu = -1;

            if (settings.size() > 1)
                nat.Name = StringTrim(settings[1]);

            L3lan.push_back(nat);

        } else if (type == "MTU") {
            if (settings.size() != 3)
                return TError(EError::InvalidValue, "Invalid MTU in: " + line);

            int mtu;
            TError error = StringToInt(settings[2], mtu);
            if (error)
                return error;

            for (auto &link: L3lan) {
                if (link.Name == settings[1]) {
                    link.Mtu = mtu;
                    return TError::Success();
                }
            }

            for (auto &link: Veth) {
                if (link.Name == settings[1]) {
                    link.Mtu = mtu;
                    return TError::Success();
                }
            }

            for (auto &link: MacVlan) {
                if (link.Name == settings[1]) {
                    link.Mtu = mtu;
                    return TError::Success();
                }
            }

            for (auto &link: IpVlan) {
                if (link.Name == settings[1]) {
                    link.Mtu = mtu;
                    return TError::Success();
                }
            }

            return TError(EError::InvalidValue, "Link not found: " + settings[1]);

        } else if (type == "autoconf") {
            if (settings.size() != 2)
                return TError(EError::InvalidValue, "Invalid autoconf in: " + line);
            Autoconf.push_back(StringTrim(settings[1]));
        } else if (type == "netns") {
            if (settings.size() != 2)
                return TError(EError::InvalidValue, "Invalid netns in: " + line);
            std::string name = StringTrim(settings[1]);
            TPath path("/var/run/netns/" + name);
            if (!path.Exists())
                return TError(EError::InvalidValue, "net namespace not found: " + name);
            NewNetNs = false;
            NetNsName = name;
        } else {
            return TError(EError::InvalidValue, "Configuration is not specified");
        }
    }

    int single = none + Inherited;
    int mixed = Steal.size() + MacVlan.size() + IpVlan.size() + Veth.size() + L3lan.size();

    if (single > 1 || (single == 1 && mixed))
        return TError(EError::InvalidValue, "none/host/inherited can't be mixed with other types");

    return TError::Success();
}

TError TNetCfg::ParseIp(std::vector<std::string> lines) {
    IpVec.clear();
    for (auto &line : lines) {
        std::vector<std::string> settings;
        TError error;

        SplitEscapedString(line, settings, ' ');
        if (settings.size() != 2)
            return TError(EError::InvalidValue, "Invalid ip address/prefix in: " + line);

        TIpVec ip;
        ip.Iface = settings[0];
        error = ip.Addr.Parse(AF_UNSPEC, settings[1]);
        if (error)
            return error;
        IpVec.push_back(ip);

        for (auto &l3: L3lan) {
            if (l3.Name == ip.Iface) {
                if (!ip.Addr.IsHost())
                    return TError(EError::InvalidValue, "Invalid ip prefix for L3 network");
                l3.Addrs.push_back(ip.Addr);
            }
        }
    }
    return TError::Success();
}

TError TNetCfg::FormatIp(std::vector<std::string> &lines) {
    for (auto &ip: IpVec)
        lines.push_back(ip.Iface + " " + ip.Addr.Format());
    return TError::Success();
}

TError TNetCfg::ParseGw(std::vector<std::string> lines) {
    GwVec.clear();
    for (auto &line : lines) {
        std::vector<std::string> settings;
        TError error;

        SplitEscapedString(line, settings, ' ');
        if (settings.size() != 2)
            return TError(EError::InvalidValue, "Invalid gateway address/prefix in: " + line);

        TGwVec gw;
        gw.Iface = settings[0];
        error = gw.Addr.Parse(AF_UNSPEC, settings[1]);
        if (error)
            return error;
        GwVec.push_back(gw);
    }
    return TError::Success();
}

std::string TNetCfg::GenerateHw(const std::string &name) {
    uint32_t n = Crc32(name);
    uint32_t h = Crc32(Hostname);

    return StringFormat("02:%02x:%02x:%02x:%02x:%02x",
            (n & 0x000000FF) >> 0,
            (h & 0xFF000000) >> 24,
            (h & 0x00FF0000) >> 16,
            (h & 0x0000FF00) >> 8,
            (h & 0x000000FF) >> 0);
}

TError TNetCfg::ConfigureVeth(TVethNetCfg &veth) {
    auto parentNl = ParentNet->GetNl();
    TNlLink peer(parentNl, ParentNet->NewDeviceName("portove-"));
    TError error;

    std::string hw = veth.Hw;
    if (hw.empty() && !Hostname.empty())
        hw = GenerateHw(veth.Name + veth.Peer);

    error = peer.AddVeth(veth.Name, hw, veth.Mtu, NetNs.GetFd());
    if (error)
        return error;

    if (!veth.Bridge.empty()) {
        TNlLink bridge(parentNl, veth.Bridge);
        error = bridge.Load();
        if (error)
            return error;

        error = bridge.Enslave(peer.GetName());
        if (error)
            return error;
    }

    return TError::Success();
}

TError TNetCfg::ConfigureL3(TL3NetCfg &l3) {
    auto lock = HostNetwork->ScopedLock();
    std::string peerName = HostNetwork->NewDeviceName("L3-");
    auto parentNl = HostNetwork->GetNl();
    TNlLink peer(parentNl, peerName);
    TNlAddr gate4, gate6;
    TError error;

    if (l3.Nat && l3.Addrs.empty()) {
        error = HostNetwork->GetNatAddress(l3.Addrs);
        if (error)
            return error;

        for (auto &addr: l3.Addrs) {
            TIpVec ip;
            ip.Iface = l3.Name;
            ip.Addr = addr;
            IpVec.push_back(ip);
        }

        SaveIp = true;
    }

    error = HostNetwork->GetGateAddress(l3.Addrs, gate4, gate6, l3.Mtu);
    if (error)
        return error;

    for (auto &addr : l3.Addrs) {
        if (addr.Family() == AF_INET && gate4.IsEmpty())
            return TError(EError::InvalidValue, "Ipv4 gateway not found");
        if (addr.Family() == AF_INET6 && gate6.IsEmpty())
            return TError(EError::InvalidValue, "Ipv6 gateway not found");
    }

    error = peer.AddVeth(l3.Name, "", l3.Mtu, NetNs.GetFd());
    if (error)
        return error;

    TNlLink link(Net->GetNl(), l3.Name);
    error = link.Load();
    if (error)
        return error;

    error = link.Up();
    if (error)
        return error;

    if (!gate4.IsEmpty()) {
        error = parentNl->ProxyNeighbour(peer.GetIndex(), gate4, true);
        if (error)
            return error;
        error = link.AddDirectRoute(gate4);
        if (error)
            return error;
        error = link.SetDefaultGw(gate4);
        if (error)
            return error;
    }

    if (!gate6.IsEmpty()) {
        error = parentNl->ProxyNeighbour(peer.GetIndex(), gate6, true);
        if (error)
            return error;
        error = link.AddDirectRoute(gate6);
        if (error)
            return error;
        error = link.SetDefaultGw(gate6);
        if (error)
            return error;
    }

    for (auto &addr : l3.Addrs) {
        error = peer.AddDirectRoute(addr);
        if (error)
            return error;

        error = HostNetwork->AddAnnounce(addr, HostNetwork->MatchDevice(l3.Master));
        if (error)
            return error;
    }

    return TError::Success();
}

TError TNetCfg::ConfigureInterfaces() {
    std::vector<std::string> links;
    auto parent_lock = ParentNet->ScopedLock();
    auto source_nl = ParentNet->GetNl();
    auto target_nl = Net->GetNl();
    TError error;

    for (auto &dev : Steal) {
        TNlLink link(source_nl, dev);
        error = link.ChangeNs(dev, NetNs.GetFd());
        if (error)
            return error;
        links.emplace_back(dev);
    }

    for (auto &ipvlan : IpVlan) {
        std::string master = ParentNet->MatchDevice(ipvlan.Master);

        TNlLink link(source_nl, "piv" + std::to_string(GetTid()));
        error = link.AddIpVlan(master, ipvlan.Mode, ipvlan.Mtu);
        if (error)
            return error;

        error = link.ChangeNs(ipvlan.Name, NetNs.GetFd());
        if (error) {
            (void)link.Remove();
            return error;
        }
        links.emplace_back(ipvlan.Name);
    }

    for (auto &mvlan : MacVlan) {
        std::string master = ParentNet->MatchDevice(mvlan.Master);

        std::string hw = mvlan.Hw;
        if (hw.empty() && !Hostname.empty())
            hw = GenerateHw(master + mvlan.Name);

        TNlLink link(source_nl, "pmv" + std::to_string(GetTid()));
        error = link.AddMacVlan(master, mvlan.Type, hw, mvlan.Mtu);
        if (error)
                return error;

        error = link.ChangeNs(mvlan.Name, NetNs.GetFd());
        if (error) {
            (void)link.Remove();
            return error;
        }
        links.emplace_back(mvlan.Name);
    }

    for (auto &veth : Veth) {
        error = ConfigureVeth(veth);
        if (error)
            return error;
        links.emplace_back(veth.Name);
    }

    parent_lock.unlock();

    for (auto &l3 : L3lan) {
        error = ConfigureL3(l3);
        if (error)
            return error;
        links.emplace_back(l3.Name);
    }

    TNlLink loopback(target_nl, "lo");
    error = loopback.Load();
    if (error)
        return error;
    error = loopback.Up();
    if (error)
        return error;

    Net->ManagedNamespace = true;

    error = Net->RefreshDevices();
    if (error)
        return error;

    Net->NewManagedDevices = false;

    for (auto &name: links) {
        if (!Net->DeviceIndex(name))
            return TError(EError::Unknown, "network device " + name + " not found");
    }

    for (auto &dev: Net->Devices) {
        if (!NetUp) {
            bool found = false;
            for (auto &ip: IpVec)
                if (ip.Iface == dev.Name)
                    found = true;
            for (auto &gw: GwVec)
                if (gw.Iface == dev.Name)
                    found = true;
            for (auto &ac: Autoconf)
                if (ac == dev.Name)
                    found = true;
            if (!found)
                continue;
        }

        TNlLink link(target_nl, dev.Name);
        error = link.Load();
        if (error)
            return error;
        error = link.Up();
        if (error)
            return error;

        for (auto &ip: IpVec) {
            if (ip.Iface == dev.Name) {
                error = link.AddAddress(ip.Addr);
                if (error)
                    return error;
            }
        }

        for (auto &gw: GwVec) {
            if (gw.Iface == dev.Name) {
                error = link.SetDefaultGw(gw.Addr);
                if (error)
                    return error;
            }
        }
    }

    return TError::Success();
}

TError TNetCfg::PrepareNetwork() {
    TError error;

    if (NewNetNs) {
        Net = std::make_shared<TNetwork>();
        error = Net->ConnectNew(NetNs);
        if (error)
            return error;

        error = ConfigureInterfaces();
        if (error) {
            (void)DestroyNetwork();
            return error;
        }

        TNetwork::AddNetwork(NetNs.GetInode(), Net);
    } else if (!Parent) {
        Net = std::make_shared<TNetwork>();
        error = Net->Connect();
        if (error)
            return error;

        error = NetNs.Open(GetTid(), "ns/net");
        if (error)
            return error;

        TNetwork::AddNetwork(NetNs.GetInode(), Net);

        error = Net->RefreshDevices();
        if (error)
            return error;
        Net->NewManagedDevices = false;

        if (config().network().has_nat_first_ipv4())
            Net->NatBaseV4.Parse(AF_INET, config().network().nat_first_ipv4());
        if (config().network().has_nat_first_ipv6())
            Net->NatBaseV6.Parse(AF_INET6, config().network().nat_first_ipv6());
        if (config().network().has_nat_count())
            Net->NatBitmap.Resize(config().network().nat_count());
        HostNetwork = Net;
    } else if (Inherited) {
        Net = Parent->Net;
        error = Parent->OpenNetns(NetNs);
        if (error)
            return error;
    } else if (NetNsName != "") {
        error = NetNs.Open("/var/run/netns/" + NetNsName);
        if (error)
            return error;

        Net = TNetwork::GetNetwork(NetNs.GetInode());
        if (!Net) {
            Net = std::make_shared<TNetwork>();

            error = Net->ConnectNetns(NetNs);
            if (error)
                return error;

            error = Net->RefreshDevices();
            if (error)
                return error;
            Net->NewManagedDevices = false;

            TNetwork::AddNetwork(NetNs.GetInode(), Net);
        }
    } else if (NetCtName != "") {
        std::shared_ptr<TContainer> target;
        error = TContainer::Find(NetCtName, target);
        if (error)
            return error;

        if (CurrentClient) {
            error = CurrentClient->CanControl(*target);
            if (error)
                return TError(error, "net container " + NetCtName);
        }

        error = target->OpenNetns(NetNs);
        if (error)
            return error;

        Net = target->Net;
    }

    return TError::Success();
}

TError TNetCfg::DestroyNetwork() {
    TError error;

    for (auto &l3 : L3lan) {
        auto lock = HostNetwork->ScopedLock();
        for (auto &addr : l3.Addrs) {
            error = HostNetwork->DelAnnounce(addr);
            if (error)
                L_ERR() << "Cannot remove announce " << addr.Format()
                        << " : " << error << std::endl;
        }
        if (l3.Nat) {
            error = HostNetwork->PutNatAddress(l3.Addrs);
            if (error)
                L_ERR() << "Cannot put NAT address : " << error << std::endl;

            auto ip = IpVec.begin();
            while (ip != IpVec.end()) {
                if (ip->Iface == l3.Name)
                    ip = IpVec.erase(ip);
                else
                    ++ip;
            }
            SaveIp = true;
        }
    }

    return error;
}
