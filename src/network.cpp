#include <algorithm>
#include <sstream>
#include <fstream>

#include "network.hpp"
#include "container.hpp"
#include "task.hpp"
#include "config.hpp"
#include "client.hpp"
#include "helpers.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/crc32.hpp"

extern "C" {
#include <linux/if.h>
#include <netinet/ip6.h>
#include <netinet/ether.h>
#include <linux/if_tun.h>
#include <linux/neighbour.h>
#include <netlink/cache.h>
#include <netlink/route/link.h>
#include <netlink/route/tc.h>
#include <netlink/route/addr.h>
#include <netlink/route/class.h>
#include <netlink/route/neighbour.h>
#include <netlink/route/qdisc/htb.h>
}

static std::shared_ptr<TNetwork> HostNetwork;

std::mutex TNetwork::NetworksMutex;
std::unordered_map<ino_t, std::shared_ptr<TNetwork>> TNetwork::NetworksIndex;
std::shared_ptr<const std::list<std::shared_ptr<TNetwork>>> TNetwork::NetworksList = std::make_shared<const std::list<std::shared_ptr<TNetwork>>>();
std::atomic<int> TNetwork::GlobalStatGen;

static std::thread NetThread;
static std::condition_variable NetThreadCv;
static uint64_t NetWatchdogPeriod;
static uint64_t NetProxyNeighbourPeriod;

static TTuple ResolvConfCurrent;
static TTuple ResolvConfPrev;
static uint64_t ResolvConfPeriod = 0;

static std::vector<std::string> UnmanagedDevices;
static std::vector<int> UnmanagedGroups;
static std::map<int, std::string> DeviceGroups;
static int VirtualDeviceGroup = 0;

static TStringMap DeviceQdisc;
static TUintMap DeviceRate;
static TUintMap DeviceCeil;
static TUintMap DeviceRateBurst;
static TUintMap DeviceCeilBurst;
static TUintMap DeviceQuantum;

static TUintMap DefaultClassRate;
static TUintMap DefaultClassCeil;

static TStringMap DefaultQdisc;
static TUintMap DefaultQdiscLimit;
static TUintMap DefaultQdiscQuantum;

static TUintMap ContainerRate;
static TStringMap ContainerQdisc;
static TUintMap ContainerQdiscLimit;
static TUintMap ContainerQdiscQuantum;

static TUintMap IngressBurst;

static std::list<std::pair<std::string, std::string>> NetSysctls = {
    { "net.core.somaxconn", "128" },

    { "net.unix.max_dgram_qlen", "10" },

    { "net.ipv4.icmp_echo_ignore_all", "0" },
    { "net.ipv4.icmp_echo_ignore_broadcasts", "1" },
    { "net.ipv4.icmp_ignore_bogus_error_responses", "1" },
    { "net.ipv4.icmp_errors_use_inbound_ifaddr", "0" },
    { "net.ipv4.icmp_ratelimit", "1000" },
    { "net.ipv4.icmp_ratemask", "6168" },
    { "net.ipv4.ping_group_range", "1\t0" },

    { "net.ipv4.tcp_ecn", "2" },
    { "net.ipv4.tcp_ecn_fallback", "1" },
    { "net.ipv4.ip_dynaddr", "0" },
    { "net.ipv4.ip_early_demux", "1" },
    { "net.ipv4.ip_default_ttl", "64" },

    { "net.ipv4.ip_local_port_range", "32768\t60999" },
    { "net.ipv4.ip_local_reserved_ports", "" },
    { "net.ipv4.ip_no_pmtu_disc", "0" },
    { "net.ipv4.ip_forward_use_pmtu", "0" },
    { "net.ipv4.ip_nonlocal_bind", "0" },
    //"net.ipv4.fwmark_reflect",
    //"net.ipv4.tcp_fwmark_accept",
    { "net.ipv4.tcp_mtu_probing", "0" },
    { "net.ipv4.tcp_base_mss", "1024" },
    { "net.ipv4.tcp_probe_threshold", "8" },
    { "net.ipv4.tcp_probe_interval", "600" },

    //"net.ipv4.igmp_link_local_mcast_reports",
    //"net.ipv4.igmp_max_memberships",
    //"net.ipv4.igmp_max_msf",
    //"net.ipv4.igmp_qrv",

    { "net.ipv4.tcp_keepalive_time", "7200" },
    { "net.ipv4.tcp_keepalive_probes", "9" },
    { "net.ipv4.tcp_keepalive_intvl", "75" },
    { "net.ipv4.tcp_syn_retries", "6" },
    { "net.ipv4.tcp_synack_retries", "5" },
    { "net.ipv4.tcp_syncookies", "1" },
    { "net.ipv4.tcp_reordering", "3" },
    { "net.ipv4.tcp_retries1", "3" },
    { "net.ipv4.tcp_retries2", "15" },
    { "net.ipv4.tcp_orphan_retries", "0" },
    { "net.ipv4.tcp_fin_timeout", "60" },
    { "net.ipv4.tcp_notsent_lowat", "-1" },
    { "net.ipv4.tcp_tw_reuse", "0" },

    { "net.ipv6.bindv6only", "0" },
    //"net.ipv6.anycast_src_echo_reply",
    //"net.ipv6.flowlabel_consistency",
    //"net.ipv6.auto_flowlabels",
    //"net.ipv6.fwmark_reflect",
    //"net.ipv6.idgen_retries",
    //"net.ipv6.idgen_delay",
    //"net.ipv6.flowlabel_state_ranges",
    { "net.ipv6.ip_nonlocal_bind", "0" },

    { "net.ipv6.icmp.ratelimit", "1000" },

    { "net.ipv6.route.gc_thresh", "1024" },
    { "net.ipv6.route.max_size", "4096" },
    { "net.ipv6.route.gc_min_interval", "0" },
    { "net.ipv6.route.gc_timeout", "60" },
    { "net.ipv6.route.gc_interval", "30" },
    { "net.ipv6.route.gc_elasticity", "9" },
    { "net.ipv6.route.mtu_expires", "600" },
    { "net.ipv6.route.min_adv_mss", "1220" },
    { "net.ipv6.route.gc_min_interval_ms", "500" },
};

bool TNetwork::NetworkSysctl(const std::string &key) {
    return StringStartsWith(key, "net.");
}

bool TNetwork::NamespaceSysctl(const std::string &key) {
    for (auto &pair: NetSysctls) {
        if (pair.first == key)
            return true;
    }
    if (StringStartsWith(key, "net.ipv4.conf."))
        return true;
    if (StringStartsWith(key, "net.ipv6.conf."))
        return true;
    if (StringStartsWith(key, "net.ipv4.neigh.") &&
            !StringStartsWith(key, "net.ipv4.neigh.default."))
        return true;
    if (StringStartsWith(key, "net.ipv6.neigh.") &&
            !StringStartsWith(key, "net.ipv6.neigh.default."))
        return true;
    return false;
}

TNetDevice::TNetDevice(struct rtnl_link *link) {
    Name = rtnl_link_get_name(link);
    Type = rtnl_link_get_type(link) ?: "";
    Index = rtnl_link_get_ifindex(link);
    Link = rtnl_link_get_link(link);
    Group = rtnl_link_get_group(link);
    MTU = rtnl_link_get_mtu(link);
    GroupName = TNetwork::DeviceGroupName(Group);

    Rate = NET_MAX_RATE;
    Ceil = NET_MAX_RATE;

    Managed = false;
    Prepared = false;
    Missing = false;
}

uint64_t TNetDevice::GetConfig(const TUintMap &cfg, uint64_t def) const {
    for (auto &it: cfg) {
        if (StringMatch(Name, it.first))
            return it.second;
    }
    auto it = cfg.find("group " + GroupName);
    if (it == cfg.end())
        it = cfg.find("default");
    if (it != cfg.end())
        return it->second;
    return def;
}

std::string TNetDevice::GetConfig(const TStringMap &cfg, std::string def) const {
    for (auto &it: cfg) {
        if (StringMatch(Name, it.first))
            return it.second;
    }
    auto it = cfg.find("group " + GroupName);
    if (it == cfg.end())
        it = cfg.find("default");
    if (it != cfg.end())
        return it->second;
    return def;
}

void TNetwork::InitializeConfig() {
    std::ifstream groupCfg("/etc/iproute2/group");
    int id;
    std::string name;
    std::map<std::string, int> groupMap;

    DeviceGroups[0] = "default";

    while (groupCfg >> std::ws) {
        if (groupCfg.peek() != '#' && (groupCfg >> id >> name)) {
            L_NET("Network device group: {} : {}", id, name);
            groupMap[name] = id;
            DeviceGroups[id] = name;
        }
        if (name == "virtual")
            VirtualDeviceGroup = id;
        groupCfg.ignore(1 << 16, '\n');
    }

    UnmanagedDevices.clear();
    UnmanagedGroups.clear();

    for (auto device: config().network().unmanaged_device()) {
        L_NET("Unmanaged network device: {}", device);
        UnmanagedDevices.push_back(device);
    }

    for (auto group: config().network().unmanaged_group()) {
        int id;

        if (groupMap.count(group)) {
            id = groupMap[group];
        } else if (StringToInt(group, id)) {
            L_NET("Unknown network device group: {}", group);
            continue;
        }

        L_NET("Unmanaged network device group: {} : {}", id, group);
        UnmanagedGroups.push_back(id);
    }

    if (config().network().has_device_qdisc())
        StringToStringMap(config().network().device_qdisc(), DeviceQdisc);
    if (config().network().has_device_rate())
        StringToUintMap(config().network().device_rate(), DeviceRate);
    if (config().network().has_device_ceil())
        StringToUintMap(config().network().device_ceil(), DeviceCeil);
    if (config().network().has_default_rate())
        StringToUintMap(config().network().default_rate(), DefaultClassRate);
    if (config().network().has_default_ceil())
        StringToUintMap(config().network().default_ceil(), DefaultClassCeil);
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
    if (config().network().has_container_qdisc_quantum())
        StringToUintMap(config().network().container_qdisc_quantum(), ContainerQdiscQuantum);

    if (config().network().has_ingress_burst())
        StringToUintMap(config().network().ingress_burst(), IngressBurst);

    NetWatchdogPeriod = config().network().watchdog_ms();

    NetProxyNeighbourPeriod = config().network().proxy_ndp_watchdog_ms();

    /* Load default net sysctl from host config */
    for (const auto &p: NetSysctls) {
        auto &key = p.first;
        auto &def = p.second;
        bool set = false;
        for (const auto &it: config().container().net_sysctl())
            set |= it.key() == key;
        std::string val;
        if (!set && !GetSysctl(key, val) && val != def) {
            L_NET("Init sysctl {} = {} (default is {})", key, val, def);
            auto sysctl = config().mutable_container()->add_net_sysctl();
            sysctl->set_key(key);
            sysctl->set_val(val);
        }
    }
}

