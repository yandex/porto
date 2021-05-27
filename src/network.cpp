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
#include "util/proc.hpp"
#include "util/string.hpp"
#include "util/crc32.hpp"
#include "util/thread.hpp"

extern "C" {
#include <unistd.h>
#include <fcntl.h>
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
#include <netlink/route/route.h>
#include <netlink/route/qdisc/htb.h>
}

static std::shared_ptr<TNetwork> HostNetwork;

std::mutex TNetwork::NetworksMutex;
std::mutex TNetwork::NetStateMutex;
int TNetwork::DefaultTos = 0;

std::unordered_map<ino_t, std::shared_ptr<TNetwork>> TNetwork::NetworksIndex;
std::shared_ptr<const std::list<std::shared_ptr<TNetwork>>> TNetwork::NetworksList = std::make_shared<const std::list<std::shared_ptr<TNetwork>>>();
std::atomic<int> TNetwork::GlobalStatGen;

static std::unique_ptr<std::thread> NetThread;
static std::condition_variable NetThreadCv;
static uint64_t NetWatchdogPeriod;
static uint64_t NetProxyNeighbourPeriod;
static uint64_t SockDiagPeriod;
static uint64_t SockDiagMaxFds;

static std::string ResolvConfCurrent;
static std::string ResolvConfPrev;
static uint64_t ResolvConfPeriod = 0;

static std::vector<std::string> UnmanagedDevices;
static std::vector<int> UnmanagedGroups;
static std::map<int, std::string> DeviceGroups;
static int VirtualDeviceGroup = 0;

static uint64_t CsWeight[NR_TC_CLASSES];
static uint64_t CsTotalWeight;
static uint64_t CsLimit[NR_TC_CLASSES];
static double CsMaxPercent[NR_TC_CLASSES];

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

static uint64_t UplinkSpeed;

static double TCP_RTO_VALUE = 0.004;

static TNLinkSockDiag SockDiag;

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

static std::list<std::string> NamespacedNetSysctls;

void InitNamespacedNetSysctls() {
    if (CompareVersions(config().linux_version(), "4.5") >= 0) {
        NamespacedNetSysctls.push_back("net.ipv4.tcp_l3mdev_accept");
    }
    if (CompareVersions(config().linux_version(), "4.7") >= 0) {
        NamespacedNetSysctls.push_back("net.ipv4.fib_multipath_use_neigh");
    }
    if (CompareVersions(config().linux_version(), "4.11") >= 0) {
        NamespacedNetSysctls.push_back("net.ipv4.tcp_max_tw_buckets");
        NamespacedNetSysctls.push_back("net.ipv4.udp_l3mdev_accept");
        NamespacedNetSysctls.push_back("net.ipv4.ip_unprivileged_port_start");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_max_syn_backlog");
    }
    if (CompareVersions(config().linux_version(), "4.12") >= 0) {
        NamespacedNetSysctls.push_back("net.ipv4.fib_multipath_hash_policy");
        NamespacedNetSysctls.push_back("net.ipv4.udp_early_demux");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_early_demux");
    }
    if (CompareVersions(config().linux_version(), "4.13") >= 0) {
        NamespacedNetSysctls.push_back("net.ipv4.tcp_sack");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_window_scaling");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_timestamps");
    }
    if (CompareVersions(config().linux_version(), "4.14") >= 0) {
        NamespacedNetSysctls.push_back("net.ipv6.flowlabel_reflect");
    }
    if (CompareVersions(config().linux_version(), "4.15") >= 0) {
        NamespacedNetSysctls.push_back("net.ipv4.tcp_abort_on_overflow");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_adv_win_scale");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_app_win");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_autocorking");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_challenge_ack_limit");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_dsack");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_early_retrans");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_fack");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_fastopen");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_fastopen_key");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_fastopen_blackhole_timeout_sec");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_frto");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_invalid_ratelimit");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_limit_output_bytes");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_max_reordering");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_min_rtt_wlen");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_min_tso_segs");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_moderate_rcvbuf");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_no_metrics_save");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_pacing_ca_ratio");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_pacing_ss_ratio");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_recovery");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_retrans_collapse");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_rfc1337");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_rmem");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_slow_start_after_idle");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_stdurg");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_thin_linear_timeouts");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_tso_win_divisor");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_wmem");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_workaround_signed_windows");
        NamespacedNetSysctls.push_back("net.ipv6.max_dst_opts_number");
        NamespacedNetSysctls.push_back("net.ipv6.max_dst_opts_length");
        NamespacedNetSysctls.push_back("net.ipv6.max_hbh_opts_number");
        NamespacedNetSysctls.push_back("net.ipv6.max_hbh_length");
    }
    if (CompareVersions(config().linux_version(), "4.17") >= 0) {
        NamespacedNetSysctls.push_back("net.ipv4.udp_wmem_min");
        NamespacedNetSysctls.push_back("net.ipv6.fib_multipath_hash_policy");
        NamespacedNetSysctls.push_back("net.ipv4.udp_rmem_min");
    }
    if (CompareVersions(config().linux_version(), "4.18") >= 0) {
        NamespacedNetSysctls.push_back("net.ipv4.tcp_comp_sack_delay_ns");
        NamespacedNetSysctls.push_back("net.ipv4.tcp_comp_sack_nr");
        NamespacedNetSysctls.push_back("net.ipv6.seg6_flowlabel");
    }
    if (CompareVersions(config().linux_version(), "4.19") >= 0) {
        NamespacedNetSysctls.push_back("net.ipv4.ip_forward_update_priority");
        NamespacedNetSysctls.push_back("net.ipv6.icmp.echo_ignore_all");
    }
}

bool TNetClass::IsDisabled() {
    return !config().network().enable_host_net_classes();
}

bool TNetwork::NetworkSysctl(const std::string &key) {
    return StringStartsWith(key, "net.");
}