std::string TNetwork::DeviceGroupName(int group) {
    auto it = DeviceGroups.find(group);
    if (it != DeviceGroups.end())
        return it->second;
    return std::to_string(group);
}

TNetwork::TNetwork() : NatBitmap(0, 0) {
    Nl = std::make_shared<TNl>();
}

TNetwork::~TNetwork() {
    PORTO_ASSERT(NetUsers.empty());
    PORTO_ASSERT(NetClasses.empty());
    PORTO_ASSERT(!NetInode);
}

void TNetwork::Register(std::shared_ptr<TNetwork> &net, ino_t inode) {
    L_NET_VERBOSE("Register network {}", inode);
    Statistics->NetworksCount++;
    auto newList = std::make_shared<std::list<std::shared_ptr<TNetwork>>>();
    for (auto &net_copy: *NetworksList)
        newList->emplace_back(net_copy);
    newList->emplace_back(net);
    std::shared_ptr<const std::list<std::shared_ptr<TNetwork>>> constList(newList);
    // std::atomic_store(&NetworksList, constList);
    NetworksList = constList;
    PORTO_ASSERT(!NetworksIndex[inode]);
    NetworksIndex[inode] = net;
    net->NetInode = inode;
    net->StatTime = GetCurrentTimeMs();
    net->StatGen =  GlobalStatGen.load();
}

void TNetwork::Unregister() {
    L_NET_VERBOSE("Unregister network {} {}", NetInode, NetName);
    Statistics->NetworksCount--;
    auto newList = std::make_shared<std::list<std::shared_ptr<TNetwork>>>();
    for (auto &net: *NetworksList)
        if (net.get() != this)
            newList->emplace_back(net);
    std::shared_ptr<const std::list<std::shared_ptr<TNetwork>>> constList(newList);
    // std::atomic_store(&NetworksList, constList);
    NetworksList = constList;
    NetworksIndex.erase(NetInode);
    NetInode = 0;
}

TError TNetwork::New(TNamespaceFd &netns, std::shared_ptr<TNetwork> &net) {
    TNamespaceFd curNs;
    TError error;

    error = curNs.Open("/proc/thread-self/ns/net");
    if (error)
        return error;

    if (unshare(CLONE_NEWNET))
        return TError::System("unshare(CLONE_NEWNET)");

    error = netns.Open("/proc/thread-self/ns/net");
    if (error)
        return error;

    net = std::make_shared<TNetwork>();

    error = net->Nl->Connect();
    if (error) {
        netns.Close();
        net = nullptr;
    }

    TError error2 = curNs.SetNs(CLONE_NEWNET);
    PORTO_ASSERT(!error2);

    auto networks_lock = LockNetworks();
    Register(net, netns.Inode());

    return error;
}

TError TNetwork::Open(const TPath &path, TNamespaceFd &netns,
                      std::shared_ptr<TNetwork> &net) {
    TNamespaceFd curNs;
    TError error;

    error = netns.Open(path);
    if (error)
        return error;

    auto inode = netns.Inode();

    auto networks_lock = LockNetworks();

    auto it = NetworksIndex.find(inode);
    if (it != NetworksIndex.end()) {
        net = it->second;
        return OK;
    }

    error = curNs.Open("/proc/thread-self/ns/net");
    if (error) {
        netns.Close();
        return error;
    }

    error = netns.SetNs(CLONE_NEWNET);
    if (error) {
        netns.Close();
        return error;
    }

    net = std::make_shared<TNetwork>();

    error = net->Nl->Connect();
    if (error) {
        netns.Close();
        net = nullptr;
    } else
        Register(net, inode);

    TError error2 = curNs.SetNs(CLONE_NEWNET);
    PORTO_ASSERT(!error2);

    return error;
}

void TNetwork::Destroy() {
    TError error;

    PORTO_ASSERT(NetUsers.empty());

    auto networks_lock = LockNetworks();
    Unregister();
    networks_lock.unlock();

    auto lock = LockNet();

    for (auto &dev: Devices) {
        if (!dev.Managed)
            continue;
        TNlQdisc qdisc(dev.Index, TC_H_ROOT, TC_HANDLE(ROOT_TC_MAJOR, ROOT_TC_MINOR));
        error = qdisc.Delete(*Nl);
        if (error)
            L_NET("Cannot remove root qdisc: {}", error);
    }

    // TODO: destroy mac/ip-vlan here

    Nl->Disconnect();
}

TError TNetwork::SetupAddrLabel() {
    TError error;

    for (auto &al: config().network().addrlabel()) {
        TNlAddr prefix;
        error = prefix.Parse(AF_UNSPEC, al.prefix());
        if (error)
            return error;
        error = Nl->AddrLabel(prefix, al.label());
        if (error)
            return error;
    }

    return error;
}

void TNetwork::GetDeviceSpeed(TNetDevice &dev) const {
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

TError TNetwork::SetupIngress(TNetDevice &dev, TNetClass &cfg) {
    TError error;

    if (cfg.RxLimit.empty() || this == HostNetwork.get())
        return OK;

    auto rate = dev.GetConfig(cfg.RxLimit);

    TNlQdisc ingress(dev.Index, TC_H_INGRESS, TC_H_MAJ(TC_H_INGRESS));
    (void)ingress.Delete(*Nl);

    if (!rate)
        return OK;

    ingress.Kind = "ingress";

    error = ingress.Create(*Nl);
    if (error) {
        if (error.Errno != ENODEV && error.Errno != ENOENT)
            L_WRN("Cannot create ingress qdisc: {}", error);

        return error;
    }

    TNlPoliceFilter police(dev.Index, TC_H_INGRESS);
    (void)police.Delete(*Nl);

    police.Mtu = 65536; /* maximum GRO skb */
    police.Rate = rate;
    police.Burst = dev.GetConfig(IngressBurst, std::max(rate / 10, police.Mtu * 10ul));

    error = police.Create(*Nl);
    if (error && error.Errno != ENODEV && error.Errno != ENOENT)
        L_WRN("Can't create ingress filter: {}", error);

    return error;
}

TError TNetwork::SetupQueue(TNetDevice &dev) {
    TError error;

    //
    // 1:0 qdisc (hfsc)
    //  |
    // 1:1 / class
    //  |
    //  +- 1:2 default class
    //  |   |
    //  |   +- 2:0 default class qdisc (sfq)
    //  |
    //  +- 1:4 container a
    //  |   |
    //  |   +- 1:4004 leaf a
    //  |   |   |
    //  |   |   +- 4:0 qdisc a (pfifo)
    //  |   |
    //  |   +- 1:5 container a/b
    //  |       |
    //  |       +- 1:4005 leaf a/b
    //  |           |
    //  |           +- 5:0 qdisc a/b (pfifo)
    //  |
    //  +- 1:6 container b
    //      |
    //      +- 1:4006 leaf b
    //          |
    //          +- 6:0 qdisc b (pfifo)


    L_NET("Setup queue for network {} device {}:{}", NetName, dev.Index, dev.Name);

    TNlQdisc qdisc(dev.Index, TC_H_ROOT, TC_HANDLE(ROOT_TC_MAJOR, ROOT_TC_MINOR));
    qdisc.Kind = dev.GetConfig(DeviceQdisc);
    qdisc.Default = TC_HANDLE(ROOT_TC_MAJOR, DEFAULT_TC_MINOR);
    qdisc.Quantum = 10;

    if (!qdisc.Check(*Nl)) {
        (void)qdisc.Delete(*Nl);
        error = qdisc.Create(*Nl);
        if (error) {
            L_ERR("Cannot create root qdisc: {}", error);
            return error;
        }
    }

    TNlClass cls;

    cls.Kind = dev.GetConfig(DeviceQdisc);
    cls.Index = dev.Index;
    cls.Parent = TC_HANDLE(ROOT_TC_MAJOR, ROOT_TC_MINOR);
    cls.Handle = TC_HANDLE(ROOT_TC_MAJOR, ROOT_CONTAINER_ID);
    cls.Prio = NET_DEFAULT_PRIO;
    cls.Rate = dev.Rate;
    cls.Ceil = dev.Ceil;

    error = cls.Create(*Nl);
    if (error) {
        L_ERR("Can't create root tclass: {}", error);
        return error;
    }

    cls.Parent = TC_HANDLE(ROOT_TC_MAJOR, ROOT_CONTAINER_ID);
    cls.Handle = TC_HANDLE(ROOT_TC_MAJOR, DEFAULT_TC_MINOR);
    cls.Rate = dev.GetConfig(DefaultClassRate);
    cls.Ceil = dev.GetConfig(DefaultClassCeil);

    error = cls.Create(*Nl);
    if (error) {
        L_ERR("Can't create default tclass: {}", error);
        return error;
    }

    TNlCgFilter filter(dev.Index, TC_HANDLE(ROOT_TC_MAJOR, ROOT_TC_MINOR), 1);
    (void)filter.Delete(*Nl);

    error = filter.Create(*Nl);
    if (error) {
        L_ERR("Can't create tc filter: {}", error);
        return error;
    }

    if (ManagedNamespace) {
        TNlQdisc defq(dev.Index, TC_HANDLE(ROOT_TC_MAJOR, DEFAULT_TC_MINOR),
                      TC_HANDLE(DEFAULT_TC_MAJOR, ROOT_TC_MINOR));
        defq.Kind = dev.GetConfig(ContainerQdisc);
        defq.Limit = dev.GetConfig(ContainerQdiscLimit, dev.MTU * 20);
        defq.Quantum = dev.GetConfig(ContainerQdiscQuantum, dev.MTU * 2);
        if (!defq.Check(*Nl)) {
            error = defq.Create(*Nl);
            if (error)
                return error;
        }
    }

    return OK;
}

void TNetwork::SetDeviceOwner(const std::string &name, int owner) {
    if (owner)
        DeviceOwners[name] = owner;
    else
        DeviceOwners.erase(name);

    for (auto &dev: Devices)
        if (dev.Name == name)
            dev.Owner = owner;
}

TError TNetwork::SyncDevices(bool force) {
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

        TNetDevice dev(link);

        /* Ignore our veth pairs */
        if (dev.Type == "veth" &&
            (StringStartsWith(dev.Name, "portove-") ||
             StringStartsWith(dev.Name, "L3-")))
            continue;

        if (DeviceOwners.count(dev.Name))
            dev.Owner = DeviceOwners.at(dev.Name);

        dev.Managed = true;

        /* In managed namespace we control all devices */;
        if (!ManagedNamespace) {
            for (auto &pattern: UnmanagedDevices)
                if (StringMatch(dev.Name, pattern))
                    dev.Managed = false;
            if (std::find(UnmanagedGroups.begin(),
                          UnmanagedGroups.end(), dev.Group) != UnmanagedGroups.end())
                dev.Managed = false;
        }

        if (dev.Type == "tun") {
            /* Ignore TUN/TAP without known owners */
            if (!dev.Owner)
                continue;
            dev.Managed = false;
        }

        if (dev.Type == "dummy")
            dev.Managed = false;

        /* Fallback tunnel for rx only */
        if (dev.Name == "ip6tnl0")
            dev.Managed = false;

        dev.Stat.RxBytes = rtnl_link_get_stat(link, RTNL_LINK_RX_BYTES);
        dev.Stat.RxPackets = rtnl_link_get_stat(link, RTNL_LINK_RX_PACKETS);
        dev.Stat.RxDrops = rtnl_link_get_stat(link, RTNL_LINK_RX_DROPPED);

        dev.Stat.TxBytes = rtnl_link_get_stat(link, RTNL_LINK_TX_BYTES);
        dev.Stat.TxPackets = rtnl_link_get_stat(link, RTNL_LINK_TX_PACKETS);
        dev.Stat.TxDrops = rtnl_link_get_stat(link, RTNL_LINK_TX_DROPPED);

        bool found = false;
        for (auto &d: Devices) {
            if (d.Name != dev.Name || d.Index != dev.Index)
                continue;
            d = dev;
            if (d.Managed && std::string(rtnl_link_get_qdisc(link) ?: "") !=
                    dev.GetConfig(DeviceQdisc)) {
                L_NET("Missing network {} qdisc at {}:{}", NetName, d.Index, d.Name);
                StartRepair();
            } else if (!force)
                d.Prepared = true;

            found = true;
            d.Stat = dev.Stat;
            break;
        }
        if (!found) {
            GetDeviceSpeed(dev);

            L_NET("New network {} {}managed device {}:{} type={} group={} mtu={} speed={}Mbps {}iB/s",
                    NetName, dev.Managed ? "" : "un",
                    dev.Index, dev.Name, dev.Type, dev.GroupName, dev.MTU,
                    dev.Ceil / 125000, StringFormatSize(dev.Ceil));

            if (dev.Managed)
                StartRepair();

            if (this == HostNetwork.get()) {
                RootContainer->NetClass.Rate[dev.Name] = dev.Rate;
                RootContainer->NetClass.Limit[dev.Name] = dev.Ceil;
                RootContainer->NetClass.RxLimit[dev.Name] = dev.Ceil;
            }

            Devices.push_back(dev);
        }
    }

    nl_cache_free(cache);

    for (auto dev = Devices.begin(); dev != Devices.end(); ) {
        if (dev->Missing) {

            L_NET("Forget network {} device {}:{}", NetName, dev->Index, dev->Name);

            if (this == HostNetwork.get()) {
                RootContainer->NetClass.Rate.erase(dev->Name);
                RootContainer->NetClass.Limit.erase(dev->Name);
                RootContainer->NetClass.RxLimit.erase(dev->Name);
            }
            dev = Devices.erase(dev);
        } else
            dev++;
    }

    return OK;
}

TError TNetwork::GetGateAddress(std::vector<TNlAddr> addrs,
                                TNlAddr &gate4, TNlAddr &gate6,
                                int &mtu, int &group) {
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

                     if (!group)
                         group = rtnl_link_get_group(link);

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

    return OK;
}

TError TNetwork::SetupProxyNeighbour(const std::vector <TNlAddr> &ips,
                                     const std::string &master) {
    struct nl_cache *cache;
    TError error;
    int ret;

    if (master != "") {
        for (auto &dev : Devices) {
            if (StringMatch(dev.Name, master)) {
                for (auto &ip: ips) {
                    error = Nl->ProxyNeighbour(dev.Index, ip, true);
                    if (error)
                        goto err;
                }
            }
        }
        return OK;
    }

    ret = rtnl_addr_alloc_cache(GetSock(), &cache);
    if (ret < 0)
        return Nl->Error(ret, "Cannot allocate addr cache");

    for (auto &dev : Devices) {
        for (auto &ip: ips) {
            bool reachable = false;

            for (auto obj = nl_cache_get_first(cache); obj;
                    obj = nl_cache_get_next(obj)) {
                auto raddr = (struct rtnl_addr *)obj;
                auto local = rtnl_addr_get_local(raddr);

                if (rtnl_addr_get_ifindex(raddr) == dev.Index &&
                        local && nl_addr_cmp_prefix(local, ip.Addr) == 0) {
                    reachable = true;
                    break;
                }
            }

            /* Add proxy entry only if address is directly reachable */
            if (reachable) {
                error = Nl->ProxyNeighbour(dev.Index, ip, true);
                if (error)
                    goto err_addr;
            }
        }
    }

err_addr:
    nl_cache_free(cache);

err:
    if (error)
        for (auto &dev: Devices)
            for (auto &ip: ips)
                Nl->ProxyNeighbour(dev.Index, ip, false);

    return error;
}

TError TNetwork::AddProxyNeightbour(const std::vector<TNlAddr> &ips,
                                    const std::string &master) {
    TError error;
    if (config().network().proxy_ndp()) {
        error = SetupProxyNeighbour(ips, master);
        if (error)
            return error;
        for (auto ip: ips)
            Neighbours.emplace_back(TNetProxyNeighbour{ip, master});
    }
    return error;
}

void TNetwork::DelProxyNeightbour(const std::vector<TNlAddr> &ips) {
    TError error;
    if (config().network().proxy_ndp()) {
        for (auto &ip: ips) {
            for (auto &dev: Devices) {
                error = Nl->ProxyNeighbour(dev.Index, ip, false);
                if (error)
                    L_ERR("Cannot remove proxy neighbour: {}", error);
            }
            Neighbours.remove_if([&](TNetProxyNeighbour &np) { return ip.IsEqual(np.Ip); });
        }
    }
}

void TNetwork::RepairProxyNeightbour() {
    struct nl_cache *cache = nullptr;
    auto sk = GetSock();
    TError error;
    int ret;
    struct ndmsg ndmsg = {};

    if (Neighbours.empty())
        return;

    ndmsg.ndm_family = AF_UNSPEC;
    ndmsg.ndm_flags = NTF_PROXY;

    ret = nl_cache_alloc_name("route/neigh", &cache);
    if (ret >= 0)
        ret = nl_send_simple(sk, RTM_GETNEIGH, NLM_F_DUMP, &ndmsg, sizeof(ndmsg));
    if (ret >= 0)
        ret = nl_cache_pickup(sk, cache);
    if (ret < 0) {
        nl_cache_free(cache);
        L_ERR("{}", Nl->Error(ret, "Cannot dump proxy neighbour"));
        return;
    }

    for (auto &nb: Neighbours) {
        bool found = false;

        for (auto obj = nl_cache_get_first(cache); obj;
                obj = nl_cache_get_next(obj)) {
            auto neigh = (struct rtnl_neigh *)obj;
            auto dst = rtnl_neigh_get_dst(neigh);

            if (dst && !nl_addr_cmp(nb.Ip.Addr, dst) &&
                    (rtnl_neigh_get_flags(neigh) & NTF_PROXY)) {
                found = true;
                break;
            }
        }

        if (!found) {
            error = SetupProxyNeighbour({nb.Ip}, nb.Master);
            if (error)
                L_ERR("Cannot setup proxy neighbour: {}", error);
        }
    }

    nl_cache_free(cache);
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

    return OK;
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

    return OK;
}

std::string TNetwork::NewDeviceName(const std::string &prefix) {
    for (int retry = 0; retry < 100; retry++) {
        std::string name = prefix + std::to_string(IfaceSeq++);
        TNlLink link(Nl, name);
        if (link.Load())
            return name;
    }
    return prefix + "0";
}

int TNetwork::DeviceIndex(const std::string &name) {
    for (auto &dev: Devices)
        if (dev.Name == name)
            return dev.Index;
    return 0;
}

std::string TNetwork::MatchDevice(const std::string &pattern) {
    for (auto &dev: Devices) {
        if (StringMatch(dev.Name, pattern))
            return dev.Name;
    }
    return pattern;
}