bool TNetwork::NamespaceSysctl(const std::string &key) {
    for (auto &pair: NetSysctls) {
        if (pair.first == key)
            return true;
    }
    for (auto& namespacedNetSysctl: NamespacedNetSysctls) {
        if (namespacedNetSysctl == key)
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
    Qdisc = rtnl_link_get_qdisc(link) ?: "";
    Index = rtnl_link_get_ifindex(link);
    Link = rtnl_link_get_link(link);
    MTU = rtnl_link_get_mtu(link);
    TxQueues = rtnl_link_get_num_tx_queues(link);
    Group = rtnl_link_get_group(link);
    GroupName = TNetwork::DeviceGroupName(Group);

    Rate = NET_MAX_RATE;
    Ceil = NET_MAX_RATE;

    Managed = false;
    Uplink = false;
    Prepared = false;
    Missing = false;
}

uint64_t TNetDevice::GetConfig(const TUintMap &cfg, uint64_t def, int cs) const {
    if (cs >= 0) {
        auto it = cfg.find(fmt::format("{} CS{}", Name, cs));
        if (it == cfg.end())
            it = cfg.find(fmt::format("CS{}", cs));
        if (it != cfg.end())
            return it->second;
    }
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

std::string TNetDevice::GetConfig(const TStringMap &cfg, std::string def, int cs) const {
    if (cs >= 0) {
        auto it = cfg.find(fmt::format("{} CS{}", Name, cs));
        if (it == cfg.end())
            it = cfg.find(fmt::format("CS{}", cs));
        if (it != cfg.end())
            return it->second;
    }
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

    UplinkSpeed = config().network().default_uplink_speed_gb() * 1000000000ull / 8;

    NetWatchdogPeriod = config().network().watchdog_ms();

    SockDiagPeriod = config().network().sock_diag_update_interval_ms();
    SockDiagMaxFds = config().network().sock_diag_max_fds();

    NetProxyNeighbourPeriod = config().network().proxy_ndp_watchdog_ms();

    InitNamespacedNetSysctls();

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

    CsTotalWeight = 0;
    for (int cs = 0; cs < NR_TC_CLASSES; cs++) {
        auto name = FormatTos(cs);
        CsWeight[cs] = 1;
        CsLimit[cs] = 0;
        CsMaxPercent[cs] = 100;
        for (auto &it: config().network().dscp_class())
            if (it.name() == name) {
                CsWeight[cs] = it.weight();
                CsLimit[cs] = it.limit();
                CsMaxPercent[cs] = it.max_percent();
            }
        if (CsWeight[cs] < 1)
            CsWeight[cs] = 1;
        CsTotalWeight += CsWeight[cs];
        L_NET("DSCP {} weight = {}, limit = {}, max_percent = {}%", FormatTos(cs), CsWeight[cs], CsLimit[cs], CsMaxPercent[cs]);
    }
    if (config().network().has_default_tos())
        ParseTos(config().network().default_tos(), DefaultTos);
    L_NET("DSCP total weight = {}, default = {}", CsTotalWeight, FormatTos(DefaultTos));
}

std::string TNetwork::DeviceGroupName(int group) {
    auto it = DeviceGroups.find(group);
    if (it != DeviceGroups.end())
        return it->second;
    return std::to_string(group);
}

TError TNetwork::ParseTos(const std::string &str, int &tos) {
    if (str.size() != 3 || str[0] != 'C' || str[1] != 'S' || str[2] < '0' || str[2] > '7')
        return TError(EError::InvalidValue, "Invalud ToS: {}", str);
    tos = str[2] - '0';
    return OK;
}

std::string TNetwork::FormatTos(int tos) {
    return fmt::format("CS{}", tos);
}

TNetwork::TNetwork(bool host) : NetIsHost(host), NatBitmap(0, 0) {
    Nl = std::make_shared<TNl>();

    DefaultClass.BaseHandle = TC_HANDLE(ROOT_TC_MAJOR, 1);
    DefaultClass.MetaHandle = TC_HANDLE(ROOT_TC_MAJOR, META_TC_MINOR);
    DefaultClass.LeafHandle = TC_HANDLE(ROOT_TC_MAJOR, DEFAULT_TC_MINOR);
    DefaultClass.Fold = &DefaultClass;
    DefaultClass.Parent = nullptr;
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

TError TNetwork::GetSockets(const std::vector<pid_t> &pids, std::unordered_set<ino_t> &sockets) {
    uint64_t fdsCount = 0;
    sockets.clear();

    for (const auto &pid : pids) {
        TFile dir;

        auto error = dir.OpenDir("/proc/" + std::to_string(pid) + "/fd");
        if (error) {
            if (errno == ENOENT || errno == ESRCH)
                continue;
            return TError("Cannot open: {}", error);
        }

        uint64_t fdSize;
        error = GetFdSize(pid, fdSize);
        if (error) {
            if (errno == ENOENT || errno == ESRCH)
                continue;
            return error;
        }

        struct stat st;

        for (uint64_t i = 0; i < fdSize && fdsCount < SockDiagMaxFds; ++i) {
            error = dir.StatAt(TPath(std::to_string(i)), true, st);
            if (error) {
                if (errno == ENOENT || errno == ESRCH)
                    continue;
                return TError("Cannot fstatat:{}", error);
            }
            if (S_ISSOCK(st.st_mode))
                sockets.insert(st.st_ino);
            ++fdsCount;
        }

        /* Too many sockets in netns, break iteration */
        if (fdsCount >= SockDiagMaxFds)
            return OK;
    }
    return OK;
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

TError TNetwork::New(TNamespaceFd &netns, std::shared_ptr<TNetwork> &net, pid_t netnsPid) {
    TNamespaceFd curNs;
    TError error;

    error = curNs.Open("/proc/thread-self/ns/net");
    if (error)
        return error;

    if (netnsPid) {
        error = netns.Open(fmt::format("/proc/{}/ns/net", netnsPid));
        if (error)
            return error;
        error = netns.SetNs(CLONE_NEWNET);
        if (error)
            return error;
    } else {
        if (unshare(CLONE_NEWNET))
            return TError::System("unshare(CLONE_NEWNET)");

        error = netns.Open("/proc/thread-self/ns/net");
        if (error)
            return error;
    }

    net = std::make_shared<TNetwork>();

    error = net->Nl->Connect();
    if (error) {
        netns.Close();
        net = nullptr;
    }

    TError error2 = curNs.SetNs(CLONE_NEWNET);
    PORTO_ASSERT(!error2);

    auto networks_lock = LockNetworks();
    Statistics->NetworksCreated++;
    Register(net, netns.Inode());

    return error;
}

TError TNetwork::Open(const TPath &path, TNamespaceFd &netns,
                      std::shared_ptr<TNetwork> &net,
                      bool host) {
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

    net = std::make_shared<TNetwork>(host);

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
        TNlQdisc qdisc(dev.Index, TC_H_ROOT, TC_HANDLE(ROOT_TC_MAJOR, 0));
        error = qdisc.Delete(*Nl);
        if (error)
            L_NET("Cannot remove root qdisc: {}", error);
    }

    if (EnabledRxLimit) {
        TNlQdisc qdisc(HostPeerIndex, TC_H_ROOT, TC_HANDLE(ROOT_TC_MAJOR, 0));
        auto lock = HostNetwork->LockNet();
        error = qdisc.Delete(*HostNetwork->GetNl());
        if (error)
            L_NET("Cannot remove root qdisc for host dev: {}", error);
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

std::string TNetwork::GetDeviceQdisc(const TNetDevice &dev) {
    if (IsHost() && TNetClass::IsDisabled())
        return dev.TxQueues > 1 ? "mq" : dev.GetConfig(DefaultQdisc);
    return dev.GetConfig(DeviceQdisc);
}

void TNetwork::GetDeviceSpeed(TNetDevice &dev) const {
    TPath knob("/sys/class/net/" + dev.Name + "/speed");
    std::string text;
    uint64_t speed;

    if (ManagedNamespace || knob.ReadAll(text) ||
            StringToUint64(text, speed) || speed < 100) {
        dev.Ceil = NET_MAX_RATE;
        dev.Rate = NET_MAX_RATE;
    } else {
        dev.Ceil = speed * 125000; /* Mbit -> Bps */
        dev.Rate = speed * 125000;
    }
}

TError TNetwork::SetupPolice(TNetDevice &dev) {
    TError error;

    PORTO_LOCKED(NetMutex);
    PORTO_LOCKED(NetStateMutex);

    if (!RootClass || IsHost())
        return OK;

    /*  DO NOT REMOVE https://st.yandex-team.ru/PORTO-809
    auto rate = dev.GetConfig(RootClass->RxLimit);

    TNlPoliceFilter police(dev.Index, TC_H_INGRESS);
    police.Mtu = 65536; // maximum GRO skb
    police.Rate = rate;
    police.Burst = dev.GetConfig(IngressBurst, std::max(rate / 10, police.Mtu * 10ul));
    (void)police.Delete(*Nl);

    TNlQdisc qdisc(dev.Index, TC_H_INGRESS, TC_H_MAJ(TC_H_INGRESS));
    qdisc.Kind = "ingress";
    (void)qdisc.Delete(*Nl);

    if (rate) {
        error = qdisc.Create(*Nl);
        if (error && error.Errno != ENODEV && error.Errno != ENOENT) {
            L_WRN("Cannot create ingress qdisc: {}", error);
            return error;
        }

        error = police.Create(*Nl);
        if (error && error.Errno != ENODEV && error.Errno != ENOENT) {
            L_WRN("Can't create ingress police filter: {}", error);
            return error;
        }
    }
    */

    auto rate = dev.GetConfig(RootClass->TxLimit);

    /* without clsact police filter requires classful qdisc,
     * so let's install trivial hfsc instead */

    TNlQdisc qdisc(dev.Index, TC_H_ROOT, TC_HANDLE(ROOT_TC_MAJOR, 0));
    qdisc.Kind = "hfsc";
    qdisc.Default = TC_HANDLE(ROOT_TC_MAJOR, 1);
    qdisc.Quantum = 10;
    (void)qdisc.Delete(*Nl);

    TNlClass cls(dev.Index, TC_HANDLE(ROOT_TC_MAJOR, 0), TC_HANDLE(ROOT_TC_MAJOR, 1));
    cls.Kind = qdisc.Kind;
    cls.Index = dev.Index;
    cls.Rate = cls.Ceil = rate;
    cls.RateBurst = cls.CeilBurst = dev.MTU * 10;

    TNlQdisc leaf(dev.Index, TC_HANDLE(ROOT_TC_MAJOR, 1), TC_HANDLE(2, 0));
    leaf.Kind = dev.GetConfig(ContainerQdisc, "pfifo");
    leaf.Limit = dev.GetConfig(ContainerQdiscLimit, 1000);
    leaf.Quantum = dev.GetConfig(ContainerQdiscQuantum, dev.MTU * 10);

    if (rate) {
        error = qdisc.Create(*Nl);
        if (error && error.Errno != ENODEV && error.Errno != ENOENT) {
            L_WRN("Cannot create egress qdisc: {}", error);
            return error;
        }

        error = cls.Create(*Nl);
        if (error) {
            L_ERR("Cannot create egress class: {}", error);
            return error;
        }

        error = leaf.Create(*Nl);
        if (error) {
            L_ERR("Cannot create egress leaf qdisc: {}", error);
            return error;
        }
    }

    return OK;
}

TError TNetwork::SetupRxLimit(TNetDevice &dev, std::unique_lock<std::mutex> &statLock) {
    // set rx limit like tx on host peer
    TError error;

    PORTO_LOCKED(NetMutex);
    PORTO_LOCKED(NetStateMutex);

    if (!RootClass || IsHost() || HostPeerIndex < 0)
        return OK;

    auto rate = dev.GetConfig(RootClass->RxLimit);

    if (!rate) {
        EnabledRxLimit = false;
        return OK;
    }

    // take host network lock first, to avoid dead lock
    statLock.unlock();
    auto lockNet = HostNetwork->LockNet();
    statLock.lock();

    auto &hostNl = *HostNetwork->GetNl();

    TNlQdisc qdisc(HostPeerIndex, TC_H_ROOT, TC_HANDLE(ROOT_TC_MAJOR, 0));
    qdisc.Index = HostPeerIndex;
    qdisc.Kind = "hfsc";
    qdisc.Default = TC_HANDLE(ROOT_TC_MAJOR, 1);
    (void)qdisc.Delete(hostNl);

    TNlClass cls(HostPeerIndex, TC_HANDLE(ROOT_TC_MAJOR, 0), TC_HANDLE(ROOT_TC_MAJOR, 1));
    cls.Kind = qdisc.Kind;
    cls.Rate = cls.Ceil = rate;
    cls.Burst = UplinkSpeed;
    cls.BurstDuration = 10000; // 10ms

    TNlQdisc leaf(HostPeerIndex, TC_HANDLE(ROOT_TC_MAJOR, 1), TC_HANDLE(2, 0));
    leaf.Kind = dev.GetConfig(ContainerQdisc, "pfifo");
    leaf.Quantum = dev.GetConfig(ContainerQdiscQuantum, dev.MTU * 10);

    // https://st.yandex-team.ru/PORTO-809
    leaf.MemoryLimit = std::max(std::floor(TCP_RTO_VALUE * rate), 64000.);

    error = qdisc.Create(hostNl);
    if (error && error.Errno != ENODEV && error.Errno != ENOENT) {
        L_ERR("Cannot create egress qdisc for host peer: {}", error);
        return error;
    }

    error = cls.Create(hostNl);
    if (error) {
        L_ERR("Cannot create egress class for host peer: {}", error);
        return error;
    }

    error = leaf.Create(hostNl);
    if (error) {
        L_ERR("Cannot create egress leaf qdisc for host peer: {}", error);
        return error;
    }

    EnabledRxLimit = true;
    return OK;
}

void TNetwork::InitClasslessQdisc(TNetDevice &dev, TNlQdisc &qdisc) {
    qdisc.Kind = dev.GetConfig(DefaultQdisc, "pfifo");
    qdisc.Limit = dev.GetConfig(DefaultQdiscLimit, 1000);
    qdisc.Quantum = dev.GetConfig(DefaultQdiscQuantum, dev.MTU * 2);
}

TError TNetwork::SetupMQ(TNetDevice &dev) {
    TError error;

    for (int i = 1; i <= dev.TxQueues; ++i) {
        TNlQdisc leaf(dev.Index, TC_HANDLE(ROOT_TC_MAJOR, i), TC_HANDLE(ROOT_TC_MAJOR + i, 0));
        InitClasslessQdisc(dev, leaf);
        error = leaf.Create(*Nl);
        if (error) {
            L_ERR("Cannot create leaf qdisc: {}", error);
            return error;
        }
    }

    return OK;
}

TError TNetwork::SetupQueue(TNetDevice &dev, bool force) {
    TError error;

    PORTO_LOCKED(NetMutex);
    PORTO_LOCKED(NetStateMutex);

    //
    // 1:0 qdisc
    //  |
    // 1:1 / class
    //  |
    //  +- 1:8..F DSCP CS0..CS7 class
    //      |
    //      +- 1:0x10 + CSn Host CSn class
    //      |   |
    //      |   +- 1x10 + CSn:0 CSn Host leaf qdisc
    //      |
    //      +- 1:0x20 + CSn Fallback CSn class
    //      |   |
    //      |   +- 1x20 + CSn:0 CSn Fallback leaf qdisc
    //      |
    //      +-1:Slot + 8 + CSn slot CSn class
    //         |
    //         +- 1:Slot + CSn default leaf for slot CSn class
    //         |   |
    //         |   +- Slot + CSn:0 slot CSn default leaf qdisc
    //         |
    //         +- 1:Container + CSn container CSn class
    //             |
    //             +- Container + CSn:0 container CSn leaf qdisc
    //

    L_NET("Setup queue for network {} device {}:{}", NetName, dev.Index, dev.Name);

    TNlQdisc qdisc(dev.Index, TC_H_ROOT, TC_HANDLE(ROOT_TC_MAJOR, 0));
    qdisc.Kind = GetDeviceQdisc(dev);
    qdisc.Default = TC_HANDLE(ROOT_TC_MAJOR, DEFAULT_TC_MINOR);
    qdisc.Quantum = 10;

    if (IsHost() && TNetClass::IsDisabled() && dev.TxQueues == 1)
        InitClasslessQdisc(dev, qdisc);

    if (force || !qdisc.Check(*Nl)) {
        (void)qdisc.Delete(*Nl);
        error = qdisc.Create(*Nl);
        if (error) {
            L_ERR("Cannot create root qdisc: {}", error);
            return error;
        }
        dev.Qdisc = qdisc.Kind;
    }

    if (IsHost() && TNetClass::IsDisabled())
        return dev.TxQueues > 1 ? SetupMQ(dev) : OK;

    TNlClass cls;

    cls.Kind = dev.GetConfig(DeviceQdisc);
    cls.Index = dev.Index;
    cls.Parent = TC_HANDLE(ROOT_TC_MAJOR, 0);
    cls.Handle = TC_HANDLE(ROOT_TC_MAJOR, 1);
    cls.Rate = dev.GetConfig(DeviceRate, dev.Rate);
    cls.Ceil = dev.GetConfig(DeviceCeil, 0);
    cls.Quantum = dev.GetConfig(DeviceQuantum, dev.MTU * 10);
    cls.RateBurst = dev.GetConfig(DeviceRateBurst, dev.MTU * 10);
    cls.CeilBurst = dev.GetConfig(DeviceCeilBurst, dev.MTU * 10);

    error = cls.Create(*Nl);
    if (error) {
        L_ERR("Can't create root tclass: {}", error);
        return error;
    }

    for (int cs = 0; cs < NR_TC_CLASSES; cs++) {
        error = SetupClass(dev, DefaultClass, cs);
        if (error) {
            L_ERR("Canot setup default tclass: {}", error);
            return error;
        }

        if (IsHost() && RootContainer) {
            auto cs_name = fmt::format("{} CS{}", dev.Name, cs);
            RootContainer->NetClass.TxRate[cs_name] = cls.Rate;
            if (cls.Ceil)
                RootContainer->NetClass.TxLimit[cs_name] = cls.Ceil;
        }
    }

    TNlCgFilter filter(dev.Index, TC_HANDLE(ROOT_TC_MAJOR, 0), 1);
    (void)filter.Delete(*Nl);

    /*
     * Without cgroup priority setup classification by cgroup classid.
     *
     * This does not work for traffic crossing net-ns in veth,
     * it will flow through fallback classes.
     */
    if (!NetclsSubsystem.HasPriority) {
        error = filter.Create(*Nl);
        if (error) {
            L_ERR("Can't create tc filter: {}", error);
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

TError TNetwork::SyncDevices() {
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

        if (DeviceOwners.count(dev.Name))
            dev.Owner = DeviceOwners.at(dev.Name);

        if (!ManagedNamespace && !dev.Managed) {
            for (auto &pattern: config().network().managed_device()) {
                if (StringMatch(dev.Name, pattern))
                    dev.Managed = dev.Uplink = true;
            }
        }

        if (dev.Type == "veth") {
            /* Ignore our veth pairs */
            if (!ManagedNamespace &&
                    (StringStartsWith(dev.Name, "portove-") ||
                     StringStartsWith(dev.Name, "L3-")))
                continue;

            if (ManagedNamespace)
                dev.Uplink = true;
        } else if (dev.Type == "macvlan" || dev.Type == "ipvlan") {
            if (ManagedNamespace)
                dev.Uplink = true;
        } else if (dev.Type == "tun") {
            /* Ignore TUN/TAP without known owners */
            if (!dev.Owner && !dev.Managed)
                continue;
        } else if (ManagedNamespace) {
            /* No qdisc in containers except police at uplink */
        } else if (dev.Type == "dummy") {
            /* Do not care */
        } else if (dev.Name == "ip6tnl0") {
            /* Fallback tunnel for RX only */
        } else if (dev.Type == "ip6tnl") {
            /* TX goes via uplink */
            if (config().network().managed_ip6tnl())
                dev.Managed = true;
        } else if (dev.Type == "vlan") {
            /* TX goes via uplink */
            if (config().network().managed_vlan())
                dev.Managed = true;
        } else {
            dev.Managed = true;

            for (auto &pattern: UnmanagedDevices)
                if (StringMatch(dev.Name, pattern))
                    dev.Managed = false;

            if (std::find(UnmanagedGroups.begin(),
                          UnmanagedGroups.end(), dev.Group) != UnmanagedGroups.end())
                dev.Managed = false;

            dev.Uplink = true;
        }

        GetDeviceSpeed(dev);

        dev.DeviceStat.RxBytes = rtnl_link_get_stat(link, RTNL_LINK_RX_BYTES);
        dev.DeviceStat.RxPackets = rtnl_link_get_stat(link, RTNL_LINK_RX_PACKETS);
        dev.DeviceStat.RxDrops = rtnl_link_get_stat(link, RTNL_LINK_RX_DROPPED);
        dev.DeviceStat.RxOverruns = rtnl_link_get_stat(link, RTNL_LINK_RX_OVER_ERR) +
                              rtnl_link_get_stat(link, RTNL_LINK_RX_ERRORS);

        dev.DeviceStat.TxBytes = rtnl_link_get_stat(link, RTNL_LINK_TX_BYTES);
        dev.DeviceStat.TxPackets = rtnl_link_get_stat(link, RTNL_LINK_TX_PACKETS);
        dev.DeviceStat.TxDrops = rtnl_link_get_stat(link, RTNL_LINK_TX_DROPPED);
        dev.DeviceStat.TxOverruns = rtnl_link_get_stat(link, RTNL_LINK_TX_ERRORS);

        if (dev.Type == "L3" || dev.Type == "veth") {
            TNlQdisc qdisc(dev.Index, TC_H_ROOT, TC_HANDLE(ROOT_TC_MAJOR, 0));
            auto qdiscStat = qdisc.Stat(*Nl);
            dev.DeviceStat.TxDrops += qdiscStat.Drops;
            dev.DeviceStat.TxOverruns += qdiscStat.Overruns;

            if (EnabledRxLimit) {
                TNlQdisc peerQdisc(HostPeerIndex, TC_H_ROOT, TC_HANDLE(ROOT_TC_MAJOR, 0));
                auto lock = HostNetwork->LockNet();
                qdiscStat = peerQdisc.Stat(*HostNetwork->GetNl());
                dev.DeviceStat.RxDrops += qdiscStat.Drops;
            }
            /* DO NOT REMOVE https://st.yandex-team.ru/PORTO-809
            TNlQdisc qdiscIngress(dev.Index, TC_H_INGRESS, TC_H_MAJ(TC_H_INGRESS));
            qdiscStat = qdiscIngress.Stat(*Nl);
            dev.DeviceStat.RxDrops += qdiscStat.Drops;
            */
        }

        auto net_state_lock = LockNetState();

        bool found = false;
        for (auto &d: Devices) {
            if (d.Name != dev.Name || d.Index != dev.Index)
                continue;

            dev.Prepared = d.Prepared;

            if (!d.Managed) {
                dev.Prepared = true;
            } else if (d.Qdisc != GetDeviceQdisc(dev)) {
                L_NET("Missing network {} qdisc at {}:{}", NetName, d.Index, d.Name);
                dev.Prepared = false;
            } else if (d.Rate != dev.Rate || d.Ceil != dev.Ceil) {
                L_NET("Speed changed {}Mbps to {}Mbps in {} at {}:{}",
                      d.Ceil / 125000, dev.Ceil / 125000, NetName, d.Index, d.Name);
                dev.Prepared = false;
            }

            d = dev;
            found = true;
            break;
        }
        if (!found) {
            L_NET("New network {} {}managed device {}:{} type={} qdisc={} group={} {} mtu={} speed={}Mbps {}iB/s",
                    NetName, dev.Managed ? "" : "un",
                    dev.Index, dev.Name, dev.Type, dev.Qdisc, dev.GroupName,
                    dev.Uplink ? "uplink" : "", dev.MTU,
                    dev.Ceil / 125000, StringFormatSize(dev.Ceil));
            Devices.push_back(dev);
        }

        if (!dev.Prepared && IsHost() && !TNetClass::IsDisabled()) {
            RootContainer->NetClass.TxLimit[dev.Name] = dev.GetConfig(DeviceCeil, dev.Ceil);
            RootContainer->NetClass.RxLimit[dev.Name] = dev.GetConfig(DeviceCeil, dev.Ceil);
        }

        if (!dev.Prepared)
            StartRepair();
    }

    nl_cache_free(cache);

    auto net_state_lock = LockNetState();

    for (auto dev = Devices.begin(); dev != Devices.end(); ) {
        if (dev->Missing) {

            L_NET("Forget network {} device {}:{}", NetName, dev->Index, dev->Name);

            if (IsHost() && !TNetClass::IsDisabled()) {
                RootContainer->NetClass.TxLimit.erase(dev->Name);
                RootContainer->NetClass.RxLimit.erase(dev->Name);
                for (int cs = 0; cs < NR_TC_CLASSES; cs++) {
                    auto cs_name = fmt::format("{} CS{}", dev->Name, cs);
                    RootContainer->NetClass.TxRate.erase(cs_name);
                    RootContainer->NetClass.TxLimit.erase(cs_name);
                    cs_name = "Leaf " + cs_name;
                    RootContainer->NetClass.TxRate.erase(cs_name);
                    RootContainer->NetClass.TxLimit.erase(cs_name);
                }

                for (auto cls: NetClasses) {
                    cls->ClassStat.erase(dev->Name);
                    for (int cs = 0; cs < NR_TC_CLASSES; cs++) {
                        auto cs_name = fmt::format("{} CS{}", dev->Name, cs);
                        cls->ClassStat[fmt::format("Saved CS{}", cs)] += cls->ClassStat[cs_name];
                        cls->ClassStat.erase(cs_name);
                    }
                }
            }
            dev = Devices.erase(dev);
        } else
            dev++;
    }

    DeviceStat.clear();
    for (auto &dev: Devices) {
        DeviceStat[dev.Name] = dev.DeviceStat;
        DeviceStat["group " + dev.GroupName] += dev.DeviceStat;
        if (dev.Uplink)
            DeviceStat["Uplink"] += dev.DeviceStat;
    }

    return OK;
}

TError TNetwork::GetL3Gate(TNetDeviceConfig &dev) {
    struct nl_cache *cache, *lcache;
    int default_mtu = -1;
    TError error;
    int ret;

    /* Skip autodetect if MTU and required gateways are set */
    bool skip = dev.Mtu > 0;
    for (auto &ip: dev.Ip) {
        if (ip.Family() == AF_INET ? dev.Gate4.IsEmpty() : dev.Gate6.IsEmpty()) {
            skip = false;
            break;
        }
    }
    if (skip)
        return OK;

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

        if (!local || rtnl_addr_get_scope(addr) == RT_SCOPE_HOST ||
                rtnl_addr_get_scope(addr) == RT_SCOPE_LINK)
            continue;

        for (auto &ip: dev.Ip) {
            if (ip.Family() != nl_addr_get_family(local))
                continue;

            if (dev.Type == "L3" && dev.Mode == "NAT") {
                /* match anything */
            } else if (nl_addr_cmp_prefix(local, ip.Addr))
                continue;

            if (dev.Gate4.IsEmpty() && ip.Family() == AF_INET) {
                dev.Gate4 = TNlAddr(local);
                nl_addr_set_prefixlen(dev.Gate4.Addr, 32);
            }

            if (dev.Gate6.IsEmpty() && ip.Family() == AF_INET6) {
                dev.Gate6 = TNlAddr(local);
                nl_addr_set_prefixlen(dev.Gate6.Addr, 128);
            }

            auto link = rtnl_link_get(lcache, rtnl_addr_get_ifindex(addr));
            if (link) {
                default_mtu = std::max(default_mtu, (int)rtnl_link_get_mtu(link));
                rtnl_link_put(link);
            }
        }
    }

    for (auto &ip: dev.Ip) {
        if (ip.Family() == AF_INET ? dev.Gate4.IsEmpty() : dev.Gate6.IsEmpty()) {
            error = TError(EError::InvalidNetworkAddress, "Ip {} have no matching address in host", ip.Format());
            Statistics->FailInvalidNetaddr++;
            break;
        }
    }

    nl_cache_free(lcache);
    nl_cache_free(cache);

    if (dev.Mtu <= 0)
        dev.Mtu = default_mtu;

    return error;
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
        std::vector<TNlAddr> addrs;

        for (auto &ip: ips) {
            error = ip.GetRange(addrs, config().network().proxy_ndp_max_range());
            if (error)
                return error;
        }

        error = SetupProxyNeighbour(addrs, master);
        if (error)
            return error;

        for (auto ip: addrs)
            Neighbours.emplace_back(TNetProxyNeighbour{ip, master});
    }
    return error;
}

void TNetwork::DelProxyNeightbour(const std::vector<TNlAddr> &ips) {
    TError error;
    if (config().network().proxy_ndp()) {
        std::vector<TNlAddr> addrs;

        for (auto &ip: ips) {
            if (ip.GetRange(addrs, config().network().proxy_ndp_max_range()))
                addrs.emplace_back(ip);
        }

        for (auto &ip: addrs) {
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

TError TNetwork::SetupClass(TNetDevice &dev, TNetClass &cfg, int cs, bool safe) {
    TError error;

    PORTO_LOCKED(NetMutex);
    PORTO_LOCKED(NetStateMutex);

    if (cfg.LeafHandle == cfg.BaseHandle)
        return OK; /* Fold */

    TNlClass cls(dev.Index, cfg.BaseHandle + cs, cfg.MetaHandle + cs);

    if (cfg.BaseHandle == TC_HANDLE(ROOT_TC_MAJOR, 1))
        cls.Parent = cfg.BaseHandle;

    cls.Kind = GetDeviceQdisc(dev);
    cls.Rate = dev.GetConfig(cfg.TxRate, 0, cs);
    cls.Ceil = dev.GetConfig(cfg.TxLimit, 0, cs);
    cls.Quantum = dev.GetConfig(DeviceQuantum, dev.MTU * 10, cs);
    cls.RateBurst = dev.GetConfig(DeviceRateBurst, dev.MTU * 10, cs);
    cls.CeilBurst = dev.GetConfig(DeviceCeilBurst, dev.MTU * 10, cs);

    if (cfg.BaseHandle == TC_HANDLE(ROOT_TC_MAJOR, 1)) {
        cls.Rate = (double)dev.GetConfig(DeviceRate, dev.Rate) *
                    CsWeight[cs] / CsTotalWeight;
        cls.Ceil = 0;
        if (CsMaxPercent[cs] < 100)
            cls.Ceil = dev.GetConfig(DeviceCeil, dev.Ceil) * CsMaxPercent[cs] / 100;
        if (CsLimit[cs] && (!cls.Ceil || CsLimit[cs] < cls.Ceil))
            cls.Ceil = CsLimit[cs];
    } else
        cls.defRate = dev.GetConfig(ContainerRate, 0, cs);

    if (cfg.MetaHandle != cfg.BaseHandle) {
        L_NET_VERBOSE("Setup CS{} meta class {:x} {} {}:{}", cs, cls.Handle, NetName, dev.Index, dev.Name);
        error = cls.Create(*Nl, safe);
        if (error) {
            (void)cls.Delete(*Nl);
            error = cls.Create(*Nl, safe);
        }
        if (error)
            return TError(error, "tc class");
    }

    TNlQdisc ctq(dev.Index, cfg.LeafHandle + cs, TC_HANDLE(TC_H_MIN(cfg.LeafHandle + cs), 0));

    cls.Parent = cfg.MetaHandle + cs;
    cls.Handle = cfg.LeafHandle + cs;

    if (cfg.LeafHandle == TC_HANDLE(ROOT_TC_MAJOR, ROOT_TC_MINOR) ||
            cfg.LeafHandle == TC_HANDLE(ROOT_TC_MAJOR, DEFAULT_TC_MINOR)) {
        cls.Rate = dev.GetConfig(DefaultClassRate, 0, cs);
        cls.Ceil = dev.GetConfig(DefaultClassCeil, 0, cs);
        cls.defRate = cls.Rate;

        ctq.Kind = dev.GetConfig(DefaultQdisc, "pfifo", cs);
        ctq.Limit = dev.GetConfig(DefaultQdiscLimit, 1000, cs);
        ctq.Quantum = dev.GetConfig(DefaultQdiscQuantum, dev.MTU * 10, cs);
    } else if (cls.Ceil == 1) {
        ctq.Kind = "blackhole";
    } else {
        ctq.Kind = dev.GetConfig(ContainerQdisc, "pfifo", cs);
        ctq.Limit = dev.GetConfig(ContainerQdiscLimit, 1000, cs);
        ctq.Quantum = dev.GetConfig(ContainerQdiscQuantum, dev.MTU * 10, cs);
    }

    L_NET_VERBOSE("Setup CS{} leaf class {:x} {} {}:{}", cs, cls.Handle, NetName, dev.Index, dev.Name);

    error = cls.Create(*Nl, safe);
    if (error)
        return TError(error, "leaf tc class");

    error = ctq.Create(*Nl);
    if (error) {
        (void)ctq.Delete(*Nl);
        error = ctq.Create(*Nl);
    }
    if (error)
        return TError(error, "leaf tc qdisc");

    if (IsHost() && RootContainer && cfg.LeafHandle == TC_HANDLE(ROOT_TC_MAJOR, ROOT_TC_MINOR)) {
        auto cs_name = fmt::format("Leaf {} CS{}", dev.Name, cs);
        RootContainer->NetClass.TxRate[cs_name] = cls.Rate;
        if (cls.Ceil)
            RootContainer->NetClass.TxLimit[cs_name] = cls.Ceil;
    }

    return OK;
}

TError TNetwork::DeleteClass(TNetDevice &dev, TNetClass &cfg, int cs) {
    TError error;

    PORTO_LOCKED(NetMutex);

    if (cfg.LeafHandle == TC_HANDLE(ROOT_TC_MAJOR, ROOT_TC_MINOR))
        return OK; /* Host */

    if (cfg.LeafHandle == cfg.BaseHandle)
        return OK; /* Fold */

    TNlQdisc ctq(dev.Index, cfg.LeafHandle + cs,
                 TC_HANDLE(TC_H_MIN(cfg.LeafHandle + cs), 0));
    (void)ctq.Delete(*Nl);

    TNlClass cls(dev.Index, TC_H_UNSPEC, cfg.LeafHandle + cs);
    (void)cls.Delete(*Nl);

    if (cfg.MetaHandle != cfg.BaseHandle) {
        TNlClass cls(dev.Index, TC_H_UNSPEC, cfg.MetaHandle + cs);
        error = cls.Delete(*Nl);
        if (error && error.Errno != ENODEV && error.Errno != ENOENT)
            return TError(error, "cannot remove class");
    }

    return OK;
}

TError TNetwork::TrySetupClasses(TNetClass &cls, bool safe) {
    auto net_lock = LockNet();
    auto state_lock = LockNetState();
    TError error;

    for (auto &dev: Devices) {
        if (!dev.Managed || !dev.Prepared)
            continue;

        for (int cs = 0; cs < NR_TC_CLASSES; cs++) {
            error = SetupClass(dev, cls, cs, safe);
            if (error)
                return error;
        }
    }

    state_lock.unlock();
    net_lock.unlock();

    return OK;
}

TError TNetwork::SetupClasses(TNetClass &cls, bool safe) {
    TError error;

    if (!IsHost()) {
        if (&cls == RootClass) {
            auto net_lock = LockNet();
            auto net_state_lock = LockNetState();
            for (auto &dev: Devices)
                if (dev.Uplink) {
                    SetupRxLimit(dev, net_state_lock);
                    SetupPolice(dev);
                }
        }
        return HostNetwork->SetupClasses(cls, safe);
    }

    // HostNetwork

    if (TNetClass::IsDisabled())
        return OK;

    error = TrySetupClasses(cls, safe);
    if (error) {
        L_NET_VERBOSE("Network {} class setup failed: {}", NetName, error);
        StartRepair();
        error = WaitRepair();
        if (error)
            return error;
        error = TrySetupClasses(cls, safe);
        if (error) {
            StartRepair();
            return error;
        }
    }

    return OK;
}

void TNetwork::InitClass(TContainer &ct) {
    auto &cls = ct.NetClass;
    if (TNetClass::IsDisabled()) {
        cls.BaseHandle = 0;
        cls.MetaHandle = 0;
        cls.LeafHandle = 0;
        cls.Fold = nullptr;
        cls.Parent = nullptr;
        return;
    }

    cls.Owner = ct.Id;

    if (!ct.Parent) {
        cls.BaseHandle = TC_HANDLE(ROOT_TC_MAJOR, 1);
        cls.MetaHandle = TC_HANDLE(ROOT_TC_MAJOR, META_TC_MINOR);
        cls.LeafHandle = TC_HANDLE(ROOT_TC_MAJOR, ROOT_TC_MINOR);
        cls.Fold = &cls;
        cls.Parent = nullptr;
    } else if (!(ct.Controllers & CGROUP_NETCLS)) {
        /* Fold into parent */
        cls.BaseHandle = ct.Parent->NetClass.LeafHandle;
        cls.MetaHandle = ct.Parent->NetClass.MetaHandle;
        cls.LeafHandle = ct.Parent->NetClass.LeafHandle;
        cls.Fold = ct.Parent->NetClass.Fold;
        cls.Parent = ct.Parent->NetClass.Parent;
    } else if (ct.Level == 1) {
        cls.BaseHandle = TC_HANDLE(ROOT_TC_MAJOR, META_TC_MINOR);
        cls.MetaHandle = TC_HANDLE(ROOT_TC_MAJOR, META_TC_MINOR | (ct.Id << 4));
        cls.LeafHandle = TC_HANDLE(ROOT_TC_MAJOR, ct.Id << 4);
        cls.Fold = &cls;
        cls.Parent = &ct.Parent->NetClass;
    } else {
        cls.BaseHandle = ct.Parent->NetClass.MetaHandle;
        cls.MetaHandle = ct.Parent->NetClass.MetaHandle;
        cls.LeafHandle = TC_HANDLE(ROOT_TC_MAJOR, ct.Id << 4);
        cls.Fold = &cls;
        cls.Parent = &ct.Parent->NetClass;
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
    if (pos == NetClasses.end())
        return;

    if (cls.Parent && (NetclsSubsystem.HasPriority || cls.OriginNet.get() == this)) {
        for (int cs = 0; cs < NR_TC_CLASSES; cs++)
            cls.Parent->ClassStat[fmt::format("Saved CS{}", cs)] += cls.ClassStat[fmt::format("CS{}", cs)];
    }

    NetClasses.erase(pos);
    cls.Registered--;
}

TError TNetwork::Reconnect() {
    TNamespaceFd netns, cur_ns;
    TError error;

    if (IsHost())
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
    TError error;

    L_NET("Repair network {}", NetName);

    NetError = TError::Queued();

    error = SyncDevices();
    if (error) {
        L_NET("Reconnect network {} netlink after: {}", NetName, error);
        error = Reconnect();
        if (error)
            L_NET("Cannot reconnect network {} netlink: {}", NetName, error);
        error = SyncDevices();
    }

    bool force = false;
    auto state_lock = LockNetState();

    if (error)
        goto out;

retry:
    for (auto &dev: Devices) {
        if (dev.Uplink) {
            SetupRxLimit(dev, state_lock);
            SetupPolice(dev);
        }

        if (!dev.Managed) {
            /*
               Cleanup:
               1) Legacy hfsc setup from containers
               2) Classful qdiscs (highly likely belonged to Porto)
                  on unmanaged at the moment host interfaces
             */
            if (!dev.Uplink &&
                (dev.Qdisc == "hfsc" || dev.Qdisc == "htb") &&
                (ManagedNamespace || config().network().enforce_unmanaged_defaults())) {
                TNlQdisc qdisc(dev.Index, TC_H_ROOT, 0);
                (void)qdisc.Delete(*Nl);
                dev.Qdisc = "";
            }
            continue;
        }

        if (!dev.Prepared || force) {
            error = SetupQueue(dev, force);
            if (error)
                break;
            dev.Prepared = true;
        }

        if (IsHost() && TNetClass::IsDisabled())
            continue;

        for (int cs = 0; cs < NR_TC_CLASSES; cs++) {
            error = SetupClass(dev, DefaultClass, cs);
            if (error)
                break;
        }

        for (auto cls: NetClasses) {
            for (int cs = 0; cs < NR_TC_CLASSES; cs++) {
                error = SetupClass(dev, *cls, cs);
                if (error)
                    break;
            }
            if (error)
                break;
        }
        if (error)
            break;
    }

    if (error) {
        if (!force) {
            force = true;
            goto retry;
        }
    }

out:
    Statistics->NetworkRepairs++;
    if (error) {
        L_ERR("Fatal network {} error: {}", NetName, error);
        Statistics->NetworkProblems++;
    }
    NetError = error;
    NetCv.notify_all();
    state_lock.unlock();
    return error;
}

TError TNetwork::SyncResolvConf() {
    std::string conf;
    TError error;

    if (!config().container().default_resolv_conf().empty()) {
        conf = StringReplaceAll(config().container().default_resolv_conf(), ";", "\n");
    } else {
        error = TPath("/etc/resolv.conf").ReadAll(conf);
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

    L_ACT("Set default resolv_conf:\n{}\n", conf);

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

void TNetwork::UpdateSockDiag() {
    auto error = SockDiag.UpdateData();
    if (error) {
        L_ERR("Cannot update sock diag: {}", error);
        RepairSockDiag();
        return;
    }

    L_DBG("Got sock diag for {} sockets", SockDiag.TcpInfoMapSize());

    auto state_lock = LockNetState();
    std::vector<std::shared_ptr<TContainer>> hostNetUsers;
    auto hostNetwork = HostNetwork;
    if (!hostNetwork)
        return;

    hostNetUsers.reserve(hostNetwork->NetUsers.size());

    for (auto ct: hostNetwork->NetUsers)
        hostNetUsers.emplace_back(ct->shared_from_this());
    state_lock.unlock();

    for (auto ct: hostNetUsers) {
        if (ct->State != EContainerState::Running)
            continue;
        auto freezer = ct->GetCgroup(FreezerSubsystem);

        std::vector<pid_t> pids;
        error = freezer.GetProcesses(pids);

        if (error) {
            L_ERR("Cannot get pids for CT{}:{}: {}", ct->Id, ct->Name, error);
            continue;
        }

        std::unordered_set<ino_t> sockets;
        error = GetSockets(pids, sockets);

        if (error) {
            L_ERR("Cannot get sockets for CT{}:{}: {}", ct->Id, ct->Name, error);
            continue;
        }

        auto prevSocketsStats = std::move(ct->SocketsStats);
        SockDiag.GetSocketsStats(sockets, ct->SocketsStats);

        auto now = GetCurrentTimeMs();

        TNetStat statDiff;

        for (auto &it : ct->SocketsStats) {
            auto itPrev = prevSocketsStats.find(it.first);
            TSockStat prev;
            if (itPrev != prevSocketsStats.end())
                prev = itPrev->second;

            statDiff.TxBytes += (it.second.TxBytes >= prev.TxBytes) ? (it.second.TxBytes - prev.TxBytes) : it.second.TxBytes;
            statDiff.RxBytes += (it.second.RxBytes >= prev.RxBytes) ? (it.second.RxBytes - prev.RxBytes) : it.second.RxBytes;
            statDiff.TxPackets += (it.second.TxPackets >= prev.TxPackets) ? (it.second.TxPackets - prev.TxPackets) : it.second.TxPackets;
            statDiff.RxPackets += (it.second.RxPackets >= prev.RxPackets) ? (it.second.RxPackets - prev.RxPackets) : it.second.RxPackets;
        }

        state_lock.lock();

        ct->SockStat += statDiff;
        ct->SockStat.UpdateTs = now;

        for (auto parent = ct->Parent; parent; parent = parent->Parent) {
            parent->SockStat += statDiff;
            parent->SockStat.UpdateTs = now;
        }
        state_lock.unlock();
    }
}

void TNetwork::RepairSockDiag() {
    SockDiag.Disconnect();
    auto error = SockDiag.Connect();
    if (error)
        L_ERR("Cannot repair sock diag: {}", error);
}

void TNetwork::UpdateProcNetStats(const std::string &basename) {
    TError error;

    auto networks = Networks();
    for (auto net : *networks) {
        auto state_lock = LockNetState();

        if (net->NetUsers.empty() || net->IsHost())
            continue;

        for (auto ct : net->NetUsers) {
            if (ct->State != EContainerState::Meta && ct->State != EContainerState::Running)
                continue;

            if (basename == "netstat")
                error = GetProcNetStats(ct->Task.Pid, net->NetStat, basename);
            else if (basename == "snmp")
                error = GetProcNetStats(ct->Task.Pid, net->NetSnmp, basename);
            if (error) {
                if (errno == ENOENT || errno == ESRCH)
                    continue;
                L_ERR("Cannot get stat for '{}', CT:{} {}", basename, ct->Name, error);
            }

            break;
        }
    }
}

void TNetwork::NetWatchdog() {
    auto now = GetCurrentTimeMs();
    auto LastProxyNeighbour = now;
    auto LastResolvConf = now;
    auto SockDiagDeadline = now;
    TError error;

    const std::string snmp = "snmp";
    const std::string netstat = "netstat";

    SetProcessName("portod-NET");
    while (HostNetwork) {
        now = GetCurrentTimeMs();
        if (SockDiagPeriod && now >= SockDiagDeadline) {
            if (TNetClass::IsDisabled())
                TNetwork::UpdateSockDiag();
            TNetwork::UpdateProcNetStats(netstat);
            TNetwork::UpdateProcNetStats(snmp);
            SockDiagDeadline = now + SockDiagPeriod;
        }
        auto nets = Networks();
        for (auto &net: *nets) {
            auto lock = net->LockNet();
            if (GetCurrentTimeMs() - net->StatTime >= NetWatchdogPeriod) {
                GlobalStatGen++;
                net->SyncStatLocked();
            }
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
        now = GetCurrentTimeMs();
        if (now < SockDiagDeadline)
            NetThreadCv.wait_for(lock, std::chrono::milliseconds(std::min(SockDiagDeadline - now, NetWatchdogPeriod / 2)));
    }
}

void TNetwork::StartRepair() {
    Statistics->NetworkProblems++;
    if (!NetError)
        L_NET_VERBOSE("Start network {} repair", NetName);
    if (!NetError)
        NetError = TError::Queued();
    NetThreadCv.notify_all();
}

TError TNetwork::WaitRepair() {

    if (HostNetwork && !IsHost()) {
        TError error = HostNetwork->WaitRepair();
        if (error)
            return error;
    }

    auto net_lock = LockNet();
    NetCv.wait(net_lock, [this]{return NetError != EError::Queued;});

    return NetError;
}

void TNetwork::InitStat(TNetClass &cls) {
    cls.ClassStat.clear();
    for (auto &dev: Devices) {
        cls.ClassStat[dev.Name] = dev.DeviceStat;
        cls.ClassStat["group " + dev.GroupName] += dev.DeviceStat;
        if (dev.Uplink)
            cls.ClassStat["Uplink"] += dev.DeviceStat;
    }
}

void TNetwork::SyncStatLocked() {
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

    if (IsHost() && TNetClass::IsDisabled())
        return;

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
        for (auto &it: cls->ClassStat) {
            if (!StringStartsWith(it.first, "Saved "))
                it.second.Reset();
        }
        for (int cs = 0; cs < NR_TC_CLASSES; cs++)
            cls->ClassStat[fmt::format("CS{}", cs)] += cls->ClassStat[fmt::format("Saved CS{}", cs)];
    }

    for (auto &dev: Devices) {

        if (!dev.ClassCache)
            continue;

        for (auto cls: NetClasses) {

            if (dev.Owner && dev.Owner != cls->Owner)
                continue;

            if (!NetclsSubsystem.HasPriority && cls->OriginNet.get() != this)
                continue;

            for (int cs = 0; cs < NR_TC_CLASSES; cs++) {
                struct rtnl_class *tc = rtnl_class_get(dev.ClassCache, dev.Index, cls->LeafHandle + cs);
                if (!tc) {
                    L_NET("Missing network {} class {:#x} at {}:{}", NetName, cls->LeafHandle + cs, dev.Index, dev.Name);
                    StartRepair();
                    continue;
                }
                auto cs_name = fmt::format("{} CS{}", dev.Name, cs);
                TNetStat &stat = cls->ClassStat[cs_name];
                stat.TxPackets += rtnl_tc_get_stat(TC_CAST(tc), RTNL_TC_PACKETS);
                stat.TxBytes += rtnl_tc_get_stat(TC_CAST(tc), RTNL_TC_BYTES);
                stat.TxDrops += rtnl_tc_get_stat(TC_CAST(tc), RTNL_TC_DROPS);
                stat.TxOverruns += rtnl_tc_get_stat(TC_CAST(tc), RTNL_TC_OVERLIMITS);
                rtnl_class_put(tc);

                cls->ClassStat[fmt::format("Leaf CS{}", cs)] += stat;

                if (cls->LeafHandle == TC_HANDLE(ROOT_TC_MAJOR, ROOT_TC_MINOR)) {
                    struct rtnl_class *tc = rtnl_class_get(dev.ClassCache, dev.Index, TC_HANDLE(ROOT_TC_MAJOR, DEFAULT_TC_MINOR) + cs);
                    if (!tc) {
                        L_NET("Missing network {} class {:#x} at {}:{}", NetName, TC_HANDLE(ROOT_TC_MAJOR, DEFAULT_TC_MINOR) + cs, dev.Index, dev.Name);
                        StartRepair();
                        continue;
                    }
                    TNetStat &def_stat = cls->ClassStat[fmt::format("Fallback CS{}", cs)];
                    def_stat.TxPackets += rtnl_tc_get_stat(TC_CAST(tc), RTNL_TC_PACKETS);
                    def_stat.TxBytes += rtnl_tc_get_stat(TC_CAST(tc), RTNL_TC_BYTES);
                    def_stat.TxDrops += rtnl_tc_get_stat(TC_CAST(tc), RTNL_TC_DROPS);
                    def_stat.TxOverruns += rtnl_tc_get_stat(TC_CAST(tc), RTNL_TC_OVERLIMITS);
                    rtnl_class_put(tc);
                    stat += def_stat;
                }
            }
        }

        for (auto it = NetClasses.rbegin(); it != NetClasses.rend(); ++it) {
            auto cls = *it;

            if (dev.Owner && dev.Owner != cls->Owner)
                continue;

            if (!NetclsSubsystem.HasPriority && cls->OriginNet.get() != this)
                continue;

            for (int cs = 0; cs < NR_TC_CLASSES; cs++) {
                auto cs_name = fmt::format("{} CS{}", dev.Name, cs);

                TNetStat &stat = cls->ClassStat[cs_name];
                cls->ClassStat[fmt::format("CS{}", cs)] += stat;
                cls->ClassStat[dev.Name] += stat;
                cls->ClassStat["group " + dev.GroupName] += stat;
                if (dev.Uplink)
                    cls->ClassStat["Uplink"] += stat;
                if (cls->Parent && dev.Managed && !dev.Owner)
                    cls->Parent->ClassStat[cs_name] += stat;
            }
        }
    }

    /* Mock statistics if traffic goes into fallback tc class in host. */
    if (!NetclsSubsystem.HasPriority) {
        for (auto cls: NetClasses) {
            if (cls->OriginNet.get() != this) {
                for (auto &dev: cls->OriginNet->Devices) {
                    auto &stat = cls->OriginNet->DeviceStat[dev.Name];
                    for (auto c = cls; c && c->Owner != ROOT_CONTAINER_ID; c = c->Parent) {
                        c->ClassStat[FormatTos(cls->DefaultTos)] += stat;
                        c->ClassStat[dev.Name] += stat;
                        c->ClassStat["group " + dev.GroupName] += stat;
                        if (dev.Uplink)
                            c->ClassStat["Uplink"] += stat;
                    }
                }
            }
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

    error = env.OpenNetwork(ct);
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

    ct.LockStateWrite();
    auto net_state_lock = LockNetState();

    PORTO_ASSERT(!ct.Net);
    ct.Net = env.Net;
    ct.NetClass.OriginNet = env.Net;

    if (env.NetIsolate)
        env.Net->RootClass = &ct.NetClass;

    ct.Net->NetUsers.push_back(&ct);

    if (ct.Controllers & CGROUP_NETCLS) {
        ct.Net->InitStat(ct.NetClass);
        if (!TNetClass::IsDisabled())
            HostNetwork->RegisterClass(ct.NetClass);
    }

    net_state_lock.unlock();

    ct.UnlockState();

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

    ct.LockStateWrite();

    auto net_state_lock = LockNetState();

    auto net = ct.Net;
    ct.Net = nullptr;
    ct.UnlockState();

    if (!net)
        return;

    if ((ct.Controllers & CGROUP_NETCLS) && !TNetClass::IsDisabled())
        HostNetwork->UnregisterClass(ct.NetClass);

    ct.NetClass.Fold = &ct.NetClass;

    if (net->RootClass == &ct.NetClass)
        net->RootClass = nullptr;

    ct.NetClass.OriginNet = nullptr;

    net->NetUsers.remove(&ct);
    bool last = net->NetUsers.empty();

    net_state_lock.unlock();

    if ((ct.Controllers & CGROUP_NETCLS) && !TNetClass::IsDisabled()) {
        auto net_lock = HostNetwork->LockNet();
        for (auto &dev: HostNetwork->Devices) {
            if (!dev.Managed || !dev.Prepared)
                continue;

            for (int cs = 0; cs < NR_TC_CLASSES; cs++) {
                error = HostNetwork->DeleteClass(dev, ct.NetClass, cs);
                if (error)
                    L_NET("Cannot delete network {} class CT{}:{} CS{} {}",
                          HostNetwork->NetName, ct.Id, ct.Name, cs, error);
            }
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
        NetThread->join();
        SockDiag.Disconnect();
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

    if (env.SaveIp) {
        ct.LockStateWrite();
        env.FormatIp(ct.IpList);
        ct.UnlockState();
    }
}

TError TNetwork::RestoreNetwork(TContainer &ct) {
    std::shared_ptr<TNetwork> net;
    TNamespaceFd netNs;
    TError error;
    TNetEnv env;

    L_ACT("Restore network {}", ct.Name);

    if (ct.NetInherit) {
        net = ct.Parent->Net;
    } else if (ct.Task.Pid) {
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

    ct.LockStateWrite();

    auto net_state_lock = LockNetState();

    if (ct.Controllers & CGROUP_NETCLS) {
        net->InitStat(ct.NetClass);
        if (!TNetClass::IsDisabled())
            HostNetwork->RegisterClass(ct.NetClass);
    }

    ct.Net = net;
    ct.NetClass.OriginNet = net;

    if (env.NetIsolate) {
        net->RootClass = &ct.NetClass;
        net->ManagedNamespace = true;
        net->NetName = ct.Name;
    }

    net->NetUsers.push_back(&ct);

    net_state_lock.unlock();

    ct.UnlockState();

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
    NetNone = false;
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
            NetNone = true;
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
            if (name.find("..") != std::string::npos)
                return TError(EError::Permission, "'..' not allowed in net namespace path");
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
            size_t nameIndex = 1;
            size_t masterIndex = 2;
            if (settings.size() > 1 && StringTrim(settings[1]) == "extra_routes") {
                dev.EnableExtraRoutes = true;
                nameIndex = 2;
                masterIndex = 3;
            }
            if (settings.size() > nameIndex)
                dev.Name = StringTrim(settings[nameIndex]);
            if (settings.size() > masterIndex)
                dev.Master = StringTrim(settings[masterIndex]);
            if (config().network().l3_default_mtu() > 0)
                dev.Mtu = config().network().l3_default_mtu();
            if (config().network().l3_default_ipv4_mtu() > 0)
                dev.GateMtu4 = config().network().l3_default_ipv4_mtu();
            if (config().network().l3_default_ipv6_mtu() > 0)
                dev.GateMtu6 = config().network().l3_default_ipv6_mtu();
        } else if (type == "NAT") {
            dev.Type = "L3";
            dev.Mode = "NAT";
            dev.Name = "eth0";
            if (settings.size() > 1)
                dev.Name = StringTrim(settings[1]);
            if (config().network().l3_default_mtu() > 0)
                dev.Mtu = config().network().l3_default_mtu();
            if (config().network().l3_default_ipv4_mtu() > 0)
                dev.GateMtu4 = config().network().l3_default_ipv4_mtu();
            if (config().network().l3_default_ipv6_mtu() > 0)
                dev.GateMtu6 = config().network().l3_default_ipv6_mtu();
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
            dev.Tap.Uid = TaskCred.GetUid();
            dev.Tap.Gid = TaskCred.GetGid();
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
        } else if (type == "ECN") {
            if (settings.size() == 1) {
                EnableECN = true;
            } else if (settings.size() == 2) {
                bool found = false;
                for (auto &dev: Devices) {
                    if (dev.Name == settings[1]) {
                        dev.EnableECN = true;
                        found = true;
                        break;
                    }
                }
                if (!found)
                    return TError(EError::InvalidValue, "Link not found: " + settings[1]);
            } else {
                return TError(EError::InvalidValue, "Invalid " + line);
            }
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
        if (ct->IpPolicy == "any")
            continue;

        if (ct->IpPolicy == "none")
            return TError(EError::Permission, "Parent container " + ct->Name + " forbid IP changing");

        if (!L3Only)
            return TError(EError::Permission, "Parent container " + ct->Name + " allows only L3 network");

        for (auto &dev: Devices) {
            for (auto &ip: dev.Ip) {
                bool allow = false;
                for (auto &line: ct->IpLimit) {
                    TNlAddr mask;
                    if (mask.Parse(AF_UNSPEC, line[0]) || mask.Family() != ip.Family())
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
            if (dev.Name == settings[0]) {
                if (ip.Family() == AF_INET)
                    dev.Gate4 = ip;
                if (ip.Family() == AF_INET6)
                    dev.Gate6 = ip;
            }
    }
    return OK;
}

TError TNetEnv::ConfigureL3(TNetDeviceConfig &dev) {
    auto lock = HostNetwork->LockNet();
    std::string peerName = HostNetwork->NewDeviceName("L3-");
    auto parentNl = HostNetwork->GetNl();
    auto Nl = Net->GetNl();
    TNlLink peer(parentNl, peerName);
    TError error;

    if (dev.Mode == "NAT" && dev.Ip.empty()) {
        error = HostNetwork->GetNatAddress(dev.Ip);
        if (error)
            return error;
        SaveIp = true;
    }

    error = HostNetwork->GetL3Gate(dev);
    if (error)
        return error;

    std::string ipStr;
    for (auto &ip : dev.Ip)
        ipStr += fmt::format("{}={} ", ip.Family() == AF_INET ? "ip4" : "ip6", ip.Format());

    L_NET("Setup L3 device {} peer={} mtu={} group={} master={} {}gw4={} mtu4={} gw6={} mtu6={}",
            dev.Name, peerName, dev.Mtu, TNetwork::DeviceGroupName(dev.Group),
            dev.Master, ipStr,
            dev.Gate4.Format(), dev.GateMtu4,
            dev.Gate6.Format(), dev.GateMtu6);

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

    if (!dev.Gate4.IsEmpty()) {
        error = Nl->PermanentNeighbour(link.GetIndex(), dev.Gate4, peerAddr, true);
        if (error)
            return error;
        error = link.AddDirectRoute(dev.Gate4, EnableECN || dev.EnableECN);
        if (error)
            return error;
    }

    if (!dev.Gate6.IsEmpty()) {
        error = Nl->PermanentNeighbour(link.GetIndex(), dev.Gate6, peerAddr, true);
        if (error)
            return error;
        error = link.AddDirectRoute(dev.Gate6, EnableECN || dev.EnableECN);
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

    Net->HostPeerIndex = peer.GetIndex();

    return OK;
}

TError TNetEnv::CreateTap(TNetDeviceConfig &dev) {
    TError error;

    if (Net != HostNetwork)
        return TError(EError::NotSupported, "Tap only for host network");

    error = Net->GetL3Gate(dev);
    if (error)
        return error;

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

    if (!dev.Gate4.IsEmpty()) {
        error = Nl->ProxyNeighbour(tapdev.GetIndex(), dev.Gate4, true);
        if (error)
            return error;
    }

    if (!dev.Gate6.IsEmpty()) {
        error = Nl->ProxyNeighbour(tapdev.GetIndex(), dev.Gate6, true);
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
                                   config().network().ipip6_ttl(),
                                   config().network().ipip6_tx_queues());
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

        if (NetUp || !dev.Ip.empty() || dev.Autoconf ||
                !dev.Gate4.IsEmpty() || !dev.Gate6.IsEmpty()) {
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

        if (!dev.Gate4.IsEmpty()) {
            error = link.SetDefaultGw(dev.Gate4, EnableECN || dev.EnableECN, dev.GateMtu4);
            if (error)
                return error;
            DefaultRoute = false;
        }

        if (!dev.Gate6.IsEmpty()) {
            error = link.SetDefaultGw(dev.Gate6, EnableECN || dev.EnableECN, dev.GateMtu6);
            if (error)
                return error;
        }

        if (dev.Type == "ipip6" && DefaultRoute) {
            TNlAddr ip;
            ip.Parse(AF_INET, "default");
            error = link.AddDirectRoute(ip, dev.EnableECN);
            if (error)
                return error;
        }
        if (dev.EnableExtraRoutes  && dev.Type == "L3") {
            if (!dev.Gate6.IsEmpty()) {
                for (const auto &extraRoute : config().network().extra_routes()) {
                    error = AddRoute(extraRoute.dst(), Net->DeviceIndex(dev.Name), dev.Gate6.Addr, extraRoute.mtu(),
                            extraRoute.has_advmss() ? extraRoute.advmss() : 0);
                    if (error)
                        return error;
                }
            }
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

TError TNetEnv::AddRoute(const std::string &dsc, int index, struct nl_addr *via, int mtu, int advmss) {
    struct nl_addr *dst = NULL;
    struct rtnl_route *route;
    struct rtnl_nexthop *nh;
    TError error;
    int ret;

    route = rtnl_route_alloc();
    if (!route)
        return TError(EError::Unknown, "Cannot allocate route");

    nh = rtnl_route_nh_alloc();
    if (!nh) {
        error = TError(EError::Unknown, "Cannot allocate nh");
        goto err;
    }

    ret = nl_addr_parse(dsc.c_str(), AF_INET6, &dst);
    if (ret) {
        error = TNLinkSockDiag::Error(ret, "Cannot parse dst");
        goto err;
    }

    rtnl_route_nh_set_gateway(nh, via);
    rtnl_route_nh_set_ifindex(nh, index);
    rtnl_route_add_nexthop(route, nh);

    if (mtu) {
        ret = rtnl_route_set_metric(route, RTAX_MTU, mtu);
        if (ret < 0) {
            error = TNLinkSockDiag::Error(ret, "Cannot set mtu");
            goto err;
        }
    }

    if (advmss) {
        ret = rtnl_route_set_metric(route, RTAX_ADVMSS, advmss);
        if (ret < 0) {
            error = TNLinkSockDiag::Error(ret, "Cannot set advmss");
            goto err;
        }
    }

    ret = rtnl_route_set_dst(route, dst);
    if (ret < 0) {
        error = TNLinkSockDiag::Error(ret, "Cannot set dst");
        goto err;
    }

    ret = rtnl_route_add(Net->GetSock(), route, NLM_F_CREATE | NLM_F_REPLACE);
    if (ret < 0) {
        error = TNLinkSockDiag::Error(ret, "Cannot add route");
        goto err;
    }

    error = OK;
err:
    if (dst)
        nl_addr_put(dst);
    rtnl_route_put(route);

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
    return TError(EError::InvalidValue, "Cannot open network of container {}", ct.Name);
}

TError TNetEnv::OpenNetwork(TContainer &ct) {
    TError error;

    /* Share L3 network with same config and ip */
    if (config().network().l3_migration_hack() &&
            NetIsolate && L3Only && ct.IpList.size()) {
        auto lock = LockContainers();
        for (auto &it: Containers) {
            auto c = it.second;
            if (c->Net && c->Task.Pid &&
                    c->NetProp == ct.NetProp &&
                    c->IpList == ct.IpList) {
                lock.unlock();
                L_NET("Share network {}", c->Name);
                return Open(*c);
            }
        }
    }

    if (!Parent) {
        error = TNetwork::Open("/proc/thread-self/ns/net", NetNs, Net, true);
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

        auto error = SockDiag.Connect();
        if (error)
            return TError("Cannot connect sock diag: {}", error);

        NetThread = std::unique_ptr<std::thread>(NewThread(&TNetwork::NetWatchdog));

        return OK;
    }

    if (NetInherit) {
        auto p = Parent.get();
        while (p->Parent && !p->Task.Pid && p->Net == p->Parent->Net)
            p = p->Parent.get();
        return Open(*p);
    }

    if (NetIsolate) {
        error = TNetwork::New(NetNs, Net, ct.DockerMode ? ct.Task.Pid : 0);
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