TError TNetwork::SetupClass(TNetDevice &dev, TNetClass &cfg) {
    TError error;
    TNlClass cls;

    if (this == HostNetwork.get())
        cls.Parent = cfg.HostParentHandle;
    else
        cls.Parent = cfg.ParentHandle;
    cls.Handle = cfg.Handle;

    if (cfg.Handle == TC_HANDLE(ROOT_TC_MAJOR, ROOT_CONTAINER_ID))
        cls.defRate = dev.Rate;
    else
        cls.defRate = dev.GetConfig(ContainerRate);

    cls.Index = dev.Index;
    cls.Kind = dev.GetConfig(DeviceQdisc);
    cls.Prio = dev.GetConfig(cfg.Prio);
    cls.Rate = dev.GetConfig(cfg.Rate);
    cls.Ceil = dev.GetConfig(cfg.Limit);
    cls.Quantum = dev.GetConfig(DeviceQuantum, dev.MTU * 2);
    cls.RateBurst = dev.GetConfig(DeviceRateBurst, dev.MTU * 10);
    cls.CeilBurst = dev.GetConfig(DeviceCeilBurst, dev.MTU * 10);

    error = cls.Create(*Nl);
    if (error) {
        (void)cls.Delete(*Nl);
        error = cls.Create(*Nl);
    }

    if (error)
        return TError(error, "tc class");

    if (!cfg.Leaf)
        return OK;

    TNlQdisc ctq(dev.Index, cfg.Leaf, TC_HANDLE(TC_H_MIN(cfg.Handle), CONTAINER_TC_MINOR));

    cls.Parent = cfg.Handle;
    cls.Handle = cfg.Leaf;

    if (cfg.Leaf == TC_HANDLE(ROOT_TC_MAJOR, DEFAULT_TC_MINOR)) {
        cls.Rate = dev.GetConfig(DefaultClassRate);
        cls.Ceil = dev.GetConfig(DefaultClassCeil);
        cls.defRate = cls.Rate;

        ctq.Handle = TC_HANDLE(DEFAULT_TC_MAJOR, ROOT_TC_MINOR);
        ctq.Kind = dev.GetConfig(DefaultQdisc);
        ctq.Limit = dev.GetConfig(DefaultQdiscLimit);
        ctq.Quantum = dev.GetConfig(DefaultQdiscQuantum, dev.MTU * 2);
    } else {
        cls.Ceil = 0;

        ctq.Kind = dev.GetConfig(ContainerQdisc);
        ctq.Limit = dev.GetConfig(ContainerQdiscLimit, dev.MTU * 20);
        ctq.Quantum = dev.GetConfig(ContainerQdiscQuantum, dev.MTU * 2);
    }

    error = cls.Create(*Nl);
    if (error)
        return TError(error, "leaf tc class");

    error = ctq.Create(*Nl);
    if (error) {
        (void)ctq.Delete(*Nl);
        error = ctq.Create(*Nl);
    }

    if (error)
        return TError(error, "leaf tc qdisc");

    return OK;
}

TError TNetwork::DeleteClass(TNetDevice &dev, TNetClass &cfg) {
    TError error;

    /* default class qdisc */
    if (cfg.Leaf == TC_HANDLE(ROOT_TC_MAJOR, DEFAULT_TC_MINOR))
        return OK;

    TNlQdisc ctq(dev.Index, cfg.Handle, TC_HANDLE(TC_H_MIN(cfg.Handle), CONTAINER_TC_MINOR));
    (void)ctq.Delete(*Nl);

    if (cfg.Leaf) {
        TNlClass cls(dev.Index, TC_H_UNSPEC, cfg.Leaf);
        (void)cls.Delete(*Nl);
    }

    TNlClass cls(dev.Index, TC_H_UNSPEC, cfg.Handle);
    error = cls.Delete(*Nl);
    if (error && error.Errno != ENODEV && error.Errno != ENOENT)
        return TError(error, "cannot remove class");

    return OK;
}

TError TNetwork::TrySetupClasses(TNetClass &cls) {
    auto net_lock = LockNet();
    auto state_lock = LockNetState();
    TError error;

    for (auto &dev: Devices) {
        if (!dev.Managed || !dev.Prepared)
            continue;

        error = SetupClass(dev, cls);
        if (error)
            return error;

        error = SetupIngress(dev, cls);
        if (error)
            return error;
    }

    state_lock.unlock();
    net_lock.unlock();

    return OK;
}

TError TNetwork::SetupClasses(TNetClass &cls) {
    TError error;

    error = TrySetupClasses(cls);
    if (error) {
        L_NET_VERBOSE("Network {} class setup failed: {}", NetName, error);
        StartRepair();
        error = WaitRepair();
        if (error)
            return error;
        error = TrySetupClasses(cls);
        if (error) {
            StartRepair();
            return error;
        }
    }

    if (this != HostNetwork.get())
        return HostNetwork->SetupClasses(cls);

    return OK;
}

void TNetwork::InitClass(TContainer &ct) {
    auto &cls = ct.NetClass;
    cls.Owner = ct.Id;
    if (!ct.Parent) {
        cls.HostParentHandle = TcHandle(ROOT_TC_MAJOR, ROOT_TC_MINOR);
        cls.ParentHandle = TcHandle(ROOT_TC_MAJOR, ROOT_TC_MINOR);
        cls.Handle = TcHandle(ROOT_TC_MAJOR, ROOT_CONTAINER_ID);
        cls.Leaf = TcHandle(ROOT_TC_MAJOR, DEFAULT_TC_MINOR);
        cls.Fold = &cls;
        cls.Parent = nullptr;
    } else if (ct.Controllers & CGROUP_NETCLS) {
        cls.HostParentHandle = ct.Parent->NetClass.Handle;
        if (ct.NetInherit)
            cls.ParentHandle = ct.Parent->NetClass.Handle;
        else
            cls.ParentHandle = TcHandle(ROOT_TC_MAJOR, ROOT_CONTAINER_ID);
        cls.Handle = TcHandle(ROOT_TC_MAJOR, ct.Id);
        cls.Leaf = TcHandle(ROOT_TC_MAJOR, ct.Id + CONTAINER_ID_MAX);
        cls.Fold = &cls;
        cls.Parent = ct.Parent->NetClass.Fold;
    } else {
        /* Fold into parent */
        cls.HostParentHandle = ct.Parent->NetClass.HostParentHandle;
        cls.ParentHandle = ct.Parent->NetClass.ParentHandle;
        cls.Handle = ct.Parent->NetClass.Handle;
        cls.Leaf = ct.Parent->NetClass.Leaf;
        cls.Fold = ct.Parent->NetClass.Fold;
        cls.Parent = ct.Parent->NetClass.Parent;
    }
}

void TNetwork::RegisterClass(TNetClass &cls) {
    auto pos = std::find(NetClasses.begin(), NetClasses.end(), cls.Parent);
    if (pos == NetClasses.end())
        NetClasses.push_back(&cls);
    else
        NetClasses.insert(++pos, &cls);
    cls.Registered++;
}

void TNetwork::UnregisterClass(TNetClass &cls) {
    auto pos = std::find(NetClasses.begin(), NetClasses.end(), &cls);
    if (pos != NetClasses.end()) {
        NetClasses.erase(pos);
        cls.Registered--;
    }
}

void TNetwork::FatalError(TError &error) {
    L_ERR("Fatal network {} error: {}", NetName, error);
    if (!NetError || NetError == EError::Queued)
        NetError = error;
}

TError TNetwork::Reconnect() {
    TNamespaceFd netns, cur_ns;
    TError error;

    if (this == HostNetwork.get())
        return Nl->Connect();

    error = cur_ns.Open("/proc/thread-self/ns/net");
    if (error)
        return error;

    auto state_lock = LockNetState();
    for (auto ct: NetUsers) {
        error = netns.Open(ct->Task.Pid, "ns/net");
        if (!error && netns.Inode() != NetInode)
            error = TError(EError::Unknown, "Wrong net-ns inode");
        if (!error)
            error = netns.SetNs(CLONE_NEWNET);
        if (!error)
            error = Nl->Connect();
        if (!error)
            break;
    }
    state_lock.unlock();

    TError error2 = cur_ns.SetNs(CLONE_NEWNET);
    PORTO_ASSERT(!error2);
    return error;
}

TError TNetwork::RepairLocked() {
    bool force = NetError != EError::Queued;
    TError error;

    NetError = TError::Queued();

    error = SyncDevices();
    if (error) {
        L_NET("Reconnect network {} netlink after: {}", NetName, error);
        error = Reconnect();
        if (error)
            L_NET("Cannot reconnect network {} netlink: {}", NetName, error);
        error = SyncDevices();
    }
    if (error) {
        FatalError(error);
        return error;
    }

    auto state_lock = LockNetState();

    for (auto &dev: Devices) {
        if (!dev.Managed)
            continue;

        if (!dev.Prepared || force) {
            error = SetupQueue(dev);
            if (error) {
                FatalError(error);
                continue;
            }
            dev.Prepared = true;
        }

        for (auto cls: NetClasses) {
            error = SetupClass(dev, *cls);
            if (error) {
                FatalError(error);
                break;
            }
            error = SetupIngress(dev, *cls);
            if (error) {
                FatalError(error);
                break;
            }
        }
    }

    state_lock.unlock();

    if (NetError == EError::Queued)
        NetError = OK;

    NetCv.notify_all();

    return NetError;
}

TError TNetwork::SyncResolvConf() {
    TTuple conf;
    TError error;

    if (!config().container().default_resolv_conf().empty()) {
        conf = SplitEscapedString(config().container().default_resolv_conf(), ';');
    } else {
        error = TPath("/etc/resolv.conf").ReadLines(conf);
        if (error)
            return error;

        if (!ResolvConfPeriod) {
            ResolvConfPeriod = config().network().resolv_conf_watchdog_ms();
        } else if (conf != ResolvConfPrev) {
            ResolvConfPrev = conf;
            return OK;
        }
    }

    if (conf == ResolvConfCurrent)
        return OK;

    error = WatchdogClient.LockContainer(RootContainer);
    if (error)
        return error;

    L_ACT("Set default resolv_conf:\n{}\n", MergeEscapeStrings(conf, '\n'));

    ResolvConfCurrent = conf;
    RootContainer->ResolvConf = conf;

    for (auto &ct: RootContainer->Subtree()) {
        if (ct->Root != "/" && !ct->HasProp(EProperty::RESOLV_CONF) &&
                ct->State != EContainerState::Dead &&
                ct->State != EContainerState::Stopped) {
            error = ct->ApplyResolvConf();
            if (error)
                L_WRN("Cannot apply resolv_conf CT{}:{} : {}", ct->Id, ct->Name, error);
        }
    }

    WatchdogClient.ReleaseContainer();

    return OK;
}

void TNetwork::NetWatchdog() {
    auto LastProxyNeighbour = GetCurrentTimeMs();
    auto LastResolvConf = LastProxyNeighbour;
    TError error;

    SetProcessName("portod-network");
    while (HostNetwork) {
        auto nets = Networks();
        for (auto &net: *nets) {
            auto lock = net->LockNet();
            if (GetCurrentTimeMs() - net->StatTime >= NetWatchdogPeriod) {
                GlobalStatGen++;
                net->SyncStatLocked();
            }
            if (net->NetError)
                net->RepairLocked();
            if (net->NetError)
                net->RepairLocked();
        }
        if (GetCurrentTimeMs() - LastProxyNeighbour >= NetProxyNeighbourPeriod) {
            auto lock = HostNetwork->LockNet();
            HostNetwork->RepairProxyNeightbour();
            LastProxyNeighbour = GetCurrentTimeMs();
        }
        if (ResolvConfPeriod && GetCurrentTimeMs() - LastResolvConf >= ResolvConfPeriod) {
            TNetwork::SyncResolvConf();
            LastResolvConf = GetCurrentTimeMs();
        }
        auto lock = LockNetworks();
        NetThreadCv.wait_for(lock, std::chrono::milliseconds(NetWatchdogPeriod/2));
    }
}

void TNetwork::StartRepair() {
    if (!NetError)
        L_NET_VERBOSE("Start network {} repair", NetName);
    if (!NetError)
        NetError = TError::Queued();
    NetThreadCv.notify_all();
}

TError TNetwork::WaitRepair() {

    if (HostNetwork && this != HostNetwork.get()) {
        TError error = HostNetwork->WaitRepair();
        if (error)
            return error;
    }

    auto net_lock = LockNet();
    NetCv.wait(net_lock, [this]{return NetError != EError::Queued;});

    return NetError;
}

void TNetwork::InitStat(TNetClass &cls) {
    cls.Stat.clear();
    for (auto &dev: Devices) {
        cls.Stat[dev.Name] = dev.Stat;
        cls.Stat["group " + dev.GroupName] += dev.Stat;
    }
}

void TNetwork::SyncStatLocked() {
    bool hostNet = this == HostNetwork.get();
    TError error;

    auto curTime = GetCurrentTimeMs();
    auto curGen = GlobalStatGen.load();

    L_NET_VERBOSE("Sync network {} statistics generation {} after {} ms",
          NetName, (unsigned)curGen, curTime - StatTime);

    error = SyncDevices();
    if (error) {
        StartRepair();
        return;
    }

    for (auto &dev: Devices) {
        dev.ClassCache = nullptr;

        if (!dev.Managed || !dev.Prepared)
            continue;

        int ret = rtnl_class_alloc_cache(GetSock(), dev.Index, &dev.ClassCache);
        if (ret) {
            L_NET("Cannot dump network {} classes at {}:{}", NetName, dev.Index, dev.Name);
            StartRepair();
            continue;
        }
    }

    auto state_lock = LockNetState();

    for (auto cls: NetClasses) {
        if (hostNet && !cls->HostClass)
            continue;
        cls->Stat.clear();
    }

    for (auto &dev: Devices) {

        for (auto cls: NetClasses) {
            if (hostNet && !cls->HostClass)
                continue;

            if (dev.Owner && dev.Owner != cls->Owner)
                continue;

            TNetStat &stat = cls->Stat[dev.Name];

            stat = dev.Stat;

            if (!dev.ClassCache)
                continue;

            struct rtnl_class *tc = rtnl_class_get(dev.ClassCache, dev.Index, cls->Leaf);
            if (!tc) {
                L_NET("Missing network {} class {} at {}:{}", NetName, cls->Leaf, dev.Index, dev.Name);
                StartRepair();
                continue;
            }

            stat.Packets += rtnl_tc_get_stat(TC_CAST(tc), RTNL_TC_PACKETS);
            stat.Bytes += rtnl_tc_get_stat(TC_CAST(tc), RTNL_TC_BYTES);
            stat.Drops += rtnl_tc_get_stat(TC_CAST(tc), RTNL_TC_DROPS);
            stat.Overlimits += rtnl_tc_get_stat(TC_CAST(tc), RTNL_TC_OVERLIMITS);
            rtnl_class_put(tc);
        }

        for (auto it = NetClasses.rbegin(); it != NetClasses.rend(); ++it) {
            auto cls = *it;

            if (hostNet && !cls->HostClass)
                continue;

            if (dev.Owner && dev.Owner != cls->Owner)
                continue;

            auto &stat = cls->Stat[dev.Name];

            if (cls->Parent && dev.Managed && !dev.Owner) {
                auto &pstat = cls->Parent->Stat[dev.Name];
                pstat.Packets += stat.Packets;
                pstat.Bytes += stat.Bytes;
                pstat.Drops += stat.Drops;
                pstat.Overlimits += stat.Overlimits;
            }

            cls->Stat["group " + dev.GroupName] += stat;
        }
    }

    StatTime = curTime;
    StatGen = curGen;

    state_lock.unlock();

    for (auto &dev: Devices) {
        if (dev.ClassCache)
            nl_cache_free(dev.ClassCache);
        dev.ClassCache = nullptr;
    }
}

void TNetwork::SyncStat() {
    auto ourGen = GlobalStatGen.fetch_add(1) + 1;
    auto net_lock = LockNet();
    if (ourGen - StatGen > 0)
        SyncStatLocked();
}

void TNetwork::SyncAllStat() {
    auto nets = Networks();
    auto ourGen = GlobalStatGen.fetch_add(1) + 1;
    for (auto &net: *nets) {
        if (ourGen - net->StatGen > 0) {
            auto lock = net->LockNet();
            if (ourGen - net->StatGen > 0)
                net->SyncStatLocked();
        }
    }
}

TError TNetwork::StartNetwork(TContainer &ct, TTaskEnv &task) {
    TNetEnv env;

    TError error = env.Parse(ct);
    if (error)
        return error;

    error = env.CheckIpLimit();
    if (error)
        return error;

    error = env.OpenNetwork();
    if (error)
        return error;

    if (env.SaveIp)
        env.FormatIp(ct.IpList);

    for (auto &dev: env.Devices) {
        if (dev.Autoconf)
            task.Autoconf.push_back(dev.Name);
        if (dev.Type == "tap") {
            auto lock = env.Net->LockNet();
            error = env.CreateTap(dev);
            if (error) {
                for (auto &dev: env.Devices)
                    if (dev.Type == "tap")
                        (void)env.DestroyTap(dev);
                return error;
            }
        }
    }

    ct.NetClass.HostClass = env.Net == HostNetwork;

    if ((ct.Controllers & CGROUP_NETCLS) && !ct.NetClass.HostClass) {
        auto lock = HostNetwork->LockNetState();
        HostNetwork->RegisterClass(ct.NetClass);
    }

    auto lock = env.Net->LockNetState();
    ct.Net = env.Net;
    if (ct.Controllers & CGROUP_NETCLS) {
        ct.Net->RegisterClass(ct.NetClass);
        ct.Net->InitStat(ct.NetClass);
    }
    ct.Net->NetUsers.push_back(&ct);
    lock.unlock();

    task.NetFd = std::move(env.NetNs);

    if (ct.Controllers & CGROUP_NETCLS) {
        error = ct.Net->SetupClasses(ct.NetClass);
        if (error)
            return error;
    }

    return OK;
}

void TNetwork::StopNetwork(TContainer &ct) {
    TError error;

    auto state_lock = ct.LockNetState();

    auto net = ct.Net;
    ct.Net = nullptr;
    if (!net)
        return;

    if (ct.Controllers & CGROUP_NETCLS)
        net->UnregisterClass(ct.NetClass);

    net->NetUsers.remove(&ct);
    bool last = net->NetUsers.empty();

    state_lock.unlock();

    if (ct.Controllers & CGROUP_NETCLS) {
        auto net_lock = net->LockNet();
        for (auto &dev: net->Devices) {
            if (!dev.Managed || !dev.Prepared)
                continue;
            error = net->DeleteClass(dev, ct.NetClass);
            if (error)
                L_NET("Cannot delete network {} class CT{}:{}: {}",
                  net->NetName, ct.Id, ct.Name, error);
        }
    }

    if ((ct.Controllers & CGROUP_NETCLS) && !ct.NetClass.HostClass) {
        state_lock = HostNetwork->LockNetState();
        HostNetwork->UnregisterClass(ct.NetClass);
        state_lock.unlock();

        auto net_lock = HostNetwork->LockNet();
        for (auto &dev: HostNetwork->Devices) {
            if (!dev.Managed || !dev.Prepared)
                continue;
            error = HostNetwork->DeleteClass(dev, ct.NetClass);
            if (error)
                L_NET("Cannot delete network {} class CT{}:{}: {}",
                  HostNetwork->NetName, ct.Id, ct.Name, error);
        }
    }

    PORTO_ASSERT(!ct.NetClass.Registered);

    TNetEnv env;
    (void)env.Parse(ct);
    env.Net = net;

    for (auto &dev: env.Devices) {
        if (dev.Type != "tap")
            continue;
        auto lock = net->LockNet();
        error = env.DestroyTap(dev);
        if (error)
            L_ERR("Cannot remove tap device {} : {}", dev.Name, error);
    }

    if (!last)
        return;

    net->Destroy();

    if (net == HostNetwork) {
        auto lock = LockNetworks();
        HostNetwork = nullptr;
        lock.unlock();
        NetThreadCv.notify_all();
        NetThread.join();
    }

    for (auto &dev : env.Devices) {
        if (dev.Type != "L3")
            continue;
        auto lock = HostNetwork->LockNet();
        if (dev.Mode == "NAT") {
            error = HostNetwork->PutNatAddress(dev.Ip);
            if (error)
                L_ERR("Cannot put NAT address : {}", error);
            dev.Ip.clear();
            env.SaveIp = true;
        } else {
            HostNetwork->DelProxyNeightbour(dev.Ip);
        }
    }

    if (env.SaveIp)
        env.FormatIp(ct.IpList);
}

TError TNetwork::RestoreNetwork(TContainer &ct) {
    std::shared_ptr<TNetwork> net;
    TNamespaceFd netNs;
    TError error;
    TNetEnv env;

    if (ct.NetInherit)
        net = ct.Parent->Net;
    else if (ct.Task.Pid) {
        error = TNetwork::Open("/proc/" + std::to_string(ct.Task.Pid) + "/ns/net", netNs, net);
        if (error) {
            if (ct.WaitTask.IsZombie())
                return OK;
            return error;
        }
    } else {
        /* FIXME kludge for restoring dead container, add dead network for that */
        L("Cannot restore network. Task is dead. Create fake netns.");
        TNamespaceFd netns;
        error = TNetwork::New(netns, net);
        if (error)
            return error;
    }

    error = env.Parse(ct);
    if (error)
        return error;

    for (auto &dev: env.Devices) {
        if (dev.Type == "tap" || dev.Type == "L3") {
            auto lock = HostNetwork->LockNet();
            error = HostNetwork->AddProxyNeightbour(dev.Ip, dev.Master);
            if (error)
                return error;
        }
    }

    ct.NetClass.HostClass = net == HostNetwork;

    if ((ct.Controllers & CGROUP_NETCLS) && !ct.NetClass.HostClass) {
        auto state_lock = HostNetwork->LockNetState();
        HostNetwork->RegisterClass(ct.NetClass);
    }

    auto state_lock = net->LockNetState();
    if (ct.Controllers & CGROUP_NETCLS) {
        net->RegisterClass(ct.NetClass);
        net->InitStat(ct.NetClass);
    }

    ct.Net = net;
    net->NetUsers.push_back(&ct);

    return OK;
}

//
// TNetEnv - container network configuration
//

std::string TNetEnv::GenerateHw(const std::string &name) {
    uint32_t n = Crc32(name);
    uint32_t h = Crc32(Hostname);

    return StringFormat("02:%02x:%02x:%02x:%02x:%02x",
            (n & 0x000000FF) >> 0,
            (h & 0xFF000000) >> 24,
            (h & 0x00FF0000) >> 16,
            (h & 0x0000FF00) >> 8,
            (h & 0x000000FF) >> 0);
}

TError TNetEnv::ParseNet(TMultiTuple &net_settings) {
    int vethIdx = 0;
    TError error;

    if (net_settings.size() == 0)
        return TError(EError::InvalidValue, "Configuration is not specified");

    NetIsolate = false;
    NetInherit = false;
    L3Only = true;

    if (config().network().enable_ip6tnl0()) {
        TNetDeviceConfig dev;
        dev.Type = "ip6tnl0";
        dev.Name = "ip6tnl0";
        Devices.push_back(dev);
    }

    for (auto &settings : net_settings) {
        TNetDeviceConfig dev;

        if (!settings.size())
            continue;

        auto line = MergeEscapeStrings(settings, ' ');
        auto type = StringTrim(settings[0]);

        if (type == "inherited" || (type == "host" && settings.size() == 1)) {
            NetInherit = true;
        } else if (type == "none") {
            NetIsolate = true;
            if (net_settings.size() != 1)
                return TError(EError::InvalidValue, "net=none must be the only option");
        } else if (type == "container") {
            if (settings.size() != 2)
                return TError(EError::InvalidValue, "Invalid " + line);
            L3Only = false;
            NetCtName = StringTrim(settings[1]);
        } else if (type == "netns") {
            if (settings.size() != 2)
                return TError(EError::InvalidValue, "Invalid " + line);
            std::string name = StringTrim(settings[1]);
            TPath path("/var/run/netns/" + name);
            if (!path.Exists())
                return TError(EError::InvalidValue, "net namespace not found: " + name);
            L3Only = false;
            NetNsName = name;
        } else if (type == "steal" || type == "host" /* legacy */) {
            if (settings.size() != 2)
                return TError(EError::InvalidValue, "Invalid " + line);
            dev.Type = "steal";
            dev.Name = StringTrim(settings[1]);
        } else if (type == "macvlan") {
            if (settings.size() < 3)
                return TError(EError::InvalidValue, "Invalid " + line);

            dev.Type = "macvlan";
            dev.Name = StringTrim(settings[2]);
            dev.Master = StringTrim(settings[1]);
            dev.Mode = "bridge";

            if (settings.size() > 3) {
                dev.Mode = StringTrim(settings[3]);
                if (!TNlLink::ValidMacVlanType(dev.Mode))
                    return TError(EError::InvalidValue, "Invalid macvlan mode " + dev.Mode);
            }

            if (settings.size() > 4) {
                error = StringToInt(settings[4], dev.Mtu);
                if (error)
                    return error;
            }

            if (settings.size() > 5) {
                dev.Mac = StringTrim(settings[5]);
                if (!TNlLink::ValidMacAddr(dev.Mac))
                    return TError(EError::InvalidValue, "Invalid macvlan mac " + dev.Mac);
            }

            /* Legacy kludge */
            if (dev.Mac.empty() && !Hostname.empty())
                dev.Mac = GenerateHw(dev.Master + dev.Name);
        } else if (type == "ipvlan") {
            if (settings.size() < 3)
                return TError(EError::InvalidValue, "Invalid " + line);

            dev.Type = "ipvlan";
            dev.Name = StringTrim(settings[2]);
            dev.Master = StringTrim(settings[1]);
            dev.Mode = "l2";

            if (settings.size() > 3) {
                dev.Mode = StringTrim(settings[3]);
                if (!TNlLink::ValidIpVlanMode(dev.Mode))
                    return TError(EError::InvalidValue, "Invalid ipvlan mode " + dev.Mode);
            }

            if (settings.size() > 4) {
                error = StringToInt(settings[4], dev.Mtu);
                if (error)
                    return error;
            }
        } else if (type == "veth") {
            if (settings.size() < 3)
                return TError(EError::InvalidValue, "Invalid " + line);

            dev.Type = "veth";
            dev.Name = StringTrim(settings[1]);
            dev.Master = StringTrim(settings[2]);

            if (settings.size() > 3) {
                error = StringToInt(settings[3], dev.Mtu);
                if (error)
                    return error;
            }

            if (settings.size() > 4) {
                dev.Mac = StringTrim(settings[4]);
                if (!TNlLink::ValidMacAddr(dev.Mac))
                    return TError(EError::InvalidValue, "Invalid veth mac " + dev.Mac);
            }

            /* Legacy kludge */
            if (dev.Mac.empty() && !Hostname.empty())
                dev.Mac = GenerateHw(dev.Name + "portove-" + std::to_string(Id) +
                                     "-" + std::to_string(vethIdx));
            vethIdx++;
        } else if (type == "L3") {
            dev.Type = "L3";
            dev.Name = "eth0";
            if (settings.size() > 1)
                dev.Name = StringTrim(settings[1]);
            if (settings.size() > 2)
                dev.Master = StringTrim(settings[2]);
            if (config().network().l3_default_mtu() > 0)
                dev.Mtu = config().network().l3_default_mtu();
        } else if (type == "NAT") {
            dev.Type = "L3";
            dev.Mode = "NAT";
            dev.Name = "eth0";
            if (settings.size() > 1)
                dev.Name = StringTrim(settings[1]);
            if (config().network().l3_default_mtu() > 0)
                dev.Mtu = config().network().l3_default_mtu();
        } else if (type == "ipip6") {
            if (settings.size() != 4)
                return TError(EError::InvalidValue, "Invalid " + line);
            dev.Type = "ipip6";
            dev.Name = StringTrim(settings[1]);
            if (dev.Name == "ip6tnl0")
                return TError(EError::InvalidValue,
                              "Cannot modify default fallback tunnel");
            error = dev.IpIp6.Remote.Parse(AF_INET6, settings[2]);
            if (error)
                return error;
            error = dev.IpIp6.Local.Parse(AF_INET6, settings[3]);
            if (error)
                return error;
            /* As in net/ipv6/ip6_tunnel.c we do not set IP6_TNL_F_IGN_ENCAP_LIMIT */
            dev.Mtu = ETH_DATA_LEN - sizeof(struct ip6_hdr) - 8;
        } else if (type == "tap") {
            if (settings.size() != 2)
                return TError(EError::InvalidValue, "Invalid " + line);
            dev.Type = "tap";
            dev.Name = StringTrim(settings[1]);
            dev.Tap.Uid = TaskCred.Uid;
            dev.Tap.Gid = TaskCred.Gid;
        } else if (type == "MTU") {
            if (settings.size() != 3)
                return TError(EError::InvalidValue, "Invalid " + line);
            int mtu;
            error = StringToInt(settings[2], mtu);
            if (error)
                return error;
            bool found = false;
            for (auto &dev: Devices) {
                if (dev.Name == settings[1]) {
                    dev.Mtu = mtu;
                    found = true;
                    break;
                }
            }
            if (!found)
                return TError(EError::InvalidValue, "Link not found: " + settings[1]);
        } else if (type == "MAC") {
            if (settings.size() != 3 || !TNlLink::ValidMacAddr(settings[2]))
                return TError(EError::InvalidValue, "Invalid " + line);
            bool found = false;
            for (auto &dev: Devices) {
                if (dev.Name == settings[1]) {
                    dev.Mac = settings[2];
                    found = true;
                    break;
                }
            }
            if (!found)
                return TError(EError::InvalidValue, "Link not found: " + settings[1]);
        } else if (type == "autoconf") {
            if (settings.size() != 2)
                return TError(EError::InvalidValue, "Invalid " + line);
            bool found = false;
            for (auto &dev: Devices) {
                if (dev.Name == settings[1]) {
                    dev.Autoconf = true;
                    found = true;
                    break;
                }
            }
            if (!found)
                return TError(EError::InvalidValue, "Link not found: " + settings[1]);
        } else if (type == "ip") {
            if (!config().network().enable_iproute())
                return TError(EError::Permission, "iproute is disabled");
            IpRoute.push_back(settings);
            NetIsolate = true;
            L3Only = false;
        } else
            return TError(EError::InvalidValue, "Unknown net option: " + type);

        if (dev.Type != "") {
            Devices.push_back(dev);
            if (dev.Type != "L3" && dev.Type != "ipip6" && dev.Type != "tap")
                L3Only = false;
            if (dev.Type == "tap")
                NetInherit = true;
            else
                NetIsolate = true;
        }
    }

    if (!!NetInherit + !!NetIsolate + !NetNsName.empty() + !NetCtName.empty() != 1)
        return TError(EError::InvalidValue, "Uncertain network type");

    return OK;
}

TError TNetEnv::CheckIpLimit() {
    /* no changes -> no limits */
    if (NetInherit)
        return OK;

    for (auto ct = Parent; ct; ct = ct->Parent) {

        /* empty means no limit */
        if (ct->IpLimit.empty() ||
                (ct->IpLimit.size() == 1 &&
                 ct->IpLimit[0] == "any"))
            continue;

        if (ct->IpLimit.size() == 1 && ct->IpLimit[0] == "none")
            return TError(EError::Permission, "Parent container " + ct->Name + " forbid IP changing");

        if (!L3Only)
            return TError(EError::Permission, "Parent container " + ct->Name + " allows only L3 network");

        for (auto &dev: Devices) {
            for (auto &ip: dev.Ip) {
                bool allow = false;
                for (auto &str: ct->IpLimit) {
                    TNlAddr mask;
                    if (mask.Parse(AF_UNSPEC, str) || mask.Family() != ip.Family())
                        continue;
                    if (mask.IsMatch(ip)) {
                        allow = true;
                        break;
                    }
                }
                if (!allow)
                    return TError(EError::Permission, "Parent container " +
                            ct->Name + " forbid address: " + ip.Format());
            }
        }
    }

    return OK;
}

TError TNetEnv::ParseIp(TMultiTuple &ip_settings) {
    TError error;
    TNlAddr ip;

    for (auto &settings : ip_settings) {
        if (settings.size() != 2)
            return TError(EError::InvalidValue, "Invalid ip format");
        error = ip.Parse(AF_UNSPEC, settings[1]);
        if (error)
            return error;
        for (auto &dev: Devices)
            if (dev.Name == settings[0])
                dev.Ip.push_back(ip);
    }
    return OK;
}

void TNetEnv::FormatIp(TMultiTuple &ip_settings) {
    ip_settings.clear();
    for (auto &dev: Devices)
        for (auto &ip: dev.Ip)
            ip_settings.push_back({ dev.Name , ip.Format() });
}

TError TNetEnv::ParseGw(TMultiTuple &gw_settings) {
    TError error;
    TNlAddr ip;

    for (auto &settings : gw_settings) {
        if (settings.size() != 2)
            return TError(EError::InvalidValue, "Invalid gateway format");
        error = ip.Parse(AF_UNSPEC, settings[1]);
        if (error)
            return error;
        for (auto &dev: Devices)
            if (dev.Name == settings[0])
                dev.Gw = ip;
    }
    return OK;
}

TError TNetEnv::ConfigureL3(TNetDeviceConfig &dev) {
    auto lock = HostNetwork->LockNet();
    std::string peerName = HostNetwork->NewDeviceName("L3-");
    auto parentNl = HostNetwork->GetNl();
    auto Nl = Net->GetNl();
    TNlLink peer(parentNl, peerName);
    TNlAddr gate4, gate6;
    TError error;

    if (dev.Mode == "NAT" && dev.Ip.empty()) {
        error = HostNetwork->GetNatAddress(dev.Ip);
        if (error)
            return error;
        SaveIp = true;
    }

    error = HostNetwork->GetGateAddress(dev.Ip, gate4, gate6, dev.Mtu, dev.Group);
    if (error)
        return error;

    std::string ipStr;
    for (auto &ip : dev.Ip) {
        if (ip.Family() == AF_INET && gate4.IsEmpty())
            return TError(EError::InvalidValue, "Ipv4 gateway not found");
        if (ip.Family() == AF_INET6 && gate6.IsEmpty())
            return TError(EError::InvalidValue, "Ipv6 gateway not found");
        ipStr += fmt::format("{}={} ", ip.Family() == AF_INET ? "ip4" : "ip6", ip.Format());
    }

    L_NET("Setup L3 device {} peer={} mtu={} group={} master={} {}gw4={} gw6={}",
            dev.Name, peerName, dev.Mtu, TNetwork::DeviceGroupName(dev.Group),
            dev.Master, ipStr, gate4.Format(), gate6.Format());

    error = peer.AddVeth(dev.Name, "", dev.Mtu, VirtualDeviceGroup, NetNs.GetFd());
    if (error)
        return error;

    TNlLink link(Nl, dev.Name);
    error = link.Load();
    if (error)
        return error;

    error = link.SetGroup(VirtualDeviceGroup);
    if (error)
        return error;

    error = link.Up();
    if (error)
        return error;

    auto peerAddr = peer.GetAddr();

    if (!gate4.IsEmpty()) {
        error = Nl->PermanentNeighbour(link.GetIndex(), gate4, peerAddr, true);
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
        error = Nl->PermanentNeighbour(link.GetIndex(), gate6, peerAddr, true);
        if (error)
            return error;
        error = link.AddDirectRoute(gate6);
        if (error)
            return error;
        error = link.SetDefaultGw(gate6);
        if (error)
            return error;
    }

    for (auto &ip: dev.Ip) {
        error = peer.AddDirectRoute(ip);
        if (error)
            return error;
    }

    if (dev.Mode != "NAT") {
        error = HostNetwork->AddProxyNeightbour(dev.Ip, dev.Master);
        if (error)
            return error;
    }

    return OK;
}

TError TNetEnv::CreateTap(TNetDeviceConfig &dev) {
    TNlAddr gate4, gate6;
    TError error;
    int group;

    if (Net != HostNetwork)
        return TError(EError::NotSupported, "Tap only for host network");

    error = Net->GetGateAddress(dev.Ip, gate4, gate6, dev.Mtu, group);
    if (error)
        return error;

    for (auto &addr : dev.Ip) {
        if (addr.Family() == AF_INET6 && gate6.IsEmpty())
            return TError(EError::InvalidValue, "Ipv6 gateway not found");
        if (addr.Family() == AF_INET && gate4.IsEmpty())
            return TError(EError::InvalidValue, "Ipv4 gateway not found");
    }

    TFile tun;
    error = tun.OpenReadWrite("/dev/net/tun");
    if (error)
        return error;

    if (dev.Name.size() >= IFNAMSIZ)
        return TError(EError::InvalidValue, "tun name too long, max {}", IFNAMSIZ-1);

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(struct ifreq));
    strncpy(ifr.ifr_name, dev.Name.c_str(), IFNAMSIZ - 1);
    ifr.ifr_flags = IFF_TAP;

    if (ioctl(tun.Fd, TUNSETIFF, &ifr))
        return TError::System("Cannot add tap device");

    if (ioctl(tun.Fd, TUNSETOWNER, dev.Tap.Uid))
        return TError::System("Cannot set tap owner uid");

    if (ioctl(tun.Fd, TUNSETGROUP, dev.Tap.Gid))
        return TError::System("Cannot set tap owner gid");

    if (ioctl(tun.Fd, TUNSETPERSIST, 1))
        return TError::System("Cannot set tap persist");

    tun.Close();

    auto Nl = Net->GetNl();

    TNlLink tapdev(Nl, dev.Name);

    error = tapdev.Load();
    if (error)
        return error;

    error = tapdev.Up();
    if (error)
        return error;

    if (dev.Mtu > 0) {
        error = tapdev.SetMtu(dev.Mtu);
        if (error)
            return error;
    }

    if (VirtualDeviceGroup) {
        error = tapdev.SetGroup(VirtualDeviceGroup);
        if (error)
            return error;
    }

    if (!dev.Mac.empty()) {
        error = tapdev.SetMacAddr(dev.Mac);
        if (error)
            return error;
    }

    for (auto &ip: dev.Ip) {
        error = tapdev.AddDirectRoute(ip);
        if (error)
            return error;
    }

    error = Net->AddProxyNeightbour(dev.Ip, dev.Master);
    if (error)
        return error;

    if (!gate6.IsEmpty()) {
        error = Nl->ProxyNeighbour(tapdev.GetIndex(), gate6, true);
        if (error)
            return error;
    }

    if (!gate4.IsEmpty()) {
        error = Nl->ProxyNeighbour(tapdev.GetIndex(), gate4, true);
        if (error)
            return error;
    }

    Net->SetDeviceOwner(dev.Name, Id);

    return OK;
}

TError TNetEnv::DestroyTap(TNetDeviceConfig &tap) {
    TNlLink tap_dev(Net->GetNl(), tap.Name);

    Net->DelProxyNeightbour(tap.Ip);

    TError error = tap_dev.Load();
    if (!error)
        error = tap_dev.Remove();

    Net->SetDeviceOwner(tap.Name, 0);

    return error;
}

TError TNetEnv::SetupInterfaces() {
    auto parent_lock = ParentNet->LockNet();
    auto source_nl = ParentNet->GetNl();
    auto target_nl = Net->GetNl();
    TError error;

    for (auto &dev : Devices) {
        if (dev.Type == "steal") {
            TNlLink link(source_nl, dev.Name);
            error = link.ChangeNs(dev.Name, NetNs.GetFd());
            if (error)
                return error;
        } else if (dev.Type == "ipvlan") {
            std::string master = ParentNet->MatchDevice(dev.Master);

            TNlLink link(source_nl, "piv" + std::to_string(GetTid()));
            error = link.AddIpVlan(master, dev.Mode, dev.Mtu);
            if (error)
                return error;

            error = link.ChangeNs(dev.Name, NetNs.GetFd());
            if (error) {
                (void)link.Remove();
                return error;
            }
        } else if (dev.Type == "macvlan") {
            std::string master = ParentNet->MatchDevice(dev.Master);

            TNlLink link(source_nl, "pmv" + std::to_string(GetTid()));
            error = link.AddMacVlan(master, dev.Mode, dev.Mac, dev.Mtu);
            if (error)
                return error;

            error = link.ChangeNs(dev.Name, NetNs.GetFd());
            if (error) {
                (void)link.Remove();
                return error;
            }
        } else if (dev.Type == "veth") {
            TNlLink peer(source_nl, ParentNet->NewDeviceName("portove-"));

            error = peer.AddVeth(dev.Name, dev.Mac, dev.Mtu,
                                 VirtualDeviceGroup, NetNs.GetFd());
            if (error)
                return error;

            if (!dev.Master.empty()) {
                TNlLink bridge(source_nl, dev.Master);
                error = bridge.Load();
                if (error)
                    return error;

                error = bridge.Enslave(peer.GetName());
                if (error)
                    return error;
            }
        }
    }

    parent_lock.unlock();

    for (auto &dev : Devices) {
        if (dev.Type == "L3") {
            error = ConfigureL3(dev);
            if (error)
                return error;
        } else if (dev.Type == "ipip6") {
            TNlLink link(target_nl, dev.Name);

            error = link.AddIp6Tnl(dev.Name, dev.IpIp6.Remote, dev.IpIp6.Local,
                                   IPPROTO_IPIP, dev.Mtu,
                                   config().network().ipip6_encap_limit(),
                                   config().network().ipip6_ttl());
            if (error)
                return error;
        }
    }

    TNlLink loopback(target_nl, "lo");
    error = loopback.Load();
    if (error)
        return error;
    error = loopback.Up();
    if (error)
        return error;

    Net->ManagedNamespace = true;

    error = Net->SyncDevices();
    if (error)
        return error;

    for (auto &dev: Devices) {
        int index = Net->DeviceIndex(dev.Name);

        if (!index && dev.Name == "ip6tnl0") {
            if (dev.Mtu < 0 && dev.Ip.empty())
                continue;
            struct ifreq ifr;
            strncpy(ifr.ifr_name, dev.Name.c_str(), sizeof(ifr.ifr_name)-1);
            if (!ioctl(nl_socket_get_fd(Net->GetSock()), SIOCGIFINDEX, &ifr))
                index = ifr.ifr_ifindex;
        }

        if (!index)
            return TError("Network device {} not found", dev.Name);

        TNlLink link(target_nl, dev.Name, index);
        error = link.Load();
        if (error)
            return error;

        if (dev.Mtu > 0 && dev.Mtu != link.GetMtu()) {
            error = link.SetMtu(dev.Mtu);
            if (error)
                return error;
        }

        if (NetUp || !dev.Ip.empty() || !dev.Gw.IsEmpty() || dev.Autoconf) {
            error = link.Up();
            if (error)
                return error;
        }

        bool DefaultRoute = false;
        for (auto &ip: dev.Ip) {
            error = link.AddAddress(ip.Addr);
            if (error)
                return error;
            DefaultRoute |= ip.IsHost();
        }

        if (!dev.Gw.IsEmpty()) {
            error = link.SetDefaultGw(dev.Gw);
            if (error)
                return error;
            DefaultRoute = false;
        }

        if (dev.Type == "ipip6" && DefaultRoute) {
            TNlAddr ip;
            ip.Parse(AF_INET, "default");
            error = link.AddDirectRoute(ip);
            if (error)
                return error;
        }
    }

    return OK;
}

TError TNetEnv::ApplySysctl() {
    TNamespaceFd curNs;
    TError error;

    error = curNs.Open("/proc/thread-self/ns/net");
    if (error)
        return error;

    error = NetNs.SetNs(CLONE_NEWNET);
    if (error)
        return error;

    for (const auto &it: config().container().net_sysctl()) {
        if (NetSysctl.count(it.key()))
            continue;
        error = SetSysctl(it.key(), it.val());
        if (error) {
            if (error.Errno == ENOENT)
                L_NET("Sysctl {} is not virtualized", it.key());
            else
                goto err;
        }
    }

    for (const auto &it: NetSysctl) {
        if (!TNetwork::NamespaceSysctl(it.first)) {
            error = TError(EError::Permission, "Sysctl " + it.first + " is not allowed");
            goto err;
        }
        error = SetSysctl(it.first, it.second);
        if (error)
            goto err;
    }

    for (auto &cmd: IpRoute) {
        error = RunCommand(cmd);
        if (error)
            goto err;
    }

    error = OK;
err:
    TError error2 = curNs.SetNs(CLONE_NEWNET);
    PORTO_ASSERT(!error2);

    return error;
}

TError TNetEnv::Parse(TContainer &ct) {
    TError error;

    Id = ct.Id;
    Name = ct.Name;
    Parent = ct.Parent;
    Hostname = ct.Hostname;
    NetUp = !ct.OsMode;
    TaskCred = ct.TaskCred;

    error = ParseNet(ct.NetProp);
    if (error)
        return error;

    error = ParseIp(ct.IpList);
    if (error)
        return error;

    error = ParseGw(ct.DefaultGw);
    if (error)
        return error;

    for (const auto &it: ct.Sysctl) {
        if (TNetwork::NetworkSysctl(it.first))
            NetSysctl[it.first] = it.second;
    }

    if (Parent)
        ParentNet = Parent->Net;

    Net = ct.Net;

    return OK;
}

TError TNetEnv::Open(TContainer &ct) {
    if (ct.Task.Pid)
        return TNetwork::Open("/proc/" + std::to_string(ct.Task.Pid) + "/ns/net", NetNs, Net);
    if (ct.Net == HostNetwork)
        return TNetwork::Open("/proc/thread-self/ns/net", NetNs, Net);
    return TError(EError::InvalidValue, "Cannot open netns: container not running");
}

TError TNetEnv::OpenNetwork() {
    TError error;

    if (NetIsolate && L3Only && config().network().l3_migration_hack() &&
            Devices.size() && Devices[0].Type == "L3" && Devices[0].Ip.size()) {
        auto lock = LockContainers();

        for (auto &it: Containers) {
            auto &ct = it.second;
            if (!ct->Net || ct->IpList.empty())
                continue;

            for (auto cfg: ct->IpList) {
                TNlAddr addr;
                if (cfg.size() == 2 && !addr.Parse(AF_UNSPEC, cfg[1]) &&
                        addr.IsMatch(Devices[0].Ip[0])) {
                    L_ACT("Reuse L3 addr {} network {}", addr.Format(), ct->Name);
                    lock.unlock();
                    return Open(*ct);
                }
            }
        }
    }

    if (!Parent) {
        error = TNetwork::Open("/proc/thread-self/ns/net", NetNs, Net);
        if (error)
            return error;

        Net->NetName = "host";
        HostNetwork = Net;

        error = Net->SyncDevices();
        if (error)
            return error;

        if (config().network().has_nat_first_ipv4())
            Net->NatBaseV4.Parse(AF_INET, config().network().nat_first_ipv4());
        if (config().network().has_nat_first_ipv6())
            Net->NatBaseV6.Parse(AF_INET6, config().network().nat_first_ipv6());
        if (config().network().has_nat_count())
            Net->NatBitmap.Resize(config().network().nat_count());

        NetThread = std::thread(&TNetwork::NetWatchdog);

        return OK;
    }

    if (NetInherit)
        return Open(*Parent);

    if (NetIsolate) {
        error = TNetwork::New(NetNs, Net);
        if (error)
            return error;

        auto lock = Net->LockNet();

        Net->NetName = Name;

        error = Net->SetupAddrLabel();
        if (error)
            return error;

        error = SetupInterfaces();
        if (error) {
            lock.unlock();
            Net->Destroy();
            return error;
        }

        error = ApplySysctl();
        if (error) {
            lock.unlock();
            Net->Destroy();
            return error;
        }

        return OK;
    }

    if (NetNsName != "") {
        error = TNetwork::Open("/var/run/netns/" + NetNsName, NetNs, Net);
        if (Net && Net->NetName.empty())
            Net->NetName = NetNsName;
        return error;
    }

    if (NetCtName != "") {
        if (!CL)
            return TError("No client for net container");

        std::shared_ptr<TContainer> target;
        auto lock = LockContainers();
        error = CL->ResolveContainer(NetCtName, target);
        if (error)
            return error;

        error = CL->CanControl(*target);
        if (error)
            return TError(error, "net container {}", NetCtName);

        return Open(*target);
    }

    return TError("Unknown network configuration");
}
